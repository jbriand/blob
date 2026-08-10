// M3's core loop: eat to grow, die when eaten. Every scenario runs through
// step(), so resolution always exercises the grid path (invariant 6), never a
// hand-rolled pair scan. Most tests switch the pellet field off
// (target_pellet_count = 0) so the board holds exactly what they placed on
// it; the field's own behavior is covered by the Pellets and Replay tests.

#include <blob/math/vec2.hpp>
#include <blob/sim/tuning.hpp>
#include <blob/sim/world.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sim = blob::sim;
namespace math = blob::math;

namespace {

/// A seeded world with the pellet field switched off — an empty arena that
/// contains exactly what the test spawns.
sim::World arena(std::uint32_t seed = 1u)
{
    sim::World w = sim::make_world(seed);
    w.tuning.target_pellet_count = 0;
    return w;
}

/// Compaction shifts indices and removes, so tests hold ids, never indices —
/// the same rule the header imposes on everyone else.
sim::Entity* find_entity(sim::World& w, sim::EntityId id)
{
    const auto it = std::ranges::find(w.entities, id, &sim::Entity::id);
    return it == w.entities.end() ? nullptr : &*it;
}

int count_kind(const sim::World& w, sim::EntityKind kind)
{
    return static_cast<int>(std::ranges::count(w.entities, kind, &sim::Entity::kind));
}

float total_mass(const sim::World& w)
{
    float sum = 0.0f;
    for (const sim::Entity& e : w.entities) {
        sum += e.mass;
    }
    return sum;
}

} // namespace

TEST(Eating, CellEatsCellOnlyAboveTheMassRatio)
{
    // Full overlap so the depth rule is trivially satisfied and the ratio is
    // the only gate. Against a mass-80 victim the gate sits at exactly 100:
    // 99.9 must starve, 100.1 must feast.
    {
        sim::World w = arena();
        const auto big = sim::spawn(w, sim::EntityKind::Cell, {1000.0f, 1000.0f}, 99.9f, 1);
        const auto small = sim::spawn(w, sim::EntityKind::Cell, {1000.0f, 1000.0f}, 80.0f, 2);
        sim::step(w, sim::tick_dt(w.tuning));

        EXPECT_NE(find_entity(w, big), nullptr);
        EXPECT_NE(find_entity(w, small), nullptr);
        EXPECT_TRUE(w.events.eats.empty());
        EXPECT_TRUE(w.events.deaths.empty());
    }
    {
        sim::World w = arena();
        const auto big = sim::spawn(w, sim::EntityKind::Cell, {1000.0f, 1000.0f}, 100.1f, 1);
        const auto small = sim::spawn(w, sim::EntityKind::Cell, {1000.0f, 1000.0f}, 80.0f, 2);
        sim::step(w, sim::tick_dt(w.tuning));

        const sim::Entity* eater = find_entity(w, big);
        ASSERT_NE(eater, nullptr);
        EXPECT_EQ(find_entity(w, small), nullptr);
        EXPECT_FLOAT_EQ(eater->mass, 180.1f);   // mass transferred, not destroyed
        ASSERT_EQ(w.events.eats.size(), 1u);
        EXPECT_EQ(w.events.eats.front().eater, big);
        EXPECT_EQ(w.events.eats.front().eaten, small);
    }
}

TEST(Eating, CellEatsCellOnlyPastTheDepthGate)
{
    // Ratio 2 (200 vs 100) so only depth decides. The reach limit is
    // r_e − depth·r_v = 4·√200 − (1/3)·40 ≈ 43.2352: a victim centre a
    // hair outside must survive, a hair inside must not. Margins of ~0.05
    // dwarf the float noise in the distance computation.
    {
        sim::World w = arena();
        sim::spawn(w, sim::EntityKind::Cell, {1000.0f, 1000.0f}, 200.0f, 1);
        const auto victim =
            sim::spawn(w, sim::EntityKind::Cell, {1043.29f, 1000.0f}, 100.0f, 2);
        sim::step(w, sim::tick_dt(w.tuning));

        EXPECT_NE(find_entity(w, victim), nullptr);
        EXPECT_TRUE(w.events.eats.empty());
    }
    {
        sim::World w = arena();
        const auto eater = sim::spawn(w, sim::EntityKind::Cell, {1000.0f, 1000.0f}, 200.0f, 1);
        const auto victim =
            sim::spawn(w, sim::EntityKind::Cell, {1043.18f, 1000.0f}, 100.0f, 2);
        sim::step(w, sim::tick_dt(w.tuning));

        EXPECT_EQ(find_entity(w, victim), nullptr);
        ASSERT_EQ(w.events.eats.size(), 1u);
        EXPECT_EQ(w.events.eats.front().eater, eater);
        EXPECT_EQ(w.events.eats.front().eaten, victim);
    }
}

TEST(Eating, PelletOnTheRimIsEaten)
{
    // Food needs only centre distance <= r_e, boundary inclusive — the same
    // convention the grid query uses. Mass 100 makes the reach exactly 40
    // (4·√100, every term exact in float), so a pellet at distance 40 is a
    // meal and one at 40.5 is not.
    sim::World w = arena();
    const auto eater = sim::spawn(w, sim::EntityKind::Cell, {2000.0f, 2000.0f}, 100.0f, 1);
    const auto on_rim = sim::spawn(w, sim::EntityKind::Pellet, {2040.0f, 2000.0f}, 1.0f);
    const auto beyond = sim::spawn(w, sim::EntityKind::Pellet, {1959.5f, 2000.0f}, 1.0f);
    sim::step(w, sim::tick_dt(w.tuning));

    EXPECT_EQ(find_entity(w, on_rim), nullptr);
    EXPECT_NE(find_entity(w, beyond), nullptr);
    const sim::Entity* cell = find_entity(w, eater);
    ASSERT_NE(cell, nullptr);
    EXPECT_FLOAT_EQ(cell->mass, 101.0f);
    ASSERT_EQ(w.events.eats.size(), 1u);
    EXPECT_EQ(w.events.eats.front().eaten, on_rim);
}

TEST(Eating, SameOwnerCellsNeverEatAndVirusesStayInert)
{
    // Ratio 15 at deep overlap would be an instant meal between rivals; the
    // shared owner alone must protect it. With both cooldowns expired the pair
    // sits outside the merge window (20 > merge_overlap·max(r)) and push-apart
    // is dormant (it runs only WHILE a cooldown runs — ROADMAP's rule, so that
    // steered remerging stays reachable): the overlap simply persists, and
    // neither mass nor an eat event may move. The virus overlaps at ratio
    // 1.5 — eatable if kind were ignored — and must sit there untouched until
    // M5's pop rule; same-owner resolution handles Cells only.
    sim::World w = arena();
    const auto big = sim::spawn(w, sim::EntityKind::Cell, {3000.0f, 3000.0f}, 150.0f, 5);
    const auto small = sim::spawn(w, sim::EntityKind::Cell, {3020.0f, 3000.0f}, 10.0f, 5);
    const auto virus = sim::spawn(w, sim::EntityKind::Virus, {3000.0f, 3000.0f}, 100.0f);
    sim::step(w, sim::tick_dt(w.tuning));

    ASSERT_EQ(w.entities.size(), 3u);
    EXPECT_TRUE(w.events.eats.empty());
    EXPECT_TRUE(w.events.deaths.empty());
    EXPECT_FLOAT_EQ(find_entity(w, big)->mass, 150.0f);
    EXPECT_FLOAT_EQ(find_entity(w, small)->mass, 10.0f);
    EXPECT_FLOAT_EQ(find_entity(w, virus)->mass, 100.0f);

    // Expired cooldowns, outside the merge window: nobody moves anybody.
    EXPECT_FLOAT_EQ(find_entity(w, big)->position.x, 3000.0f);
    EXPECT_FLOAT_EQ(find_entity(w, small)->position.x, 3020.0f);
    EXPECT_FLOAT_EQ(find_entity(w, virus)->position.x, 3000.0f);
}

TEST(Eating, ChainResolvesInArrayOrderAndConservesMass)
{
    // Three rivals stacked on one spot: mid (first in the array) eats small,
    // then big eats the grown mid. The ratio gate would clear either way
    // (110 >= 1.25*60 too); the recorded event order and the conserved total
    // are what prove
    // resolution ran in array order. Total mass lands in one survivor intact.
    sim::World w = arena();
    const auto mid = sim::spawn(w, sim::EntityKind::Cell, {4000.0f, 4000.0f}, 60.0f, 2);
    const auto small = sim::spawn(w, sim::EntityKind::Cell, {4000.0f, 4000.0f}, 20.0f, 3);
    const auto big = sim::spawn(w, sim::EntityKind::Cell, {4000.0f, 4000.0f}, 110.0f, 1);
    sim::step(w, sim::tick_dt(w.tuning));

    ASSERT_EQ(w.entities.size(), 1u);
    const sim::Entity* survivor = find_entity(w, big);
    ASSERT_NE(survivor, nullptr);
    EXPECT_FLOAT_EQ(survivor->mass, 190.0f);
    EXPECT_FLOAT_EQ(total_mass(w), 190.0f);   // 60 + 20 + 110, nothing leaked

    ASSERT_EQ(w.events.eats.size(), 2u);
    EXPECT_EQ(w.events.eats[0].eater, mid);
    EXPECT_EQ(w.events.eats[0].eaten, small);
    EXPECT_EQ(w.events.eats[1].eater, big);
    EXPECT_EQ(w.events.eats[1].eaten, mid);

    // Both victims were their players' last cells; set_difference emits the
    // ids sorted.
    EXPECT_EQ(w.events.deaths, (std::vector<sim::PlayerId>{2, 3}));
}

TEST(Eating, SurvivorsKeepTheirIdsAndIdsAreNeverReused)
{
    // Compaction removes the middle entity; the survivors' ids must ride
    // through unchanged and the next spawn must continue the monotonic
    // sequence — id 2 is retired forever, not recycled.
    sim::World w = arena();
    const auto first = sim::spawn(w, sim::EntityKind::Cell, {500.0f, 500.0f}, 130.0f, 1);
    const auto middle = sim::spawn(w, sim::EntityKind::Cell, {500.0f, 500.0f}, 100.0f, 2);
    const auto last = sim::spawn(w, sim::EntityKind::Cell, {7000.0f, 7000.0f}, 10.0f, 3);
    ASSERT_EQ(first, 1u);
    ASSERT_EQ(middle, 2u);
    ASSERT_EQ(last, 3u);

    sim::step(w, sim::tick_dt(w.tuning));

    ASSERT_EQ(w.entities.size(), 2u);
    EXPECT_NE(find_entity(w, first), nullptr);
    EXPECT_EQ(find_entity(w, middle), nullptr);
    EXPECT_NE(find_entity(w, last), nullptr);
    EXPECT_EQ(w.events.deaths, (std::vector<sim::PlayerId>{2}));

    const auto next = sim::spawn(w, sim::EntityKind::Pellet, {1.0f, 1.0f}, 1.0f);
    EXPECT_EQ(next, 4u);
}

TEST(Eating, EatingBothCellsInOneTickYieldsExactlyOneDeath)
{
    // A player's whole roster can fall inside a single tick; the death list
    // is sorted-unique, so the obituary still runs once.
    sim::World w = arena();
    sim::spawn(w, sim::EntityKind::Cell, {2000.0f, 2000.0f}, 150.0f, 1);
    sim::spawn(w, sim::EntityKind::Cell, {2000.0f, 2000.0f}, 100.0f, 7);
    sim::spawn(w, sim::EntityKind::Cell, {2000.0f, 2000.0f}, 10.0f, 7);
    sim::step(w, sim::tick_dt(w.tuning));

    EXPECT_EQ(w.events.eats.size(), 2u);
    EXPECT_EQ(w.events.deaths, (std::vector<sim::PlayerId>{7}));
}

TEST(Eating, DeathFiresOnlyWhenTheLastCellFalls)
{
    // A two-cell player losing one cell is wounded, not dead; the event must
    // wait for the roster to hit zero. Events are also re-derived every step,
    // so a quiet tick reports nothing.
    sim::World w = arena();
    const auto predator = sim::spawn(w, sim::EntityKind::Cell, {1000.0f, 1000.0f}, 150.0f, 1);
    const auto near_prey = sim::spawn(w, sim::EntityKind::Cell, {1000.0f, 1000.0f}, 100.0f, 7);
    const auto far_prey = sim::spawn(w, sim::EntityKind::Cell, {6000.0f, 6000.0f}, 10.0f, 7);

    sim::step(w, sim::tick_dt(w.tuning));
    ASSERT_EQ(w.events.eats.size(), 1u);
    EXPECT_EQ(w.events.eats.front().eaten, near_prey);
    EXPECT_TRUE(w.events.deaths.empty());   // player 7 still owns far_prey

    sim::step(w, sim::tick_dt(w.tuning));   // out of reach: a quiet tick
    EXPECT_TRUE(w.events.eats.empty());     // last step's news was cleared
    EXPECT_TRUE(w.events.deaths.empty());

    // Teleport the predator onto the survivor (tests may poke public fields;
    // the next step rebuilds the grid over whatever it finds).
    find_entity(w, predator)->position = {6000.0f, 6000.0f};
    sim::step(w, sim::tick_dt(w.tuning));

    ASSERT_EQ(w.events.eats.size(), 1u);
    EXPECT_EQ(w.events.eats.front().eaten, far_prey);
    EXPECT_EQ(w.events.deaths, (std::vector<sim::PlayerId>{7}));
}

TEST(Decay, IsFrameRateIndependent)
{
    // One simulated second sliced two ways must shed (near-)the same mass:
    // e^(−λ·dt) composes across any dt split, which is the whole reason for
    // the exponential form. NEAR, not ==: the two paths round differently,
    // and bit-equality across different dt splits is not something step()
    // promises (the invariant-3 canary in test_world makes the same call).
    sim::World coarse = arena();
    sim::World fine = arena();
    sim::spawn(coarse, sim::EntityKind::Cell, {4000.0f, 4000.0f}, 1000.0f, 1);
    sim::spawn(fine, sim::EntityKind::Cell, {4000.0f, 4000.0f}, 1000.0f, 1);

    for (int i = 0; i < 10; ++i) {
        sim::step(coarse, 0.1f);
    }
    for (int i = 0; i < 100; ++i) {
        sim::step(fine, 0.01f);
    }

    const float coarse_mass = coarse.entities.front().mass;
    const float fine_mass = fine.entities.front().mass;
    EXPECT_NEAR(coarse_mass, fine_mass, 0.01f);
    EXPECT_NEAR(coarse_mass, 1000.0f * 0.998002f, 0.05f);   // 1000·e^(−0.002·1)
}

TEST(Decay, FloorsAtTheThresholdAndSparesTheSmall)
{
    // decay_rate cranked so one big dt would overshoot the floor: the clamp
    // must land *exactly* on the threshold (it is an assignment, hence the
    // exact float comparisons). Mass at or below the line is never touched.
    sim::World w = arena();
    w.tuning.decay_rate = 1.0f;
    const auto heavy = sim::spawn(w, sim::EntityKind::Cell, {1000.0f, 1000.0f}, 500.0f, 1);
    const auto light = sim::spawn(w, sim::EntityKind::Cell, {7000.0f, 7000.0f}, 150.0f, 2);
    const auto at_line = sim::spawn(w, sim::EntityKind::Cell, {1000.0f, 7000.0f}, 200.0f, 3);
    sim::step(w, 1.0f);   // 500·e^−1 ≈ 183.9, well past the floor

    EXPECT_EQ(find_entity(w, heavy)->mass, 200.0f);
    EXPECT_EQ(find_entity(w, light)->mass, 150.0f);
    EXPECT_EQ(find_entity(w, at_line)->mass, 200.0f);
}

TEST(Pellets, FieldIsSownAndRestoredThroughTheSameIds)
{
    // A small field keeps the arithmetic checkable: the first step sows all
    // of it, a grazer parked on a known pellet eats everything in reach, and
    // the same step's respawn phase refills the field — with fresh ids.
    sim::World w = sim::make_world(20260810u);
    w.tuning.target_pellet_count = 64;

    sim::step(w, sim::tick_dt(w.tuning));
    ASSERT_EQ(count_kind(w, sim::EntityKind::Pellet), 64);
    ASSERT_EQ(w.next_id, 65u);

    // Park a grazer exactly on pellet #1: at least that one is in reach.
    const math::Vec2 spot = w.entities.front().position;
    const auto grazer = sim::spawn(w, sim::EntityKind::Cell, spot, 100.0f, 1);

    // Predict the meal count with the sim's own inclusive rule and radius.
    const float reach = sim::radius_for_mass(w.tuning, 100.0f);
    int expected_eats = 0;
    for (const sim::Entity& e : w.entities) {
        if (e.kind == sim::EntityKind::Pellet &&
            math::length_sq(e.position - spot) <= reach * reach) {
            ++expected_eats;
        }
    }
    ASSERT_GE(expected_eats, 1);

    sim::step(w, sim::tick_dt(w.tuning));

    EXPECT_EQ(count_kind(w, sim::EntityKind::Pellet), 64);   // restored same step
    EXPECT_EQ(w.events.eats.size(), static_cast<std::size_t>(expected_eats));
    EXPECT_FLOAT_EQ(find_entity(w, grazer)->mass,
                    100.0f + static_cast<float>(expected_eats) * w.tuning.pellet_mass);
    // Replacements are new entities: grazer took 65, each respawn advanced
    // the sequence past it. Nothing was recycled.
    EXPECT_EQ(w.next_id, 66u + static_cast<sim::EntityId>(expected_eats));
}

TEST(PlayerLifecycle, SpawnPlacesOneStartingCellInsideTheWorld)
{
    sim::World w = arena(9u);
    const auto id = sim::spawn_player(w, 42);

    ASSERT_EQ(w.entities.size(), 1u);
    const sim::Entity& cell = w.entities.front();
    EXPECT_EQ(cell.id, id);
    EXPECT_EQ(cell.owner, 42);
    EXPECT_EQ(cell.kind, sim::EntityKind::Cell);
    EXPECT_FLOAT_EQ(cell.mass, w.tuning.spawn_mass);
    EXPECT_GE(cell.position.x, 0.0f);
    EXPECT_LT(cell.position.x, w.tuning.world_extent);
    EXPECT_GE(cell.position.y, 0.0f);
    EXPECT_LT(cell.position.y, w.tuning.world_extent);
}

TEST(PlayerLifecycle, DespawnIsImmediateSilentAndTakesTheIntentAlong)
{
    sim::World w = arena(10u);
    const auto keeper = sim::spawn_player(w, 1);
    sim::spawn_player(w, 2);
    sim::apply_intent(w, sim::PlayerIntent{.player = 2, .direction = {1.0f, 0.0f}});
    sim::step(w, sim::tick_dt(w.tuning));

    sim::despawn_player(w, 2);
    ASSERT_EQ(w.entities.size(), 1u);   // gone before any step, not marked
    EXPECT_NE(find_entity(w, keeper), nullptr);

    // Disconnect must never read as a kill.
    sim::step(w, sim::tick_dt(w.tuning));
    EXPECT_TRUE(w.events.deaths.empty());

    // The old intent went with the player: a respawned player 2 must sit
    // still, not inherit the previous owner's ghost cursor.
    const auto respawned = sim::spawn_player(w, 2);
    const math::Vec2 before = find_entity(w, respawned)->position;
    sim::step(w, sim::tick_dt(w.tuning));
    const math::Vec2 after = find_entity(w, respawned)->position;
    EXPECT_EQ(before.x, after.x);
    EXPECT_EQ(before.y, after.y);
}

TEST(Replay, SameSeedAndScriptReplayIdentically)
{
    // The crown jewel and the cheapest desync detector there is — later the
    // foundation for client-side prediction: a world must be a pure function
    // of (seed, call sequence). Exact float == is legitimate because both
    // runs are the same binary executing the same instruction stream; the
    // cross-platform claim is explicitly not made (ROADMAP, cross-cutting
    // rules). The script exercises everything M3 added: a live pellet field,
    // pellet meals, a cell kill with its death event, and mid-run lifecycle
    // churn — all of which consume world.rng in replayed order.
    struct Tally {
        std::size_t eats{};
        std::size_t deaths{};
    };
    const auto run_script = [](sim::World& w) {
        Tally tally;
        sim::spawn_player(w, 1);
        sim::spawn_player(w, 2);
        // A planted meal: big rival over a doomed small one, so at least one
        // cell eat and one death happen regardless of where players wander.
        sim::spawn(w, sim::EntityKind::Cell, {4096.0f, 4096.0f}, 150.0f, 3);
        sim::spawn(w, sim::EntityKind::Cell, {4096.0f, 4096.0f}, 20.0f, 4);
        const auto dir = [](int tick, int player) -> math::Vec2 {
            switch ((tick / 25 + player) % 4) {
            case 0:  return {1.0f, 0.0f};
            case 1:  return {0.0f, 1.0f};
            case 2:  return {-1.0f, 0.0f};
            default: return {0.0f, -1.0f};
            }
        };
        for (int t = 0; t < 200; ++t) {
            sim::apply_intent(w, sim::PlayerIntent{.player = 1, .direction = dir(t, 1)});
            sim::apply_intent(w, sim::PlayerIntent{.player = 2, .direction = dir(t, 2)});
            if (t == 100) {
                sim::despawn_player(w, 2);   // lifecycle is part of the script
            }
            if (t == 120) {
                sim::spawn_player(w, 2);     // and so is the reconnect
            }
            sim::step(w, sim::tick_dt(w.tuning));
            tally.eats += w.events.eats.size();
            tally.deaths += w.events.deaths.size();
        }
        return tally;
    };

    sim::World a = sim::make_world(424242u);
    sim::World b = sim::make_world(424242u);
    const Tally tally_a = run_script(a);
    const Tally tally_b = run_script(b);

    EXPECT_GT(tally_a.eats, 0u);      // the script really ate
    EXPECT_GT(tally_a.deaths, 0u);    // and really killed
    EXPECT_EQ(tally_a.eats, tally_b.eats);
    EXPECT_EQ(tally_a.deaths, tally_b.deaths);

    EXPECT_EQ(a.tick, b.tick);
    EXPECT_EQ(a.next_id, b.next_id);
    EXPECT_TRUE(a.rng == b.rng);      // the generators advanced in lockstep
    ASSERT_EQ(a.entities.size(), b.entities.size());
    for (std::size_t i = 0; i < a.entities.size(); ++i) {
        const sim::Entity& ea = a.entities[i];
        const sim::Entity& eb = b.entities[i];
        EXPECT_EQ(ea.id, eb.id) << "entity " << i;
        EXPECT_EQ(ea.owner, eb.owner) << "entity " << i;
        EXPECT_EQ(ea.kind, eb.kind) << "entity " << i;
        EXPECT_EQ(ea.position.x, eb.position.x) << "entity " << i;
        EXPECT_EQ(ea.position.y, eb.position.y) << "entity " << i;
        EXPECT_EQ(ea.mass, eb.mass) << "entity " << i;
    }
}
