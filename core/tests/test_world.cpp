#include <blob/sim/spatial_grid.hpp>
#include <blob/sim/tuning.hpp>
#include <blob/sim/world.hpp>

#include <gtest/gtest.h>

#include <cstdint>

namespace sim = blob::sim;

TEST(World, StepIsFrameRateIndependent)
{
    // Same total time, different step counts -> same position. If this ever
    // fails, something in step() has started depending on call frequency.
    sim::World coarse;
    sim::World fine;

    const auto seed = [](sim::World& w) {
        // Empty pellet field: eating an M3 pellet quantizes the mass (and so
        // the speed) change to a tick boundary, which is inherent to discrete
        // meals, not a dt bug — this canary asserts what it always has, that
        // the *continuous* dynamics are dt-scaled.
        w.tuning.target_pellet_count = 0;
        const auto id = sim::spawn(w, sim::EntityKind::Cell, {100.0f, 100.0f}, 10.0f,
                                   /*owner=*/1);
        EXPECT_NE(id, 0u);
        sim::apply_intent(w, sim::PlayerIntent{.player = 1, .direction = {1.0f, 0.0f}});
    };
    seed(coarse);
    seed(fine);

    for (int i = 0; i < 10; ++i) {
        sim::step(coarse, 0.1f);
    }
    for (int i = 0; i < 100; ++i) {
        sim::step(fine, 0.01f);
    }

    EXPECT_NEAR(coarse.entities.front().position.x, fine.entities.front().position.x, 0.5f);
}

TEST(World, HeavierCellsAreSlower)
{
    EXPECT_GT(sim::speed_for_mass(sim::default_tuning, 10.0f),
              sim::speed_for_mass(sim::default_tuning, 100.0f));
    EXPECT_GT(sim::speed_for_mass(sim::default_tuning, 100.0f),
              sim::speed_for_mass(sim::default_tuning, 1000.0f));
}

TEST(World, EntitiesStayInsideTheWorldSquare)
{
    sim::World w;
    w.tuning.target_pellet_count = 0;   // the clamp is the point; 200 ticks of pellet field is not
    sim::spawn(w, sim::EntityKind::Cell, {10.0f, 10.0f}, 10.0f, /*owner=*/1);
    sim::apply_intent(w, sim::PlayerIntent{.player = 1, .direction = {-1.0f, -1.0f}});

    for (int i = 0; i < 200; ++i) {
        sim::step(w, sim::tick_dt(w.tuning));
    }

    EXPECT_GE(w.entities.front().position.x, 0.0f);
    EXPECT_GE(w.entities.front().position.y, 0.0f);
}

TEST(World, TickCounterAdvancesOncePerStep)
{
    sim::World w;
    EXPECT_EQ(w.tick, 0u);
    sim::step(w, sim::tick_dt(w.tuning));
    sim::step(w, sim::tick_dt(w.tuning));
    EXPECT_EQ(w.tick, 2u);
}

TEST(World, LatestIntentReplacesThepreviousOne)
{
    sim::World w;
    sim::spawn(w, sim::EntityKind::Cell, {4000.0f, 4000.0f}, 10.0f, /*owner=*/1);
    sim::apply_intent(w, sim::PlayerIntent{.player = 1, .direction = {1.0f, 0.0f}});
    sim::step(w, sim::tick_dt(w.tuning));
    const float after_right = w.entities.front().position.x;

    sim::apply_intent(w, sim::PlayerIntent{.player = 1, .direction = {-1.0f, 0.0f}});
    sim::step(w, sim::tick_dt(w.tuning));
    EXPECT_LT(w.entities.front().position.x, after_right);
}

TEST(World, IdsAreMonotonicAndStartAtOne)
{
    // next_id is a public field now; nothing may hand out 0, which spawn()
    // callers and the wire format both treat as "no entity".
    sim::World w;
    EXPECT_EQ(w.next_id, 1u);

    const auto first = sim::spawn(w, sim::EntityKind::Pellet, {1.0f, 1.0f}, 1.0f);
    const auto second = sim::spawn(w, sim::EntityKind::Pellet, {2.0f, 2.0f}, 1.0f);
    EXPECT_EQ(first, 1u);
    EXPECT_EQ(second, 2u);
    EXPECT_EQ(w.entities.size(), 2u);
}

TEST(World, StepRebuildsTheGridOverCurrentPositions)
{
    // The M2 wiring check: after a step, the world's own grid answers
    // queries about where entities are *now* (post-integration), so the
    // broad phase M3 builds on is already in place.
    sim::World w;
    sim::spawn(w, sim::EntityKind::Cell, {1234.0f, 5678.0f}, 10.0f, /*owner=*/1);
    sim::apply_intent(w, sim::PlayerIntent{.player = 1, .direction = {1.0f, 0.0f}});
    sim::step(w, sim::tick_dt(w.tuning));

    const blob::math::Vec2 pos = w.entities.front().position;
    int hits = 0;
    std::uint32_t found = 0xffffffffu;
    sim::for_each_in_circle(w.grid, pos, 1.0f,
                            [&](std::uint32_t index, blob::math::Vec2) {
                                found = index;
                                ++hits;
                            });
    EXPECT_EQ(hits, 1);
    EXPECT_EQ(found, 0u);   // entry indices are positions in world.entities
}

TEST(Tuning, DefaultsAreSane)
{
    // The old drift test (tick_rate vs. a stored tick_dt) is impossible to
    // fail now that tick_dt is *derived* from the struct; what is still worth
    // pinning is that the shipped defaults describe a playable game.
    constexpr sim::Tuning t = sim::default_tuning;

    EXPECT_NEAR(sim::tick_dt(t), 0.05f, 1e-6f);   // 20 Hz

    EXPECT_GT(t.tick_rate, 0);
    EXPECT_GT(t.world_extent, 0.0f);
    EXPECT_GT(t.base_speed, 0.0f);
    EXPECT_GT(t.radius_factor, 0.0f);
    EXPECT_GT(t.grid_cell_size, 0.0f);
    // Negative on purpose: big = slow. A positive sign would invert the genre.
    EXPECT_LT(t.speed_mass_exponent, 0.0f);

    // Radius grows monotonically with mass, from exactly zero.
    EXPECT_EQ(sim::radius_for_mass(t, 0.0f), 0.0f);
    EXPECT_LT(sim::radius_for_mass(t, 10.0f), sim::radius_for_mass(t, 100.0f));
    EXPECT_LT(sim::radius_for_mass(t, 100.0f), sim::radius_for_mass(t, 1000.0f));

    // M3 knobs. eat_ratio strictly > 1, or equal cells would eat each other
    // on touch; the depth factor must be a real fraction — 0 would make rim
    // contact enough, 1 would demand more than full engulfment of the radius.
    EXPECT_GT(t.eat_ratio, 1.0f);
    EXPECT_GT(t.eat_depth_factor, 0.0f);
    EXPECT_LT(t.eat_depth_factor, 1.0f);
    EXPECT_GT(t.target_pellet_count, 0);
    EXPECT_GT(t.pellet_mass, 0.0f);
    EXPECT_GT(t.spawn_mass, 0.0f);
    EXPECT_GT(t.decay_threshold, 0.0f);
    EXPECT_GT(t.decay_rate, 0.0f);
    // A fresh spawn must sit below the decay line, or players would leak
    // mass from the moment they appear.
    EXPECT_LT(t.spawn_mass, t.decay_threshold);
}
