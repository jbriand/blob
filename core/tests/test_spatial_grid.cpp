// Differential tests: the grid must agree exactly with the O(n^2) truth on
// seeded layouts, and its candidate-pair volume must stay near-linear so an
// accidental broad-phase regression fails loudly instead of just slowly
// (invariant 6). Seeded PRNGs are fine here — the no-RNG rule binds core
// code, not its tests — but distribution output is implementation-defined,
// so every expectation is differential or structural, never a pinned value.

#include <blob/math/vec2.hpp>
#include <blob/sim/spatial_grid.hpp>
#include <blob/sim/tuning.hpp>
#include <blob/sim/world.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <utility>
#include <vector>

namespace sim = blob::sim;
namespace math = blob::math;

namespace {

constexpr float world_side = sim::default_tuning.world_extent;
constexpr float cell_side  = sim::default_tuning.grid_cell_size;

std::vector<sim::Entity> entities_at(const std::vector<math::Vec2>& positions)
{
    std::vector<sim::Entity> out;
    out.reserve(positions.size());
    std::uint32_t id = 1;
    for (const math::Vec2 p : positions) {
        out.push_back(sim::Entity{
            .id = id++,
            .owner = 0,
            .kind = sim::EntityKind::Pellet,
            .position = p,
            .velocity = {},
            .mass = 1.0f,
        });
    }
    return out;
}

std::vector<math::Vec2> uniform_layout(std::size_t n, std::mt19937& rng)
{
    std::uniform_real_distribution<float> coord(0.0f, world_side);
    std::vector<math::Vec2> out(n);
    for (math::Vec2& p : out) {
        p = {coord(rng), coord(rng)};
    }
    return out;
}

std::vector<math::Vec2> clustered_layout(std::mt19937& rng)
{
    // Several tight clumps: the load the game actually produces (pellet
    // fields, cell pile-ups) and the worst case for bucket skew.
    std::uniform_real_distribution<float> centre(0.0f, world_side);
    std::normal_distribution<float> spread(0.0f, cell_side * 0.25f);
    std::vector<math::Vec2> out;
    for (int c = 0; c < 6; ++c) {
        const math::Vec2 middle{centre(rng), centre(rng)};
        for (int i = 0; i < 150; ++i) {
            out.push_back({std::clamp(middle.x + spread(rng), 0.0f, world_side),
                           std::clamp(middle.y + spread(rng), 0.0f, world_side)});
        }
    }
    return out;
}

std::vector<std::uint32_t> brute_force_in_circle(const std::vector<sim::Entity>& entities,
                                                 math::Vec2 center, float radius)
{
    std::vector<std::uint32_t> out;
    for (std::size_t i = 0; i < entities.size(); ++i) {
        // The exact expression the grid query uses, so float rounding cannot
        // make the two sides disagree about a point sitting on the rim.
        if (math::length_sq(entities[i].position - center) <= radius * radius) {
            out.push_back(static_cast<std::uint32_t>(i));
        }
    }
    return out;   // ascending by construction
}

std::vector<std::uint32_t> grid_in_circle(const sim::SpatialGrid& grid, math::Vec2 center,
                                          float radius)
{
    std::vector<std::uint32_t> out;
    sim::for_each_in_circle(grid, center, radius,
                            [&out](std::uint32_t index, math::Vec2) { out.push_back(index); });
    std::sort(out.begin(), out.end());
    return out;
}

void expect_grid_matches_brute_force(const std::vector<sim::Entity>& entities)
{
    sim::SpatialGrid grid;
    sim::rebuild(grid, entities, world_side, cell_side);

    const math::Vec2 centers[] = {
        {0.0f, 0.0f},                          // corners: the rectangle walk must clamp
        {world_side, world_side},
        {world_side, 0.0f},
        {0.0f, world_side},
        {world_side / 2.0f, world_side / 2.0f},
        {cell_side * 3.0f, cell_side * 3.0f},  // centre on a cell-boundary lattice point
        {1234.5f, 6789.0f},
    };
    // Radii below, at, and above the bucket size: 3*cell_side proves the walk
    // covers more than the 3x3 neighbourhood.
    const float radii[] = {0.0f, cell_side / 2.0f, cell_side, 3.0f * cell_side};

    for (const math::Vec2 c : centers) {
        for (const float r : radii) {
            EXPECT_EQ(grid_in_circle(grid, c, r), brute_force_in_circle(entities, c, r))
                << "center (" << c.x << ", " << c.y << ") radius " << r;
        }
    }
}

} // namespace

TEST(SpatialGrid, MatchesBruteForceOnUniformLayout)
{
    std::mt19937 rng{20260810u};
    expect_grid_matches_brute_force(entities_at(uniform_layout(1000, rng)));
}

TEST(SpatialGrid, MatchesBruteForceOnClusteredLayout)
{
    std::mt19937 rng{4242u};
    expect_grid_matches_brute_force(entities_at(clustered_layout(rng)));
}

TEST(SpatialGrid, BoundaryPositionsLandInExactlyOneBucketAndAreFindable)
{
    const std::vector<math::Vec2> positions = {
        {0.0f, 0.0f},
        {world_side, world_side},        // step()'s clamp produces exactly this
        {world_side, 0.0f},
        {0.0f, world_side},
        {cell_side, cell_side},          // interior cell-boundary lattice points
        {cell_side * 5.0f, cell_side * 2.0f},
        {cell_side * 7.0f, 100.0f},
        {100.0f, cell_side * 9.0f},
    };
    const auto entities = entities_at(positions);
    sim::SpatialGrid grid;
    sim::rebuild(grid, entities, world_side, cell_side);

    // Exactly one bucket each: the CSR ranges partition entries, so it is
    // enough that the total matches and every index appears exactly once.
    ASSERT_EQ(grid.entries.size(), positions.size());
    ASSERT_EQ(static_cast<std::size_t>(grid.starts.back()), positions.size());
    std::vector<std::uint32_t> seen;
    for (const sim::GridEntry& entry : grid.entries) {
        seen.push_back(entry.index);
    }
    std::sort(seen.begin(), seen.end());
    for (std::size_t i = 0; i < seen.size(); ++i) {
        EXPECT_EQ(seen[i], static_cast<std::uint32_t>(i));
    }

    // And findable exactly where they sit. Radius 0 is the inclusive-boundary
    // case: length_sq == 0 must pass the <= filter.
    for (std::size_t i = 0; i < positions.size(); ++i) {
        const std::vector<std::uint32_t> expected{static_cast<std::uint32_t>(i)};
        EXPECT_EQ(grid_in_circle(grid, positions[i], 0.0f), expected);
    }
}

TEST(SpatialGrid, AllEntitiesInOneCellStillPairUpAndAreFindable)
{
    std::vector<math::Vec2> positions;
    for (int i = 0; i < 8; ++i) {
        positions.push_back({10.0f + static_cast<float>(i), 20.0f});
    }
    const auto entities = entities_at(positions);
    sim::SpatialGrid grid;
    sim::rebuild(grid, entities, world_side, cell_side);

    int pairs = 0;
    sim::for_each_candidate_pair(grid, [&pairs](std::uint32_t i, std::uint32_t j) {
        EXPECT_NE(i, j);
        ++pairs;
    });
    EXPECT_EQ(pairs, 8 * 7 / 2);   // one bucket -> the full n(n-1)/2

    EXPECT_EQ(grid_in_circle(grid, {14.0f, 20.0f}, cell_side).size(), 8u);
}

TEST(SpatialGrid, EmptyWorldIsLegal)
{
    sim::SpatialGrid grid;
    sim::rebuild(grid, {}, world_side, cell_side);
    EXPECT_TRUE(grid.entries.empty());

    int visits = 0;
    sim::for_each_in_circle(grid, {world_side / 2.0f, world_side / 2.0f}, 3.0f * cell_side,
                            [&visits](std::uint32_t, math::Vec2) { ++visits; });
    sim::for_each_candidate_pair(grid, [&visits](std::uint32_t, std::uint32_t) { ++visits; });
    EXPECT_EQ(visits, 0);

    // A never-rebuilt grid must also be safely queryable (public fields: the
    // queries cannot assume rebuild() ever ran).
    const sim::SpatialGrid untouched;
    sim::for_each_in_circle(untouched, {0.0f, 0.0f}, cell_side,
                            [&visits](std::uint32_t, math::Vec2) { ++visits; });
    sim::for_each_candidate_pair(untouched, [&visits](std::uint32_t, std::uint32_t) { ++visits; });
    EXPECT_EQ(visits, 0);
}

TEST(SpatialGrid, CandidatePairsCoverEveryClosePairExactlyOnce)
{
    std::mt19937 rng{777u};
    const auto entities = entities_at(uniform_layout(500, rng));
    sim::SpatialGrid grid;
    sim::rebuild(grid, entities, world_side, cell_side);

    std::vector<std::pair<std::uint32_t, std::uint32_t>> candidates;
    sim::for_each_candidate_pair(grid, [&candidates](std::uint32_t i, std::uint32_t j) {
        EXPECT_NE(i, j);   // no self-pairs, ever
        candidates.emplace_back(std::min(i, j), std::max(i, j));
    });

    // No (i,j)/(j,i) double reports: normalized pairs must be unique.
    std::sort(candidates.begin(), candidates.end());
    EXPECT_EQ(std::adjacent_find(candidates.begin(), candidates.end()), candidates.end());

    // Complete for pair distance <= cell_size: a superset is allowed (that is
    // what "candidate" means), a miss is a broad-phase hole.
    const auto n = static_cast<std::uint32_t>(entities.size());
    for (std::uint32_t i = 0; i < n; ++i) {
        for (std::uint32_t j = i + 1; j < n; ++j) {
            const float d_sq =
                math::length_sq(entities[i].position - entities[j].position);
            if (d_sq <= cell_side * cell_side) {
                EXPECT_TRUE(std::binary_search(candidates.begin(), candidates.end(),
                                               std::pair{i, j}))
                    << "missing close pair (" << i << ", " << j << ")";
            }
        }
    }
}

TEST(SpatialGrid, CandidatePairCountStaysNearLinear)
{
    // The O(n^2) tripwire (invariant 6). n = 2048 uniform over the default
    // 32x32 grid is ~2 entities per bucket; each entity is paired against its
    // own bucket + 8 neighbours exactly once, so expect roughly
    // 9 * density * n / 2 ~= 9 * 2 * 2048 / 2 ~= 18k pairs. The 40n = 81 920
    // bound leaves generous slack for seed clumping, while a broad-phase
    // regression to all-pairs would report n^2/2 ~= 2.1M and fail loudly.
    std::mt19937 rng{99u};
    const std::size_t n = 2048;
    const auto entities = entities_at(uniform_layout(n, rng));
    sim::SpatialGrid grid;
    sim::rebuild(grid, entities, world_side, cell_side);

    std::size_t pairs = 0;
    sim::for_each_candidate_pair(grid, [&pairs](std::uint32_t, std::uint32_t) { ++pairs; });

    RecordProperty("candidate_pairs", static_cast<int>(pairs));
    EXPECT_LE(pairs, 40u * n);
    EXPECT_GT(pairs, 0u);   // sanity: the tripwire is measuring something
}

TEST(SpatialGrid, RebuildIsDeterministicAndReusesStorage)
{
    std::mt19937 rng{31337u};
    const auto layout_a = entities_at(uniform_layout(300, rng));
    const auto layout_b = entities_at(clustered_layout(rng));

    const auto expect_same_grids = [](const sim::SpatialGrid& lhs, const sim::SpatialGrid& rhs) {
        EXPECT_EQ(lhs.cols, rhs.cols);
        ASSERT_EQ(lhs.starts, rhs.starts);
        ASSERT_EQ(lhs.entries.size(), rhs.entries.size());
        for (std::size_t k = 0; k < lhs.entries.size(); ++k) {
            EXPECT_EQ(lhs.entries[k].index, rhs.entries[k].index);
            EXPECT_TRUE(lhs.entries[k].position == rhs.entries[k].position);
        }
    };

    // Same span twice -> bit-identical starts and entries: the grid is a pure
    // function of its input (invariant 3's determinism, extended to M2).
    sim::SpatialGrid first;
    sim::SpatialGrid again;
    sim::rebuild(first, layout_b, world_side, cell_side);
    sim::rebuild(again, layout_b, world_side, cell_side);
    expect_same_grids(first, again);

    // And a reused grid that previously held a different world must converge
    // to the same layout — the capacity-reuse path cannot leak stale state.
    sim::SpatialGrid reused;
    sim::rebuild(reused, layout_a, world_side, cell_side);
    sim::rebuild(reused, layout_b, world_side, cell_side);
    expect_same_grids(first, reused);
}
