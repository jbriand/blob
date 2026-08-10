#include <blob/sim/world.hpp>

#include <gtest/gtest.h>

namespace sim = blob::sim;

TEST(World, StepIsFrameRateIndependent)
{
    // Same total time, different step counts -> same position. If this ever
    // fails, something in step() has started depending on call frequency.
    sim::World coarse;
    sim::World fine;

    const auto seed = [](sim::World& w) {
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
    EXPECT_GT(sim::speed_for_mass(10.0f), sim::speed_for_mass(100.0f));
    EXPECT_GT(sim::speed_for_mass(100.0f), sim::speed_for_mass(1000.0f));
}

TEST(World, EntitiesStayInsideTheWorldSquare)
{
    sim::World w;
    sim::spawn(w, sim::EntityKind::Cell, {10.0f, 10.0f}, 10.0f, /*owner=*/1);
    sim::apply_intent(w, sim::PlayerIntent{.player = 1, .direction = {-1.0f, -1.0f}});

    for (int i = 0; i < 200; ++i) {
        sim::step(w, sim::tick_dt);
    }

    EXPECT_GE(w.entities.front().position.x, 0.0f);
    EXPECT_GE(w.entities.front().position.y, 0.0f);
}

TEST(World, TickCounterAdvancesOncePerStep)
{
    sim::World w;
    EXPECT_EQ(w.tick, 0u);
    sim::step(w, sim::tick_dt);
    sim::step(w, sim::tick_dt);
    EXPECT_EQ(w.tick, 2u);
}

TEST(World, LatestIntentReplacesThepreviousOne)
{
    sim::World w;
    sim::spawn(w, sim::EntityKind::Cell, {4000.0f, 4000.0f}, 10.0f, /*owner=*/1);
    sim::apply_intent(w, sim::PlayerIntent{.player = 1, .direction = {1.0f, 0.0f}});
    sim::step(w, sim::tick_dt);
    const float after_right = w.entities.front().position.x;

    sim::apply_intent(w, sim::PlayerIntent{.player = 1, .direction = {-1.0f, 0.0f}});
    sim::step(w, sim::tick_dt);
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

TEST(World, TickConstantsAgreeWithEachOther)
{
    // tick_rate and tick_dt live side by side at namespace scope now, so
    // nothing stops the two drifting apart in an edit except this.
    EXPECT_NEAR(sim::tick_dt * static_cast<float>(sim::tick_rate), 1.0f, 1e-6f);
    EXPECT_GT(sim::world_extent, 0.0f);
}
