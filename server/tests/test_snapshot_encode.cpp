#include "snapshot_encode.hpp"

#include <blob/net/quantize.hpp>
#include <blob/sim/world.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace server = blob::server;
namespace net    = blob::net;
namespace sim    = blob::sim;

// The chunk expectations below (200 -> 3) are spelled against this value.
static_assert(net::max_entities_per_chunk == 91);

TEST(SnapshotEncode, CollectsEveryEntityInArrayOrder)
{
    sim::World world;
    const sim::EntityId cell   = sim::spawn(world, sim::EntityKind::Cell, {1024.0f, 2048.0f}, 10.0f, 7);
    const sim::EntityId pellet = sim::spawn(world, sim::EntityKind::Pellet, {0.0f, 8192.0f}, 1.0f);
    const sim::EntityId virus  = sim::spawn(world, sim::EntityKind::Virus, {4096.0f, 4096.0f}, 100.0f);

    std::vector<net::EntityRecord> records;
    server::collect_records(world, records);

    ASSERT_EQ(records.size(), 3u);
    EXPECT_EQ(records[0].id, cell);
    EXPECT_EQ(records[1].id, pellet);
    EXPECT_EQ(records[2].id, virus);

    EXPECT_EQ(records[0].owner, 7u);
    EXPECT_EQ(records[1].owner, 0u);
    EXPECT_EQ(records[0].kind, static_cast<std::uint8_t>(sim::EntityKind::Cell));
    EXPECT_EQ(records[1].kind, static_cast<std::uint8_t>(sim::EntityKind::Pellet));
    EXPECT_EQ(records[2].kind, static_cast<std::uint8_t>(sim::EntityKind::Virus));

    // Exact wire values, spelled with quantize.hpp — the one packing
    // authority (invariant 4) — plus the known u16 endpoints.
    const float extent = world.tuning.world_extent;
    EXPECT_EQ(records[0].x, net::quantize_position(1024.0f, extent));
    EXPECT_EQ(records[0].y, net::quantize_position(2048.0f, extent));
    EXPECT_EQ(records[1].x, 0u);       // world edge 0
    EXPECT_EQ(records[1].y, 65535u);   // world edge extent
    EXPECT_EQ(records[0].mass, 10u);   // linear mass encoding keeps small masses exact
    EXPECT_EQ(records[1].mass, 1u);
    EXPECT_EQ(records[2].mass, 100u);
}

TEST(SnapshotEncode, QuantizesAgainstTheWorldsOwnExtent)
{
    // A config-overridden extent must drive the packing: hard-coding 8192
    // here would warp every position on a non-default server.
    sim::World world;
    world.tuning.world_extent = 1000.0f;
    sim::spawn(world, sim::EntityKind::Cell, {500.0f, 250.0f}, 10.0f, 1);

    std::vector<net::EntityRecord> records;
    server::collect_records(world, records);

    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].x, net::quantize_position(500.0f, 1000.0f));
    EXPECT_EQ(records[0].y, net::quantize_position(250.0f, 1000.0f));
    EXPECT_NE(records[0].x, net::quantize_position(500.0f, 8192.0f));
}

TEST(SnapshotEncode, RefillClearsThePreviousContents)
{
    // The broadcast loop reuses one vector across iterations; a refill must
    // never leak last tick's records into this tick's snapshot.
    sim::World world;
    sim::spawn(world, sim::EntityKind::Pellet, {1.0f, 1.0f}, 1.0f);

    std::vector<net::EntityRecord> records(50);   // stale "last tick" contents
    server::collect_records(world, records);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].id, 1u);
}

TEST(SnapshotEncode, ChunkArithmeticCoversEveryRecordExactlyOnce)
{
    const auto chunks_for = [](std::size_t n) {
        std::vector<net::EntityRecord> records(n);
        for (std::size_t i = 0; i < n; ++i) {
            records[i].id = static_cast<std::uint32_t>(i + 1);
        }
        std::size_t   total          = 0;
        std::uint32_t expected_first = 1;
        const std::size_t count = server::for_each_chunk(
            records, [&](std::span<const net::EntityRecord> chunk) {
                EXPECT_FALSE(chunk.empty());
                EXPECT_LE(chunk.size(), net::max_entities_per_chunk);
                EXPECT_EQ(chunk.front().id, expected_first);   // in order, no overlap
                expected_first += static_cast<std::uint32_t>(chunk.size());
                total += chunk.size();
            });
        EXPECT_EQ(total, n);   // every record exactly once
        return count;
    };

    EXPECT_EQ(chunks_for(0), 0u);     // an empty world is not worth a datagram
    EXPECT_EQ(chunks_for(1), 1u);
    EXPECT_EQ(chunks_for(91), 1u);    // exactly full
    EXPECT_EQ(chunks_for(92), 2u);    // one record spills into a second chunk
    EXPECT_EQ(chunks_for(200), 3u);   // 91 + 91 + 18
}
