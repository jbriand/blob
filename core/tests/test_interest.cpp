// Interest management: the zoom curve and the visible-set query. The query is
// differential-tested against the O(n) brute-force truth on seeded layouts —
// seeded PRNGs are fine here (the no-RNG rule binds core code, not its tests)
// — but distribution output is implementation-defined, so every expectation
// is differential or structural, never a pinned coordinate.

#include <blob/math/vec2.hpp>
#include <blob/sim/interest.hpp>
#include <blob/sim/spatial_grid.hpp>
#include <blob/sim/tuning.hpp>
#include <blob/sim/world.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

namespace sim  = blob::sim;
namespace math = blob::math;

namespace {

constexpr float world_side = sim::default_tuning.world_extent;
constexpr float cell_side  = sim::default_tuning.grid_cell_size;

/// A world holding pellets at exactly `positions`, with the grid rebuilt so
/// collect_visible has something to query — the standing contract is that the
/// grid describes the world as of the last step()/rebuild.
sim::World world_at(const std::vector<math::Vec2>& positions)
{
    sim::World world;
    for (const math::Vec2 p : positions) {
        sim::spawn(world, sim::EntityKind::Pellet, p, 1.0f);
    }
    sim::rebuild(world.grid, world.entities, world.tuning.world_extent,
                 world.tuning.grid_cell_size);
    return world;
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
    // Tight clumps — the load interest management exists for (a crowd inside
    // one view circle) and the worst case for bucket skew.
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

std::vector<std::uint32_t> brute_force_visible(const sim::World& world, math::Vec2 centre,
                                               float radius)
{
    std::vector<std::uint32_t> out;
    for (std::size_t i = 0; i < world.entities.size(); ++i) {
        // The exact rim expression the grid query uses, so float rounding
        // cannot make the two sides disagree about a point on the boundary.
        if (math::length_sq(world.entities[i].position - centre) <= radius * radius) {
            out.push_back(static_cast<std::uint32_t>(i));
        }
    }
    return out;   // ascending by construction
}

void expect_visible_matches_brute_force(const sim::World& world)
{
    const math::Vec2 centres[] = {
        {0.0f, 0.0f},                            // corners: the covered-cell walk must clamp
        {world_side, world_side},
        {world_side / 2.0f, world_side / 2.0f},
        {cell_side * 3.0f, cell_side * 3.0f},    // centre on a bucket-boundary lattice point
        {1234.5f, 6789.0f},
    };
    // Radii below, at, and well above the bucket size — the realistic view
    // radii (view_base 640 and up) all exceed cell_size 256, so the
    // several-buckets case is the one the shipping query lives in.
    const float radii[] = {0.0f, cell_side / 2.0f, cell_side, 3.0f * cell_side,
                           sim::view_radius(sim::default_tuning, 100.0f)};

    std::vector<std::uint32_t> visible;
    for (const math::Vec2 c : centres) {
        for (const float r : radii) {
            sim::collect_visible(world, c, r, visible);
            std::vector<std::uint32_t> sorted = visible;
            std::sort(sorted.begin(), sorted.end());
            EXPECT_EQ(sorted, brute_force_visible(world, c, r))
                << "centre (" << c.x << ", " << c.y << ") radius " << r;
        }
    }
}

} // namespace

TEST(Interest, ViewRadiusFollowsTheSqrtLawAndIsMonotone)
{
    const sim::Tuning& t = sim::default_tuning;

    // The floor: a session with no cells this instant (mid-respawn) queries
    // with mass 0 and must still see view_base; a negative mass is a bug but
    // a representable one, and it must not shrink the view below the floor.
    EXPECT_EQ(sim::view_radius(t, 0.0f), t.view_base);
    EXPECT_EQ(sim::view_radius(t, -25.0f), t.view_base);

    // Monotone: growing never narrows the view.
    float previous = sim::view_radius(t, 0.0f);
    for (const float mass : {1.0f, 10.0f, 36.0f, 100.0f, 400.0f, 2500.0f, 65535.0f}) {
        const float radius = sim::view_radius(t, mass);
        EXPECT_GT(radius, previous) << "mass " << mass;
        previous = radius;
    }

    // The same √ law as drawn radius: quadrupling the mass doubles the
    // mass-driven part of the view, exactly as it doubles radius_for_mass —
    // the view widens at the rate the player's cells grow on screen.
    EXPECT_FLOAT_EQ(sim::view_radius(t, 400.0f) - t.view_base,
                    2.0f * (sim::view_radius(t, 100.0f) - t.view_base));
}

TEST(Interest, MatchesBruteForceOnUniformLayout)
{
    std::mt19937 rng{20260810u};
    expect_visible_matches_brute_force(world_at(uniform_layout(1000, rng)));
}

TEST(Interest, MatchesBruteForceOnClusteredLayout)
{
    std::mt19937 rng{4242u};
    expect_visible_matches_brute_force(world_at(clustered_layout(rng)));
}

TEST(Interest, OutputOrderIsDeterministicAndTheVectorIsCleared)
{
    std::mt19937 rng{777u};
    const sim::World world = world_at(uniform_layout(500, rng));
    const math::Vec2 centre{world_side / 2.0f, world_side / 2.0f};
    const float radius = sim::view_radius(sim::default_tuning, 250.0f);

    // Same query twice into the same scratch vector: identical sequences,
    // element for element — per-peer snapshot content is built on this order,
    // so it must be a pure function of the world, not of allocator or hash
    // accidents. The second call also proves collect_visible clears before
    // filling (the reused-scratch contract), since any leftover would
    // duplicate an index.
    std::vector<std::uint32_t> first;
    sim::collect_visible(world, centre, radius, first);
    ASSERT_FALSE(first.empty());   // sanity: the query found something to compare

    std::vector<std::uint32_t> again = first;   // pre-seeded with stale contents
    sim::collect_visible(world, centre, radius, again);
    EXPECT_EQ(first, again);
}

TEST(Interest, NeverRebuiltWorldYieldsAnEmptyVisibleSet)
{
    // A World that has never stepped has a never-rebuilt grid; the query must
    // treat it as empty (grid contract), not index out of bounds.
    sim::World world;
    sim::spawn(world, sim::EntityKind::Pellet, {100.0f, 100.0f}, 1.0f);

    std::vector<std::uint32_t> visible{99u};   // stale scratch to be cleared
    sim::collect_visible(world, {100.0f, 100.0f}, 500.0f, visible);
    EXPECT_TRUE(visible.empty());
}
