// blob server — authoritative, headless.
//
// No SFML, no window, no GPU. If this file ever needs a display it means
// something belongs in client/ instead.

#include "config.hpp"
#include "session.hpp"
#include "snapshot_encode.hpp"
#include "tick_loop.hpp"

#include <blob/math/vec2.hpp>
#include <blob/net/protocol.hpp>
#include <blob/net/quantize.hpp>
#include <blob/sim/world.hpp>

#include <enet/enet.h>

#include <array>
#include <atomic>
#include <charconv>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

std::atomic_bool g_running{true};

extern "C" void on_signal(int) { g_running.store(false, std::memory_order_relaxed); }

/// Probed in the working directory when --config is not given; a missing file
/// here is normal (run on defaults), unlike an explicitly named one.
constexpr const char* default_config_path = "blob-server.cfg";

/// RAII for the ENet global. Every early return below is therefore safe.
struct EnetGuard {
    EnetGuard() : ok{enet_initialize() == 0} {}
    ~EnetGuard() { if (ok) { enet_deinitialize(); } }
    EnetGuard(const EnetGuard&) = delete;
    EnetGuard& operator=(const EnetGuard&) = delete;
    bool ok;
};

struct CliArgs {
    const char*                  config_path = nullptr;   ///< nullptr = probe the default path
    std::optional<std::uint16_t> port;                    ///< CLI beats file beats default
    bool                         ok = true;
};

CliArgs parse_cli(int argc, char** argv)
{
    CliArgs args{};
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--config") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "--config needs a path\n");
                args.ok = false;
                return args;
            }
            args.config_path = argv[++i];
        } else if (!args.port) {
            std::uint16_t port{};
            const char* const last  = arg.data() + arg.size();
            const auto [ptr, ec]    = std::from_chars(arg.data(), last, port);
            if (ec != std::errc{} || ptr != last) {
                std::fprintf(stderr, "not a port: '%s'\nusage: blob-server [--config <path>] [port]\n",
                             argv[i]);
                args.ok = false;
                return args;
            }
            args.port = port;
        } else {
            std::fprintf(stderr, "unexpected argument '%s'\nusage: blob-server [--config <path>] [port]\n",
                         argv[i]);
            args.ok = false;
            return args;
        }
    }
    return args;
}

/// Defaults < config file < CLI port. Returns nullopt after printing why:
/// a typo'd config must fail loudly, never run on defaults.
std::optional<blob::server::ServerConfig> resolve_config(const CliArgs& args)
{
    const bool  explicit_path = args.config_path != nullptr;
    const char* path          = explicit_path ? args.config_path : default_config_path;

    blob::server::ServerConfig      config{};
    const blob::server::LoadResult  loaded = blob::server::load_config_file(path);
    switch (loaded.status) {
    case blob::server::LoadStatus::Loaded:
        if (!loaded.parsed.errors.empty()) {
            for (const blob::server::ConfigError& e : loaded.parsed.errors) {
                std::fprintf(stderr, "%s:%d: %s\n", path, e.line, e.message.c_str());
            }
            return std::nullopt;
        }
        config = loaded.parsed.config;
        std::printf("config loaded from %s\n", path);
        break;
    case blob::server::LoadStatus::Absent:
        if (explicit_path) {
            std::fprintf(stderr, "config file not found: %s\n", path);
            return std::nullopt;
        }
        break;   // no file at the default path: silent defaults, by design
    case blob::server::LoadStatus::Unreadable:
        // Even at the default path: a file that exists but cannot be read is
        // never "no config" — running on defaults here would be exactly the
        // silent fallback this feature exists to prevent.
        std::fprintf(stderr, "config file unreadable: %s\n", path);
        return std::nullopt;
    }

    if (args.port) {
        config.port = *args.port;
    }
    return config;
}

// ---------------------------------------------------------------------------
// Session <-> peer plumbing.
// ---------------------------------------------------------------------------

/// The idiomatic ENet peer tag: peer->data is a void* for exactly this, and
/// the PlayerId is small enough to live *in* the pointer value rather than
/// behind it — no allocation, nothing to free on disconnect.
void tag_peer(ENetPeer& peer, blob::sim::PlayerId id) noexcept
{
    peer.data = reinterpret_cast<void*>(static_cast<std::uintptr_t>(id));
}

[[nodiscard]] blob::sim::PlayerId peer_id(const ENetPeer& peer) noexcept
{
    return static_cast<blob::sim::PlayerId>(reinterpret_cast<std::uintptr_t>(peer.data));
}

struct ServerState {
    blob::sim::World                         world;
    std::vector<blob::server::PlayerSession> sessions;
    blob::sim::PlayerId                      next_player_id{1};
};

void handle_connect(ServerState& state, ENetPeer& peer)
{
    // Monotonic, never reused within a run (0 stays the "no owner" sentinel;
    // a u16 wrap after 65535 connects would recycle, which the wire format
    // cannot avoid anyway).
    const blob::sim::PlayerId id = state.next_player_id++;
    if (state.next_player_id == 0) {
        state.next_player_id = 1;
    }
    tag_peer(peer, id);
    blob::server::add_session(state.sessions, id);

    // Placeholder spawn: world centre plus a small deterministic id-based
    // offset so two players never stack exactly. M3's spawn_player (safe
    // placement, respawn) replaces this call site.
    const float extent = state.world.tuning.world_extent;
    const float offset = 64.0f * static_cast<float>(id % 16u);
    blob::sim::spawn(state.world, blob::sim::EntityKind::Cell,
                     {extent * 0.5f + offset, extent * 0.5f + offset}, 10.0f, id);

    // Welcome is session state: reliable Control channel (invariant 5).
    std::array<std::byte, 8> buffer{};
    blob::net::ByteWriter    writer{.buffer = buffer};
    blob::net::write_welcome(writer, {
        .version      = blob::net::protocol_version,
        .player_id    = id,
        .world_extent = static_cast<std::uint16_t>(state.world.tuning.world_extent),
        .tick_rate    = static_cast<std::uint8_t>(state.world.tuning.tick_rate),
    });
    const std::span<const std::byte> bytes = blob::net::written(writer);
    ENetPacket* packet = enet_packet_create(bytes.data(), bytes.size(), ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(&peer, static_cast<enet_uint8>(blob::net::Channel::Control), packet);

    std::printf("player %u connected (%x:%u)\n", id, peer.address.host, peer.address.port);
}

void handle_receive(ServerState& state, ENetPeer& peer, const ENetPacket& packet)
{
    // Only Input arrives today; read_input rejects everything else (wrong
    // MessageId, truncation) and malformed wire data is dropped, never fatal.
    blob::net::ByteReader reader{
        .buffer = {reinterpret_cast<const std::byte*>(packet.data), packet.dataLength}};
    const std::optional<blob::net::InputCommand> cmd = blob::net::read_input(reader);
    if (!cmd) {
        return;
    }
    blob::server::PlayerSession* session =
        blob::server::find_session(state.sessions, peer_id(peer));
    if (session == nullptr) {
        return;   // no session for this peer (races a disconnect) — ignore
    }

    // Serial-number guard: the input stream is unreliable and unordered, so
    // accept only strictly newer sequences — a reordered or duplicated
    // datagram must not roll intent back to an older cursor.
    if (session->received_input && !blob::server::sequence_newer(cmd->sequence, session->last_sequence)) {
        return;
    }
    session->received_input = true;
    session->last_sequence  = cmd->sequence;

    // Dequantize, then normalize UNCONDITIONALLY: a hostile client sending
    // (127,127) would otherwise move sqrt(2) faster than a legal one. The
    // server is authoritative (invariant 2) and sanitizes intent at the door;
    // honest sub-unit vectors pass through normalized() unchanged in
    // direction, and {0,0} ("hold still") stays {0,0}.
    const blob::math::Vec2 direction = blob::math::normalized({
        blob::net::dequantize_direction(cmd->dir_x),
        blob::net::dequantize_direction(cmd->dir_y)});

    blob::sim::apply_intent(state.world, {.player    = session->id,
                                          .direction = direction,
                                          .split     = cmd->split,
                                          .eject     = cmd->eject});
}

void handle_disconnect(ServerState& state, ENetPeer& peer)
{
    const blob::sim::PlayerId id = peer_id(peer);
    blob::server::remove_session(state.sessions, id);
    // Placeholder despawn: drop the player's entities and their standing
    // intent. M3's despawn_player replaces both erase_ifs.
    std::erase_if(state.world.entities,
                  [id](const blob::sim::Entity& e) { return e.owner == id; });
    std::erase_if(state.world.intents,
                  [id](const blob::sim::PlayerIntent& i) { return i.player == id; });
    peer.data = nullptr;
    std::printf("player %u disconnected\n", id);
}

/// One switch for both service sites (the drain loop and the sleep below) so
/// a connect or input arriving mid-sleep is handled identically, not dropped.
void handle_event(ServerState& state, const ENetEvent& event)
{
    switch (event.type) {
    case ENET_EVENT_TYPE_CONNECT:
        handle_connect(state, *event.peer);
        break;
    case ENET_EVENT_TYPE_RECEIVE:
        handle_receive(state, *event.peer, *event.packet);
        enet_packet_destroy(event.packet);   // always, decoded or dropped
        break;
    case ENET_EVENT_TYPE_DISCONNECT:
        handle_disconnect(state, *event.peer);
        break;
    default:
        break;
    }
}

} // namespace

int main(int argc, char** argv)
{
    const CliArgs args = parse_cli(argc, argv);
    if (!args.ok) {
        return 1;
    }
    const std::optional<blob::server::ServerConfig> config = resolve_config(args);
    if (!config) {
        return 1;
    }

    EnetGuard enet;
    if (!enet.ok) {
        std::fprintf(stderr, "enet_initialize failed\n");
        return 1;
    }

    ENetAddress address{};
    address.host = ENET_HOST_ANY;
    address.port = config->port;

    ENetHost* host = enet_host_create(&address,
                                      config->max_clients,
                                      static_cast<std::size_t>(blob::net::Channel::Count),
                                      0,   // no incoming bandwidth cap
                                      0);  // no outgoing bandwidth cap
    if (host == nullptr) {
        std::fprintf(stderr, "enet_host_create failed on port %u\n", config->port);
        return 1;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    ServerState state{};
    state.world.tuning = config->tuning;
    auto loop = blob::server::make_tick_loop(state.world.tuning.tick_rate);

    std::vector<blob::net::EntityRecord> records;   // reused across iterations

    std::printf("blob-server listening on udp/%u, %d Hz, %zu peer slots\n",
                config->port, state.world.tuning.tick_rate, config->max_clients);

    while (g_running.load(std::memory_order_relaxed)) {
        // Drain the socket first: input that arrived since the last tick should
        // be applied to *this* tick, not the next one.
        ENetEvent event{};
        while (enet_host_service(host, &event, 0) > 0) {
            handle_event(state, event);
        }

        int ticks_this_iteration = 0;
        for (int i = blob::server::pump(loop); i > 0; --i) {
            blob::sim::step(state.world, blob::sim::tick_dt(state.world.tuning));
            ++ticks_this_iteration;
        }

        // Broadcast once per outer iteration, never per catch-up tick: nobody
        // renders the intermediate states of a catch-up burst, so only the
        // final one is worth bandwidth.
        if (ticks_this_iteration > 0 && !state.sessions.empty()) {
            blob::server::collect_records(state.world, records);
            const auto tick = static_cast<std::uint32_t>(state.world.tick);
            blob::server::for_each_chunk(
                records, [&](std::span<const blob::net::EntityRecord> chunk) {
                    std::array<std::byte, blob::net::snapshot_soft_mtu> buffer;
                    blob::net::ByteWriter writer{.buffer = buffer};
                    blob::net::write_snapshot(writer, tick, chunk);
                    const std::span<const std::byte> bytes = blob::net::written(writer);
                    // Flags 0 = unreliable sequenced: ENet drops a chunk that
                    // arrives after a newer tick's, which is benign — records
                    // are absolute state, already superseded (invariant 5).
                    ENetPacket* packet = enet_packet_create(bytes.data(), bytes.size(), 0);
                    enet_host_broadcast(host, static_cast<enet_uint8>(blob::net::Channel::Snapshot),
                                        packet);
                });
        }

        // Sleep out the remainder inside ENet so a packet can wake us early —
        // and handle whatever woke us, same as the drain loop.
        if (const auto idle = blob::server::time_to_next_tick(loop); idle.count() > 0) {
            if (enet_host_service(host, &event, static_cast<enet_uint32>(idle.count())) > 0) {
                handle_event(state, event);
            }
        }
    }

    std::printf("\nshutting down after %llu ticks (%llu dropped)\n",
                static_cast<unsigned long long>(loop.ticks_run),
                static_cast<unsigned long long>(loop.ticks_dropped));

    enet_host_destroy(host);
    return 0;
}
