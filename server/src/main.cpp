// blob server — authoritative, headless.
//
// No SFML, no window, no GPU. If this file ever needs a display it means
// something belongs in client/ instead.

#include "config.hpp"
#include "tick_loop.hpp"

#include <blob/net/protocol.hpp>
#include <blob/sim/world.hpp>

#include <enet/enet.h>

#include <atomic>
#include <charconv>
#include <csignal>
#include <cstdio>
#include <optional>
#include <string_view>
#include <system_error>

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

    blob::sim::World world;
    world.tuning = config->tuning;
    auto loop = blob::server::make_tick_loop(world.tuning.tick_rate);

    std::printf("blob-server listening on udp/%u, %d Hz, %zu peer slots\n",
                config->port, world.tuning.tick_rate, config->max_clients);

    while (g_running.load(std::memory_order_relaxed)) {
        // Drain the socket first: input that arrived since the last tick should
        // be applied to *this* tick, not the next one.
        ENetEvent event{};
        while (enet_host_service(host, &event, 0) > 0) {
            switch (event.type) {
            case ENET_EVENT_TYPE_CONNECT:
                std::printf("peer connected: %x:%u\n", event.peer->address.host,
                            event.peer->address.port);
                // TODO: assign a PlayerId, send Welcome on Channel::Control.
                break;
            case ENET_EVENT_TYPE_RECEIVE:
                // TODO: decode with blob::net::read_input, feed blob::sim::apply_intent().
                enet_packet_destroy(event.packet);
                break;
            case ENET_EVENT_TYPE_DISCONNECT:
                std::printf("peer disconnected\n");
                break;
            default:
                break;
            }
        }

        for (int i = blob::server::pump(loop); i > 0; --i) {
            blob::sim::step(world, blob::sim::tick_dt(world.tuning));
            // TODO: per-peer interest query + snapshot encode on Channel::Snapshot.
        }

        // Sleep out the remainder inside ENet so a packet can wake us early.
        if (const auto idle = blob::server::time_to_next_tick(loop); idle.count() > 0) {
            if (enet_host_service(host, &event, static_cast<enet_uint32>(idle.count())) > 0 &&
                event.type == ENET_EVENT_TYPE_RECEIVE) {
                enet_packet_destroy(event.packet);
            }
        }
    }

    std::printf("\nshutting down after %llu ticks (%llu dropped)\n",
                static_cast<unsigned long long>(loop.ticks_run),
                static_cast<unsigned long long>(loop.ticks_dropped));

    enet_host_destroy(host);
    return 0;
}
