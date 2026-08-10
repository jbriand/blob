#include "snapshot_encode.hpp"

#include <blob/math/vec2.hpp>
#include <blob/net/quantize.hpp>
#include <blob/sim/spatial_grid.hpp>
#include <blob/sim/world.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
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

// --- per-peer selection (M6) ------------------------------------------------

namespace {

/// collect_visible answers through the grid, and these worlds are hand-built
/// rather than stepped, so the standing "grid describes the last step" state
/// is established by hand.
void rebuild_grid(sim::World& world)
{
    sim::rebuild(world.grid, world.entities, world.tuning.world_extent,
                 world.tuning.grid_cell_size);
}

} // namespace

TEST(SnapshotEncode, CollectsRecordsForExactlyTheNamedIndices)
{
    sim::World world;
    const sim::EntityId cell   = sim::spawn(world, sim::EntityKind::Cell, {100.0f, 100.0f}, 10.0f, 1);
    sim::spawn(world, sim::EntityKind::Pellet, {200.0f, 200.0f}, 1.0f);
    const sim::EntityId virus  = sim::spawn(world, sim::EntityKind::Virus, {300.0f, 300.0f}, 100.0f);

    // Indices 0 and 2, in the given order — the middle entity stays out, and
    // a stale out-of-range index degrades to a missing record, never a read.
    const std::vector<std::uint32_t> indices{2u, 0u, 99u};
    std::vector<net::EntityRecord>   records(7);   // stale contents to be cleared
    server::collect_records_for(world, indices, records);

    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[0].id, virus);
    EXPECT_EQ(records[1].id, cell);
    EXPECT_EQ(records[1].owner, 1u);
    EXPECT_EQ(records[1].x, net::quantize_position(100.0f, world.tuning.world_extent));
}

TEST(SnapshotEncode, UnderBudgetPeerGetsTheWholeVisibleSetAndOnlyThat)
{
    sim::World world;
    const blob::math::Vec2 centre{4096.0f, 4096.0f};
    // Four entities inside a 500-unit view, two well outside it.
    for (int i = 1; i <= 4; ++i) {
        sim::spawn(world, sim::EntityKind::Pellet,
                   {centre.x + 100.0f * static_cast<float>(i), centre.y}, 1.0f);
    }
    sim::spawn(world, sim::EntityKind::Pellet, {centre.x + 2000.0f, centre.y}, 1.0f);
    sim::spawn(world, sim::EntityKind::Pellet, {centre.x, centre.y + 3000.0f}, 1.0f);
    rebuild_grid(world);

    std::vector<std::uint32_t>     visible;
    std::vector<net::EntityRecord> records;
    server::collect_visible_records(world, centre, 500.0f, 3 * net::max_entities_per_chunk,
                                    visible, records);

    // Everything in view and nothing beyond it, without any truncation.
    std::vector<std::uint32_t> ids;
    for (const net::EntityRecord& r : records) {
        ids.push_back(r.id);
    }
    std::sort(ids.begin(), ids.end());
    EXPECT_EQ(ids, (std::vector<std::uint32_t>{1u, 2u, 3u, 4u}));
}

TEST(SnapshotEncode, OverBudgetKeepsExactlyTheNearestRecords)
{
    sim::World world;
    const blob::math::Vec2 centre{4096.0f, 4096.0f};
    // Eight entities on a line at strictly increasing distances 30..240, so
    // "nearest" has one right answer: ids 1..budget in distance order.
    for (int i = 1; i <= 8; ++i) {
        sim::spawn(world, sim::EntityKind::Pellet,
                   {centre.x + 30.0f * static_cast<float>(i), centre.y}, 1.0f);
    }
    rebuild_grid(world);

    const std::size_t budget = 4;
    std::vector<std::uint32_t>     visible;
    std::vector<net::EntityRecord> records;
    server::collect_visible_records(world, centre, 10000.0f, budget, visible, records);

    // Exactly the budget, nearest first — the farthest four are the drops.
    ASSERT_EQ(records.size(), budget);
    for (std::size_t i = 0; i < budget; ++i) {
        EXPECT_EQ(records[i].id, static_cast<std::uint32_t>(i + 1)) << "record " << i;
    }
}

TEST(SnapshotEncode, BudgetDistanceTiesBreakOnIndexAscending)
{
    // Two entities at exactly the same distance and one past them: with room
    // for one, the tie must resolve by index — the deterministic tiebreak the
    // per-peer promise depends on — not by whatever order a sort happens to
    // leave equal keys in.
    sim::World world;
    const blob::math::Vec2 centre{4096.0f, 4096.0f};
    const sim::EntityId left  = sim::spawn(world, sim::EntityKind::Pellet,
                                           {centre.x - 50.0f, centre.y}, 1.0f);   // index 0
    sim::spawn(world, sim::EntityKind::Pellet, {centre.x + 50.0f, centre.y}, 1.0f);
    sim::spawn(world, sim::EntityKind::Pellet, {centre.x, centre.y + 90.0f}, 1.0f);
    rebuild_grid(world);

    std::vector<std::uint32_t>     visible;
    std::vector<net::EntityRecord> records;
    server::collect_visible_records(world, centre, 1000.0f, 1, visible, records);

    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].id, left);
}

TEST(SnapshotEncode, PerPeerSelectionIsDeterministic)
{
    // Same world, same query, twice: identical record sequences, field for
    // field. What a peer receives must be a pure function of the world —
    // per-peer output depending on allocator or ordering accidents would
    // desync spectators of the same fight.
    sim::World world;
    const blob::math::Vec2 centre{4096.0f, 4096.0f};
    std::mt19937 rng{123u};
    std::uniform_real_distribution<float> offset(-1500.0f, 1500.0f);
    for (int i = 0; i < 600; ++i) {
        sim::spawn(world, sim::EntityKind::Pellet,
                   {centre.x + offset(rng), centre.y + offset(rng)}, 1.0f);
    }
    rebuild_grid(world);

    const std::size_t budget = 100;   // well under the 600 in view: truncation runs
    std::vector<std::uint32_t>     visible;
    std::vector<net::EntityRecord> first;
    std::vector<net::EntityRecord> again;
    server::collect_visible_records(world, centre, 3000.0f, budget, visible, first);
    server::collect_visible_records(world, centre, 3000.0f, budget, visible, again);

    ASSERT_EQ(first.size(), budget);
    ASSERT_EQ(again.size(), budget);
    for (std::size_t i = 0; i < budget; ++i) {
        EXPECT_EQ(first[i].id, again[i].id) << "record " << i;
        EXPECT_EQ(first[i].x, again[i].x) << "record " << i;
        EXPECT_EQ(first[i].y, again[i].y) << "record " << i;
        EXPECT_EQ(first[i].mass, again[i].mass) << "record " << i;
    }
}
