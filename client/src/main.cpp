// blob client — SFML 3 window, input capture, snapshot rendering.
//
// Rendering stays deliberately boring: raw latest-snapshot circles and a
// camera, redrawn at whatever rate snapshots arrive (20 Hz stutter accepted).
// The snapshot buffer + interpolation is the next client iteration, and it
// should land in core as pure logic — nothing here may grow gameplay.

#include <blob/math/vec2.hpp>
#include <blob/net/protocol.hpp>
#include <blob/net/quantize.hpp>
#include <blob/sim/tuning.hpp>
#include <blob/sim/world.hpp>

#include <enet/enet.h>

#include <SFML/Graphics.hpp>

#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

using steady_clock = std::chrono::steady_clock;

struct EnetGuard {
    EnetGuard() : ok{enet_initialize() == 0} {}
    ~EnetGuard() { if (ok) { enet_deinitialize(); } }
    EnetGuard(const EnetGuard&) = delete;
    EnetGuard& operator=(const EnetGuard&) = delete;
    bool ok;
};

/// The world as of the newest complete-enough tick we have seen. Chunks are
/// self-contained and absolute, so assembly is: newer tick replaces the set,
/// same tick appends (multi-chunk snapshot), older tick is stale — drop it.
struct SnapshotView {
    std::uint32_t                        tick{};
    std::vector<blob::net::EntityRecord> entities;
    bool                                 any{};
};

void apply_chunk(SnapshotView& view, const blob::net::SnapshotHeader& header,
                 std::span<const blob::net::EntityRecord> records)
{
    if (!view.any || header.tick > view.tick) {
        view.any  = true;
        view.tick = header.tick;
        view.entities.assign(records.begin(), records.end());
    } else if (header.tick == view.tick) {
        view.entities.insert(view.entities.end(), records.begin(), records.end());
    }
    // header.tick < view.tick: a chunk that lost the race to a newer tick —
    // benign on the unreliable channel, the newer state already superseded it.
}

/// The refusal reasons, spelled for a human reading the console.
[[nodiscard]] const char* goodbye_reason_name(blob::net::GoodbyeReason reason) noexcept
{
    switch (reason) {
    case blob::net::GoodbyeReason::VersionMismatch: return "protocol version mismatch";
    case blob::net::GoodbyeReason::ServerFull:      return "server is full";
    case blob::net::GoodbyeReason::Shutdown:        return "server shutting down";
    }
    return "unknown";   // unreachable: read_goodbye rejects unknown reasons
}

/// Blocks (servicing ENet) until the Welcome arrives on Control, the server
/// rejects us, or the deadline passes. Sends the Hello the moment the
/// transport comes up — the server will not spawn us, let alone Welcome us,
/// until it has checked our version. Returns nullopt after printing why.
std::optional<blob::net::WelcomePayload> await_welcome(ENetHost* host, ENetPeer* server,
                                                       std::string_view nickname)
{
    const auto deadline = steady_clock::now() + std::chrono::seconds{5};
    while (steady_clock::now() < deadline) {
        ENetEvent event{};
        if (enet_host_service(host, &event, 50) <= 0) {
            continue;
        }
        switch (event.type) {
        case ENET_EVENT_TYPE_CONNECT: {
            // Transport is up: introduce ourselves. Hello is session state —
            // reliable Control (invariant 5) — and carries our version so the
            // server can refuse a mismatch before spending anything on us.
            std::array<std::byte,
                       blob::net::hello_header_bytes + blob::net::max_hello_name_bytes>
                                  buffer{};
            blob::net::ByteWriter writer{.buffer = buffer};
            blob::net::write_hello(writer, blob::net::protocol_version, nickname);
            const std::span<const std::byte> bytes = blob::net::written(writer);
            ENetPacket* packet =
                enet_packet_create(bytes.data(), bytes.size(), ENET_PACKET_FLAG_RELIABLE);
            enet_peer_send(server, static_cast<enet_uint8>(blob::net::Channel::Control), packet);
            break;   // now the Welcome (or a Goodbye refusing us) follows
        }
        case ENET_EVENT_TYPE_RECEIVE: {
            const std::span<const std::byte> data{
                reinterpret_cast<const std::byte*>(event.packet->data),
                event.packet->dataLength};
            blob::net::ByteReader reader{.buffer = data};
            const std::optional<blob::net::WelcomePayload> welcome =
                blob::net::read_welcome(reader);
            if (welcome) {
                enet_packet_destroy(event.packet);
                return welcome;
            }
            // A Goodbye instead of a Welcome is the server refusing us — say
            // why rather than letting the disconnect look like a dead server.
            blob::net::ByteReader goodbye_reader{.buffer = data};
            const std::optional<blob::net::GoodbyeReason> goodbye =
                blob::net::read_goodbye(goodbye_reader);
            enet_packet_destroy(event.packet);
            if (goodbye) {
                std::fprintf(stderr, "server refused us: %s\n", goodbye_reason_name(*goodbye));
                return std::nullopt;
            }
            break;   // e.g. a snapshot chunk outracing the Welcome — keep waiting
        }
        case ENET_EVENT_TYPE_DISCONNECT:
            std::fprintf(stderr, "server closed the connection during the handshake\n");
            return std::nullopt;
        default:
            break;
        }
    }
    std::fprintf(stderr, "timed out waiting for the server (5 s)\n");
    return std::nullopt;
}

[[nodiscard]] sf::Color colour_for(const blob::net::EntityRecord& record,
                                   std::uint16_t player_id) noexcept
{
    switch (static_cast<blob::sim::EntityKind>(record.kind)) {
    case blob::sim::EntityKind::Cell:
        return record.owner == player_id ? sf::Color{120, 220, 130}    // mine
                                         : sf::Color{90, 160, 240};    // theirs
    case blob::sim::EntityKind::Pellet:      return sf::Color{170, 170, 175};
    case blob::sim::EntityKind::Virus:       return sf::Color{200, 90, 200};
    case blob::sim::EntityKind::EjectedMass: return sf::Color{235, 200, 90};
    }
    return sf::Color::White;   // unreachable: read_snapshot rejects bad kinds
}

} // namespace

int main(int argc, char** argv)
{
    const char*   host_name = argc > 1 ? argv[1] : "127.0.0.1";
    std::uint16_t port      = 7777;
    if (argc > 2) {
        const std::string_view arg  = argv[2];
        const char* const      last = arg.data() + arg.size();
        const auto [ptr, ec]        = std::from_chars(arg.data(), last, port);
        if (ec != std::errc{} || ptr != last) {
            std::fprintf(stderr, "not a port: '%s'\nusage: blob-client [host] [port] [nickname]\n",
                         argv[2]);
            return 1;
        }
    }
    // Nickname for the Hello, truncated to the wire's byte cap here at the
    // door — write_hello treats a longer name as misuse, and a UI decision
    // like truncation belongs to the UI, not the codec. (Cutting mid-UTF-8
    // sequence is possible and harmless: names are display-only.)
    std::string_view nickname = argc > 3 ? argv[3] : "player";
    if (nickname.size() > blob::net::max_hello_name_bytes) {
        nickname = nickname.substr(0, blob::net::max_hello_name_bytes);
    }

    EnetGuard enet;
    if (!enet.ok) {
        std::fprintf(stderr, "enet_initialize failed\n");
        return 1;
    }

    ENetHost* client = enet_host_create(nullptr, 1,
                                        static_cast<std::size_t>(blob::net::Channel::Count),
                                        0, 0);
    if (client == nullptr) {
        std::fprintf(stderr, "enet_host_create failed\n");
        return 1;
    }

    ENetAddress address{};
    if (enet_address_set_host(&address, host_name) != 0) {
        std::fprintf(stderr, "cannot resolve host '%s'\n", host_name);
        enet_host_destroy(client);
        return 1;
    }
    address.port = port;

    ENetPeer* server = enet_host_connect(client, &address,
                                         static_cast<std::size_t>(blob::net::Channel::Count), 0);
    if (server == nullptr) {
        std::fprintf(stderr, "enet_host_connect failed\n");
        enet_host_destroy(client);
        return 1;
    }
    std::printf("connecting to %s:%u...\n", host_name, port);

    const std::optional<blob::net::WelcomePayload> welcome =
        await_welcome(client, server, nickname);
    if (!welcome) {
        enet_peer_reset(server);
        enet_host_destroy(client);
        return 1;
    }
    // Belt and braces: the server refuses a mismatch first (our Hello carries
    // the version), but a Welcome that slipped through still gets checked.
    if (welcome->version != blob::net::protocol_version) {
        std::fprintf(stderr, "protocol mismatch: server speaks v%u, this client speaks v%u\n",
                     welcome->version, blob::net::protocol_version);
        enet_peer_disconnect(server, 0);
        enet_host_flush(client);
        enet_host_destroy(client);
        return 1;
    }

    // Everything spatial below uses the extent FROM THE WELCOME — the server
    // may run a config-overridden world, and dequantizing against a local
    // constant would warp every position.
    const std::uint16_t player_id     = welcome->player_id;
    const float         world_extent  = static_cast<float>(welcome->world_extent);
    const float         send_interval = 1.0f / static_cast<float>(std::max<int>(welcome->tick_rate, 1));
    std::printf("welcome: player %u, extent %u, %u Hz\n", player_id, welcome->world_extent,
                welcome->tick_rate);

    sf::RenderWindow window{sf::VideoMode{{1280u, 720u}},
                            "blob — player " + std::to_string(player_id)};
    window.setVerticalSyncEnabled(true);
    // One KeyPressed per physical press: with auto-repeat on, a held Space
    // would machine-gun split flags at the OS repeat rate (M4 actions are
    // edges, and the edge is the key going down).
    window.setKeyRepeatEnabled(false);

    SnapshotView view{};
    sf::CircleShape shape;   // one shape reused for every entity drawn
    // Camera: fixed world-unit size (no zoom curve until interest management),
    // centred on our first cell, holding its last position while we have none.
    blob::math::Vec2 camera_centre{world_extent * 0.5f, world_extent * 0.5f};
    constexpr sf::Vector2f camera_size{2560.0f, 1440.0f};

    sf::Clock     frame_clock;
    float         send_accumulator = 0.0f;
    std::uint16_t sequence         = 0;
    bool          server_gone      = false;
    // M4 action keys (Space = split, W = eject — the genre's convention)
    // queue here between sends; the next InputCommand carries them, then
    // they re-arm. The server latches per press, so one press is one action.
    bool          split_queued     = false;
    bool          eject_queued     = false;

    while (window.isOpen()) {
        while (const std::optional event = window.pollEvent()) {
            if (event->is<sf::Event::Closed>()) {
                window.close();
            } else if (const auto* key = event->getIf<sf::Event::KeyPressed>()) {
                if (key->code == sf::Keyboard::Key::Escape) {
                    window.close();
                } else if (key->code == sf::Keyboard::Key::Space) {
                    split_queued = true;
                } else if (key->code == sf::Keyboard::Key::W) {
                    eject_queued = true;
                }
            }
        }

        // Drain the socket: adopt the newest snapshot state.
        ENetEvent event{};
        while (enet_host_service(client, &event, 0) > 0) {
            switch (event.type) {
            case ENET_EVENT_TYPE_RECEIVE: {
                const std::span<const std::byte> data{
                    reinterpret_cast<const std::byte*>(event.packet->data),
                    event.packet->dataLength};
                blob::net::ByteReader reader{.buffer = data};
                std::array<blob::net::EntityRecord, blob::net::max_entities_per_chunk> chunk{};
                if (const std::optional<blob::net::SnapshotHeader> header =
                        blob::net::read_snapshot(reader, chunk)) {
                    apply_chunk(view, *header, std::span{chunk}.first(header->count));
                } else {
                    // The only other server->client message mid-game is a
                    // Goodbye on Control (e.g. Shutdown): print why and leave
                    // cleanly instead of waiting out the dead connection.
                    blob::net::ByteReader goodbye_reader{.buffer = data};
                    if (const std::optional<blob::net::GoodbyeReason> goodbye =
                            blob::net::read_goodbye(goodbye_reader)) {
                        std::fprintf(stderr, "server said goodbye: %s\n",
                                     goodbye_reason_name(*goodbye));
                        server_gone = true;   // the server is done with us
                        window.close();
                    }
                }
                enet_packet_destroy(event.packet);   // always — decoded or not
                break;
            }
            case ENET_EVENT_TYPE_DISCONNECT:
                std::fprintf(stderr, "server closed the connection\n");
                server_gone = true;
                window.close();
                break;
            default:
                break;
            }
        }
        if (server_gone) {
            break;
        }

        // Input -> intent at the server's tick rate (from the Welcome — the
        // rate is server-authoritative, never assumed). The client sends only
        // intent, never a position (invariant 2).
        send_accumulator += frame_clock.restart().asSeconds();
        if (send_accumulator >= send_interval) {
            send_accumulator -= send_interval;
            if (send_accumulator > send_interval) {
                send_accumulator = 0.0f;   // stalled: input is latest-wins, never burst catch-up
            }

            const sf::Vector2i mouse = sf::Mouse::getPosition(window);
            const sf::Vector2f centre{static_cast<float>(window.getSize().x) * 0.5f,
                                      static_cast<float>(window.getSize().y) * 0.5f};
            const blob::math::Vec2 dir = blob::math::normalized(
                {static_cast<float>(mouse.x) - centre.x, static_cast<float>(mouse.y) - centre.y});

            const blob::net::InputCommand cmd{
                .sequence = ++sequence,
                .dir_x    = blob::net::quantize_direction(dir.x),
                .dir_y    = blob::net::quantize_direction(dir.y),
                .split    = split_queued,
                .eject    = eject_queued,
            };
            split_queued = false;   // the queued edges ride exactly one command;
            eject_queued = false;   // from here the server's latch owns them
            std::array<std::byte, 6> buffer{};   // write_input is exactly 6 B
            blob::net::ByteWriter    writer{.buffer = buffer};
            blob::net::write_input(writer, cmd);
            const std::span<const std::byte> bytes = blob::net::written(writer);
            // Flags 0 on the Snapshot channel: input is a latest-wins 20 Hz
            // stream (invariant 5) — a lost one is outdated by the next, and
            // the server's sequence guard handles reordering.
            ENetPacket* packet = enet_packet_create(bytes.data(), bytes.size(), 0);
            enet_peer_send(server, static_cast<enet_uint8>(blob::net::Channel::Snapshot), packet);
            enet_host_flush(client);
        }

        // Render the raw latest snapshot. No interpolation yet — 20 Hz
        // stutter accepted until the snapshot buffer lands (in core, as pure
        // logic; see the file comment).
        window.clear(sf::Color{18, 18, 22});
        if (view.any) {
            for (const blob::net::EntityRecord& record : view.entities) {
                if (record.owner == player_id &&
                    static_cast<blob::sim::EntityKind>(record.kind) == blob::sim::EntityKind::Cell) {
                    camera_centre = {blob::net::dequantize_position(record.x, world_extent),
                                     blob::net::dequantize_position(record.y, world_extent)};
                    break;   // first own cell; multi-cell centroid is M4's problem
                }
            }
            window.setView(sf::View{{camera_centre.x, camera_centre.y}, camera_size});

            for (const blob::net::EntityRecord& record : view.entities) {
                // Display-only maths on wire values (the server keeps the
                // floats). radius_for_mass over default_tuning diverges
                // cosmetically if a server config overrides radius_factor —
                // tuning sync is M6 territory.
                const float radius = blob::sim::radius_for_mass(
                    blob::sim::default_tuning, blob::net::dequantize_mass(record.mass));
                shape.setRadius(radius);
                shape.setOrigin({radius, radius});
                shape.setPosition({blob::net::dequantize_position(record.x, world_extent),
                                   blob::net::dequantize_position(record.y, world_extent)});
                shape.setFillColor(colour_for(record, player_id));
                window.draw(shape);
            }
        }
        window.display();
    }

    // Polite goodbye unless the server already left: queue the disconnect and
    // give ENet a moment to deliver it, so the server frees the slot now
    // rather than after its timeout.
    if (!server_gone) {
        enet_peer_disconnect(server, 0);
        const auto deadline = steady_clock::now() + std::chrono::seconds{1};
        bool acknowledged = false;
        while (!acknowledged && steady_clock::now() < deadline) {
            ENetEvent event{};
            if (enet_host_service(client, &event, 50) > 0) {
                if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                    enet_packet_destroy(event.packet);
                } else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
                    acknowledged = true;
                }
            }
        }
    }

    enet_host_destroy(client);
    return 0;
}
