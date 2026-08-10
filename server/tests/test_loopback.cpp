// End-to-end over real UDP, in one process: an ENet server host and an ENet
// client host on 127.0.0.1, asserting the full chain the playable build
// depends on — connect -> Welcome -> Input -> apply_intent -> step ->
// snapshot broadcast -> client decodes and watches its own cell move.
//
// The server half is a miniature made of the SAME building blocks main.cpp
// wires together (sessions vector + sequence guard + apply_intent + step +
// collect_records/write_snapshot broadcast) — main() itself is not callable
// from a test, and manual play covers its wiring. One deliberate divergence:
// main spawns via spawn_player (rng placement), the test spawns at a pinned
// position so "chases the cursor rightward" cannot start clamped against the
// world's right edge.
// Unlike main, the test drives step() directly instead of a tick loop, so it
// finishes in milliseconds of wall time, not in 50 ms ticks.

#include "session.hpp"
#include "snapshot_encode.hpp"

#include <blob/math/vec2.hpp>
#include <blob/net/protocol.hpp>
#include <blob/net/quantize.hpp>
#include <blob/sim/world.hpp>

#include <enet/enet.h>

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace {

namespace net = blob::net;
namespace sim = blob::sim;
using steady_clock = std::chrono::steady_clock;

// Fixed and unregistered; nothing else in the suite binds it. Explicitly
// 127.0.0.1 and never ENET_HOST_ANY — a wildcard listener is what trips the
// Windows Firewall consent prompt on dev boxes.
constexpr std::uint16_t loopback_port = 27877;

constexpr enet_uint8 channel_control  = static_cast<enet_uint8>(net::Channel::Control);
constexpr enet_uint8 channel_snapshot = static_cast<enet_uint8>(net::Channel::Snapshot);

struct EnetGuard {
    EnetGuard() : ok{enet_initialize() == 0} {}
    ~EnetGuard() { if (ok) { enet_deinitialize(); } }
    EnetGuard(const EnetGuard&) = delete;
    EnetGuard& operator=(const EnetGuard&) = delete;
    bool ok;
};

struct HostGuard {
    explicit HostGuard(ENetHost* h) : host{h} {}
    ~HostGuard() { if (host != nullptr) { enet_host_destroy(host); } }
    HostGuard(const HostGuard&) = delete;
    HostGuard& operator=(const HostGuard&) = delete;
    ENetHost* host;
};

// --- the miniature server -------------------------------------------------

struct MiniServer {
    ENetHost*                                host{};
    sim::World                               world;
    std::vector<blob::server::PlayerSession> sessions;
    sim::PlayerId                            next_player_id{1};
    std::vector<net::EntityRecord>           records;
};

void server_handle_event(MiniServer& s, const ENetEvent& event)
{
    switch (event.type) {
    case ENET_EVENT_TYPE_CONNECT: {
        const sim::PlayerId id = s.next_player_id++;
        event.peer->data = reinterpret_cast<void*>(static_cast<std::uintptr_t>(id));
        blob::server::add_session(s.sessions, id);

        // Pinned spawn (not spawn_player): the movement assert needs a cell
        // with room to travel right — see the header comment.
        const float extent = s.world.tuning.world_extent;
        const float offset = 64.0f * static_cast<float>(id % 16u);
        sim::spawn(s.world, sim::EntityKind::Cell,
                   {extent * 0.5f + offset, extent * 0.5f + offset},
                   s.world.tuning.spawn_mass, id);

        std::array<std::byte, 8> buffer{};
        net::ByteWriter          writer{.buffer = buffer};
        net::write_welcome(writer, {.version      = net::protocol_version,
                                    .player_id    = id,
                                    .world_extent = static_cast<std::uint16_t>(extent),
                                    .tick_rate    = static_cast<std::uint8_t>(s.world.tuning.tick_rate)});
        const std::span<const std::byte> bytes = net::written(writer);
        enet_peer_send(event.peer, channel_control,
                       enet_packet_create(bytes.data(), bytes.size(), ENET_PACKET_FLAG_RELIABLE));
        break;
    }
    case ENET_EVENT_TYPE_RECEIVE: {
        net::ByteReader reader{
            .buffer = {reinterpret_cast<const std::byte*>(event.packet->data),
                       event.packet->dataLength}};
        if (const std::optional<net::InputCommand> cmd = net::read_input(reader)) {
            const auto id = static_cast<sim::PlayerId>(
                reinterpret_cast<std::uintptr_t>(event.peer->data));
            if (blob::server::PlayerSession* session =
                    blob::server::find_session(s.sessions, id)) {
                if (!session->received_input ||
                    blob::server::sequence_newer(cmd->sequence, session->last_sequence)) {
                    session->received_input = true;
                    session->last_sequence  = cmd->sequence;
                    const blob::math::Vec2 direction = blob::math::normalized(
                        {net::dequantize_direction(cmd->dir_x),
                         net::dequantize_direction(cmd->dir_y)});
                    sim::apply_intent(s.world, {.player    = id,
                                                .direction = direction,
                                                .split     = cmd->split,
                                                .eject     = cmd->eject});
                }
            }
        }
        enet_packet_destroy(event.packet);
        break;
    }
    case ENET_EVENT_TYPE_DISCONNECT: {
        const auto id =
            static_cast<sim::PlayerId>(reinterpret_cast<std::uintptr_t>(event.peer->data));
        blob::server::remove_session(s.sessions, id);
        sim::despawn_player(s.world, id);   // entities + standing intent, like main
        break;
    }
    default:
        break;
    }
}

/// Waits up to `wait_ms` for the first event, then drains without blocking.
void service_server(MiniServer& s, enet_uint32 wait_ms)
{
    ENetEvent event{};
    while (enet_host_service(s.host, &event, wait_ms) > 0) {
        server_handle_event(s, event);
        wait_ms = 0;
    }
}

void server_broadcast(MiniServer& s)
{
    blob::server::collect_records(s.world, s.records);
    const auto tick = static_cast<std::uint32_t>(s.world.tick);
    blob::server::for_each_chunk(s.records, [&](std::span<const net::EntityRecord> chunk) {
        std::array<std::byte, net::snapshot_soft_mtu> buffer;
        net::ByteWriter writer{.buffer = buffer};
        net::write_snapshot(writer, tick, chunk);
        const std::span<const std::byte> bytes = net::written(writer);
        enet_host_broadcast(s.host, channel_snapshot,
                            enet_packet_create(bytes.data(), bytes.size(), 0));
    });
}

// --- the miniature client -------------------------------------------------

struct MiniClient {
    ENetHost*                                host{};
    ENetPeer*                                peer{};
    std::optional<net::WelcomePayload>       welcome;
    std::uint32_t                            tick{};
    bool                                     any_snapshot{};
    std::vector<net::EntityRecord>           entities;
};

void client_handle_event(MiniClient& c, const ENetEvent& event)
{
    if (event.type != ENET_EVENT_TYPE_RECEIVE) {
        return;
    }
    const std::span<const std::byte> data{
        reinterpret_cast<const std::byte*>(event.packet->data), event.packet->dataLength};

    net::ByteReader welcome_reader{.buffer = data};
    if (const std::optional<net::WelcomePayload> welcome = net::read_welcome(welcome_reader)) {
        c.welcome = welcome;
    } else {
        net::ByteReader snapshot_reader{.buffer = data};
        std::array<net::EntityRecord, net::max_entities_per_chunk> chunk{};
        if (const std::optional<net::SnapshotHeader> header =
                net::read_snapshot(snapshot_reader, chunk)) {
            const auto first = chunk.begin();
            const auto last  = first + static_cast<std::ptrdiff_t>(header->count);
            // Latest-tick assembly, exactly as the real client: newer replaces,
            // same appends, older drops.
            if (!c.any_snapshot || header->tick > c.tick) {
                c.any_snapshot = true;
                c.tick         = header->tick;
                c.entities.assign(first, last);
            } else if (header->tick == c.tick) {
                c.entities.insert(c.entities.end(), first, last);
            }
        }
    }
    enet_packet_destroy(event.packet);
}

void service_client(MiniClient& c, enet_uint32 wait_ms)
{
    ENetEvent event{};
    while (enet_host_service(c.host, &event, wait_ms) > 0) {
        client_handle_event(c, event);
        wait_ms = 0;
    }
}

/// The client's own cell in the latest assembled snapshot, if visible.
[[nodiscard]] const net::EntityRecord* find_own_cell(const MiniClient& c)
{
    for (const net::EntityRecord& record : c.entities) {
        if (record.owner == c.welcome->player_id &&
            record.kind == static_cast<std::uint8_t>(sim::EntityKind::Cell)) {
            return &record;
        }
    }
    return nullptr;
}

void send_input(MiniClient& c, std::uint16_t sequence, float dir_x, float dir_y)
{
    const net::InputCommand cmd{
        .sequence = sequence,
        .dir_x    = net::quantize_direction(dir_x),
        .dir_y    = net::quantize_direction(dir_y),
        .split    = false,
        .eject    = false,
    };
    std::array<std::byte, 6> buffer{};
    net::ByteWriter          writer{.buffer = buffer};
    net::write_input(writer, cmd);
    const std::span<const std::byte> bytes = net::written(writer);
    // Flags 0 on the Snapshot channel, exactly like the real client
    // (invariant 5): latest-wins, the sequence guard sorts out reordering.
    enet_peer_send(c.peer, channel_snapshot,
                   enet_packet_create(bytes.data(), bytes.size(), 0));
    enet_host_flush(c.host);
}

} // namespace

TEST(Loopback, CursorChaseOverRealUdp)
{
    const EnetGuard enet;
    ASSERT_TRUE(enet.ok);

    ENetAddress bind_address{};
    ASSERT_EQ(enet_address_set_host(&bind_address, "127.0.0.1"), 0);
    bind_address.port = loopback_port;

    MiniServer server{};
    // The wire chain is the thing under test, not gameplay: an empty pellet
    // field keeps every broadcast a single deterministic chunk, the spawn
    // mass exactly 10 on first sighting, and the post-disconnect world
    // exactly empty. Eating has its own suite in core.
    server.world.tuning.target_pellet_count = 0;
    server.host = enet_host_create(&bind_address, 8,
                                   static_cast<std::size_t>(net::Channel::Count), 0, 0);
    ASSERT_NE(server.host, nullptr) << "cannot bind 127.0.0.1:" << loopback_port;
    const HostGuard server_guard{server.host};

    MiniClient client{};
    client.host = enet_host_create(nullptr, 1,
                                   static_cast<std::size_t>(net::Channel::Count), 0, 0);
    ASSERT_NE(client.host, nullptr);
    const HostGuard client_guard{client.host};

    ENetAddress connect_address = bind_address;
    client.peer = enet_host_connect(client.host, &connect_address,
                                    static_cast<std::size_t>(net::Channel::Count), 0);
    ASSERT_NE(client.peer, nullptr);

    // One overall deadline for the whole test; every wait below is a short
    // service timeout inside a deadline-bounded loop, so expiry means a
    // failed assertion with a message — never a hang.
    const auto deadline = steady_clock::now() + std::chrono::seconds{5};

    // --- connect -> Welcome ---
    while (!client.welcome && steady_clock::now() < deadline) {
        service_server(server, 2);
        service_client(client, 2);
    }
    ASSERT_TRUE(client.welcome.has_value()) << "no Welcome within the 5 s deadline";
    EXPECT_EQ(client.welcome->version, net::protocol_version);
    EXPECT_EQ(client.welcome->version, 3u);
    ASSERT_NE(client.welcome->player_id, 0u);
    EXPECT_EQ(client.welcome->world_extent, 8192u);
    EXPECT_EQ(client.welcome->tick_rate, 20u);
    ASSERT_EQ(server.sessions.size(), 1u);

    // --- input -> apply -> step -> broadcast -> decode, until the cell has
    // demonstrably chased the cursor rightward ---
    const float         extent = static_cast<float>(client.welcome->world_extent);
    std::optional<float> first_x;
    float               last_x   = 0.0f;
    int                 sightings = 0;
    std::uint16_t       sequence  = 0;

    while (sightings < 6 && steady_clock::now() < deadline) {
        send_input(client, ++sequence, 1.0f, 0.0f);   // hard right

        service_server(server, 2);   // decode + sequence guard + apply_intent
        sim::step(server.world, sim::tick_dt(server.world.tuning));
        server_broadcast(server);

        service_client(client, 2);   // adopt the latest snapshot

        if (const net::EntityRecord* own = find_own_cell(client)) {
            const float x = net::dequantize_position(own->x, extent);
            if (!first_x) {
                first_x = x;
                EXPECT_EQ(own->mass, 10u);   // the placeholder spawn, quantized exactly
            }
            last_x = x;
            ++sightings;
        }
    }

    ASSERT_TRUE(first_x.has_value()) << "own cell never appeared in a snapshot within 5 s";
    ASSERT_GE(sightings, 2) << "too few snapshots decoded within 5 s";
    // 720 u/s at mass 10 and 50 ms steps move ~36 units per applied round —
    // orders of magnitude above the ~0.13-unit position quantization step.
    EXPECT_GT(last_x, *first_x + 1.0f)
        << "cell did not move right: first_x=" << *first_x << " last_x=" << last_x;

    // --- clean teardown: polite disconnect, server forgets the player ---
    enet_peer_disconnect(client.peer, 0);
    while (!server.sessions.empty() && steady_clock::now() < deadline) {
        service_client(client, 2);
        service_server(server, 2);
    }
    EXPECT_TRUE(server.sessions.empty()) << "server never saw the disconnect within 5 s";
    EXPECT_TRUE(server.world.entities.empty());   // placeholder despawn ran
    // Hosts are destroyed by the guards, then EnetGuard deinitializes.
}
