// blob server — authoritative, headless.
//
// No SFML, no window, no GPU. If this file ever needs a display it means
// something belongs in client/ instead.

#include "tick_loop.hpp"

#include <blob/net/protocol.hpp>
#include <blob/sim/world.hpp>

#include <enet/enet.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>

namespace {

std::atomic_bool g_running{true};

extern "C" void on_signal(int) { g_running.store(false, std::memory_order_relaxed); }

constexpr enet_uint16 default_port = 7777;
constexpr std::size_t max_clients  = 64;

/// RAII for the ENet global. Every early return below is therefore safe.
struct EnetGuard {
    EnetGuard() : ok{enet_initialize() == 0} {}
    ~EnetGuard() { if (ok) { enet_deinitialize(); } }
    EnetGuard(const EnetGuard&) = delete;
    EnetGuard& operator=(const EnetGuard&) = delete;
    bool ok;
};

} // namespace

int main(int argc, char** argv)
{
    const enet_uint16 port =
        argc > 1 ? static_cast<enet_uint16>(std::strtoul(argv[1], nullptr, 10)) : default_port;

    EnetGuard enet;
    if (!enet.ok) {
        std::fprintf(stderr, "enet_initialize failed\n");
        return 1;
    }

    ENetAddress address{};
    address.host = ENET_HOST_ANY;
    address.port = port;

    ENetHost* host = enet_host_create(&address,
                                      max_clients,
                                      static_cast<std::size_t>(blob::net::Channel::Count),
                                      0,   // no incoming bandwidth cap
                                      0);  // no outgoing bandwidth cap
    if (host == nullptr) {
        std::fprintf(stderr, "enet_host_create failed on port %u\n", port);
        return 1;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    blob::sim::World world;
    auto loop = blob::server::make_tick_loop(blob::sim::tick_rate);

    std::printf("blob-server listening on udp/%u, %d Hz\n", port, blob::sim::tick_rate);

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
            blob::sim::step(world, blob::sim::tick_dt);
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
