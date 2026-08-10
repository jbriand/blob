// M4's skill layer: split, eject, merge. Every scenario runs through step()
// so the action phase, the exponential impulse glide and the same-owner
// resolution are exercised end to end, never poked in isolation. Worlds run
// pellet-free (target_pellet_count = 0) wherever counts or exact totals
// matter; the live-field case is the replay test at the bottom.

#include <blob/math/vec2.hpp>
#include <blob/sim/tuning.hpp>
#include <blob/sim/world.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
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

sim::Entity* find_kind(sim::World& w, sim::EntityKind kind)
{
    const auto it = std::ranges::find(w.entities, kind, &sim::Entity::kind);
    return it == w.entities.end() ? nullptr : &*it;
}

const sim::PlayerIntent* find_intent(const sim::World& w, sim::PlayerId player)
{
    const auto it = std::ranges::find(w.intents, player, &sim::PlayerIntent::player);
    return it == w.intents.end() ? nullptr : &*it;
}

int count_kind(const sim::World& w, sim::EntityKind kind)
{
    return static_cast<int>(std::ranges::count(w.entities, kind, &sim::Entity::kind));
}

int count_player_cells(const sim::World& w, sim::PlayerId player)
{
    int n = 0;
    for (const sim::Entity& e : w.entities) {
        if (e.kind == sim::EntityKind::Cell && e.owner == player) {
            ++n;
        }
    }
    return n;
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

TEST(Split, HalvesConserveMassExactlyAndBothCarryTheCooldown)
{
    // 100 halves to 50 + 50 — float halving is exact, so the totals compare
    // with ==, not a tolerance: conservation is by construction.
    sim::World w = arena();
    const auto parent = sim::spawn(w, sim::EntityKind::Cell, {1000.0f, 1000.0f}, 100.0f, 1);
    sim::apply_intent(w, sim::PlayerIntent{.player = 1, .direction = {1.0f, 0.0f}, .split = true});
    const float dt = sim::tick_dt(w.tuning);
    sim::step(w, dt);

    ASSERT_EQ(w.entities.size(), 2u);
    EXPECT_EQ(total_mass(w), 100.0f);
    const sim::Entity* elder = find_entity(w, parent);
    const sim::Entity* half = find_entity(w, parent + 1);   // ids are monotonic: the half took the next one
    ASSERT_NE(elder, nullptr);
    ASSERT_NE(half, nullptr);
    EXPECT_EQ(elder->mass, 50.0f);
    EXPECT_EQ(half->mass, 50.0f);

    // Both halves carry the mass-scaled cooldown, already one dt into its
    // countdown — the arming tick decrements too, dt-scaled like everything.
    const float armed =
        w.tuning.merge_cooldown_base + w.tuning.merge_cooldown_per_mass * 50.0f - dt;
    EXPECT_FLOAT_EQ(elder->merge_cooldown, armed);
    EXPECT_FLOAT_EQ(half->merge_cooldown, armed);
}

TEST(Split, BurstLandsExactlyOnTheCapAndAtCapRefuses)
{
    // Four doublings reach the cap from one cell; the fifth application must
    // refuse even though every cell is still heavy enough (600/16 = 37.5 ≥
    // min_split_mass, give or take decay dust) — the refusal below is the
    // cap's, not the mass gate's.
    sim::World w = arena();
    sim::spawn(w, sim::EntityKind::Cell, {4096.0f, 4096.0f}, 600.0f, 1);

    const std::array<int, 5> expected{2, 4, 8, 16, 16};
    for (std::size_t round = 0; round < expected.size(); ++round) {
        sim::apply_intent(w, sim::PlayerIntent{.player = 1, .split = true});
        sim::step(w, sim::tick_dt(w.tuning));
        EXPECT_EQ(count_player_cells(w, 1), expected[round]) << "round " << round;
        EXPECT_LE(count_player_cells(w, 1), w.tuning.max_cells_per_player);
    }
}

TEST(Split, CapTruncationFollowsArrayOrder)
{
    // Twelve cells, room for four more: exactly the first four in array
    // (= spawn) order split — that order is the documented deterministic
    // priority, so the halved masses must sit on ids 1..4 and nowhere else.
    sim::World w = arena();
    for (int i = 0; i < 12; ++i) {
        sim::spawn(w, sim::EntityKind::Cell,
                   {500.0f + 600.0f * static_cast<float>(i), 400.0f}, 100.0f, 1);
    }
    sim::apply_intent(w, sim::PlayerIntent{.player = 1, .split = true});
    sim::step(w, sim::tick_dt(w.tuning));

    EXPECT_EQ(count_player_cells(w, 1), 16);
    for (sim::EntityId id = 1; id <= 12; ++id) {
        const sim::Entity* cell = find_entity(w, id);
        ASSERT_NE(cell, nullptr);
        EXPECT_EQ(cell->mass, id <= 4 ? 50.0f : 100.0f) << "id " << id;
    }
}

TEST(Split, FlagIsAOneShotEdge)
{
    // One latched press, three ticks: exactly one split. The halves stay
    // above min_split_mass on purpose — if the flag ever re-fired, two cells
    // would become four and this would fail loudly.
    sim::World w = arena();
    sim::spawn(w, sim::EntityKind::Cell, {2000.0f, 2000.0f}, 100.0f, 1);
    sim::apply_intent(w, sim::PlayerIntent{.player = 1, .direction = {1.0f, 0.0f}, .split = true});

    sim::step(w, sim::tick_dt(w.tuning));
    EXPECT_EQ(count_player_cells(w, 1), 2);

    // The step consumed the flags but kept the steering: actions are edges,
    // direction is a level.
    const sim::PlayerIntent* intent = find_intent(w, 1);
    ASSERT_NE(intent, nullptr);
    EXPECT_FALSE(intent->split);
    EXPECT_FALSE(intent->eject);
    EXPECT_FLOAT_EQ(intent->direction.x, 1.0f);

    sim::step(w, sim::tick_dt(w.tuning));
    sim::step(w, sim::tick_dt(w.tuning));
    EXPECT_EQ(count_player_cells(w, 1), 2);
}

TEST(Split, KickFollowsIntentAndZeroIntentSplitsInPlace)
{
    // Directed: the new half carries impulse along the intent, one decay
    // tick in — velocity is rewritten by steering every tick, which is
    // exactly why the kick lives in its own field.
    {
        sim::World w = arena();
        const auto parent = sim::spawn(w, sim::EntityKind::Cell, {3000.0f, 3000.0f}, 100.0f, 1);
        sim::apply_intent(w,
                          sim::PlayerIntent{.player = 1, .direction = {0.0f, 1.0f}, .split = true});
        const float dt = sim::tick_dt(w.tuning);
        sim::step(w, dt);

        const sim::Entity* elder = find_entity(w, parent);
        const sim::Entity* half = find_entity(w, parent + 1);
        ASSERT_NE(elder, nullptr);
        ASSERT_NE(half, nullptr);
        EXPECT_EQ(half->impulse.x, 0.0f);
        EXPECT_FLOAT_EQ(half->impulse.y,
                        w.tuning.split_impulse_speed *
                            std::exp(-w.tuning.impulse_damping_rate * dt));
        EXPECT_GT(half->position.y, elder->position.y);   // launched ahead along the intent
    }
    // Zero intent: the split still happens — the count grows — but nobody
    // flies; push-apart provides the separation, along +x (the documented
    // coincident-centres tiebreak), half the penetration each way.
    {
        sim::World w = arena();
        const auto parent = sim::spawn(w, sim::EntityKind::Cell, {3000.0f, 3000.0f}, 100.0f, 1);
        sim::apply_intent(w, sim::PlayerIntent{.player = 1, .split = true});
        sim::step(w, sim::tick_dt(w.tuning));

        ASSERT_EQ(count_player_cells(w, 1), 2);
        const sim::Entity* elder = find_entity(w, parent);
        const sim::Entity* half = find_entity(w, parent + 1);
        ASSERT_NE(elder, nullptr);
        ASSERT_NE(half, nullptr);
        EXPECT_EQ(half->impulse.x, 0.0f);
        EXPECT_EQ(half->impulse.y, 0.0f);
        const float r = sim::radius_for_mass(w.tuning, 50.0f);
        EXPECT_FLOAT_EQ(elder->position.x, 3000.0f - r);
        EXPECT_FLOAT_EQ(half->position.x, 3000.0f + r);
        EXPECT_FLOAT_EQ(elder->position.y, 3000.0f);
        EXPECT_FLOAT_EQ(half->position.y, 3000.0f);
    }
}

TEST(Split, ImpulseDecaysToSteeringOnly)
{
    // After 4 s the kick is dead (e^(−3.5·4) ≈ 8·10⁻⁷ of it left): the
    // launched half's per-tick displacement must match its plain-steering
    // sibling's — equal-mass halves steer at the same speed, so only a
    // surviving impulse could tell them apart.
    sim::World w = arena();
    const auto parent = sim::spawn(w, sim::EntityKind::Cell, {1000.0f, 4000.0f}, 40.0f, 1);
    sim::apply_intent(w, sim::PlayerIntent{.player = 1, .direction = {1.0f, 0.0f}, .split = true});
    const float dt = sim::tick_dt(w.tuning);
    for (int i = 0; i < 80; ++i) {   // the direction keeps steering +x; flags fired once
        sim::step(w, dt);
    }

    const float elder_x = find_entity(w, parent)->position.x;
    const float half_x = find_entity(w, parent + 1)->position.x;
    sim::step(w, dt);
    const float elder_delta = find_entity(w, parent)->position.x - elder_x;
    const float half_delta = find_entity(w, parent + 1)->position.x - half_x;

    EXPECT_NEAR(half_delta, elder_delta, 5e-3f);
    EXPECT_NEAR(half_delta, sim::speed_for_mass(w.tuning, 20.0f) * dt, 5e-3f);
}

TEST(Eject, PaysTheCostAndTheDifferenceEvaporates)
{
    sim::World w = arena();
    const auto cell = sim::spawn(w, sim::EntityKind::Cell, {2000.0f, 2000.0f}, 100.0f, 1);
    sim::apply_intent(w, sim::PlayerIntent{.player = 1, .direction = {1.0f, 0.0f}, .eject = true});
    sim::step(w, sim::tick_dt(w.tuning));

    ASSERT_EQ(count_kind(w, sim::EntityKind::EjectedMass), 1);
    const sim::Entity* payer = find_entity(w, cell);
    const sim::Entity* pellet = find_entity(w, cell + 1);
    ASSERT_NE(payer, nullptr);
    ASSERT_NE(pellet, nullptr);
    EXPECT_EQ(payer->mass, 100.0f - w.tuning.eject_mass_cost);
    EXPECT_EQ(pellet->mass, w.tuning.ejected_mass);
    // Yours: you may eat it back, and M5's viruses attribute feeds by owner.
    EXPECT_EQ(pellet->owner, 1);
    // The ledger: cost in, carried out, the difference evaporates — ejecting
    // must never print mass (== on the total, the arithmetic is exact).
    EXPECT_EQ(total_mass(w), 100.0f - (w.tuning.eject_mass_cost - w.tuning.ejected_mass));
}

TEST(Eject, NeedsBothTheMinimumMassAndADirection)
{
    // A hair under min_eject_mass: nothing happens at all.
    {
        sim::World w = arena();
        const auto cell = sim::spawn(w, sim::EntityKind::Cell, {2000.0f, 2000.0f}, 34.9f, 1);
        sim::apply_intent(w,
                          sim::PlayerIntent{.player = 1, .direction = {1.0f, 0.0f}, .eject = true});
        sim::step(w, sim::tick_dt(w.tuning));

        EXPECT_EQ(count_kind(w, sim::EntityKind::EjectedMass), 0);
        EXPECT_EQ(find_entity(w, cell)->mass, 34.9f);
    }
    // Zero direction: "hold still" has no aim — no pellet, and no cost paid.
    {
        sim::World w = arena();
        const auto cell = sim::spawn(w, sim::EntityKind::Cell, {2000.0f, 2000.0f}, 100.0f, 1);
        sim::apply_intent(w, sim::PlayerIntent{.player = 1, .eject = true});
        sim::step(w, sim::tick_dt(w.tuning));

        EXPECT_EQ(count_kind(w, sim::EntityKind::EjectedMass), 0);
        EXPECT_EQ(find_entity(w, cell)->mass, 100.0f);
    }
}

TEST(Eject, TravelIsFrameRateIndependent)
{
    // One simulated second sliced 10×0.1 vs 100×0.01 must land the pellet in
    // (near-)the same spot — the e^(−λ·dt) canary for the new mechanic. The
    // per-step displacement is the exact integral of the decaying flight,
    // which telescopes across any slicing to v·(1−e^(−λT))/λ; a rectangle
    // v·dt step (or a per-tick damping factor) would drift by double-digit
    // percentages between these two slicings and fail far outside the
    // tolerance. NEAR, not ==: the two paths round differently, exactly as
    // the decay and movement canaries accept.
    sim::World coarse = arena();
    sim::World fine = arena();
    const auto seed_world = [](sim::World& w) {
        sim::spawn(w, sim::EntityKind::Cell, {2000.0f, 4000.0f}, 100.0f, 1);
        sim::apply_intent(w,
                          sim::PlayerIntent{.player = 1, .direction = {1.0f, 0.0f}, .eject = true});
    };
    seed_world(coarse);
    seed_world(fine);

    for (int i = 0; i < 10; ++i) {
        sim::step(coarse, 0.1f);
    }
    for (int i = 0; i < 100; ++i) {
        sim::step(fine, 0.01f);
    }

    sim::Entity* coarse_pellet = find_kind(coarse, sim::EntityKind::EjectedMass);
    sim::Entity* fine_pellet = find_kind(fine, sim::EntityKind::EjectedMass);
    ASSERT_NE(coarse_pellet, nullptr);
    ASSERT_NE(fine_pellet, nullptr);
    EXPECT_NEAR(coarse_pellet->position.x, fine_pellet->position.x, 0.5f);

    // And the absolute distance matches the closed form v·(1−e^(−λT))/λ,
    // measured from the rim-to-rim launch point.
    const sim::Tuning& t = coarse.tuning;
    const float launch_x = 2000.0f + sim::radius_for_mass(t, 100.0f - t.eject_mass_cost) +
                           sim::radius_for_mass(t, t.ejected_mass);
    const float expected =
        t.eject_speed * (1.0f - std::exp(-t.impulse_damping_rate)) / t.impulse_damping_rate;
    EXPECT_NEAR(coarse_pellet->position.x - launch_x, expected, 0.5f);
}

TEST(Merge, GatedByCooldownThenElderAbsorbsYounger)
{
    // While either timer runs, deep overlap is resolved by push-apart — the
    // distance grows — and only once both floor at exactly 0 does the same
    // overlap fuse the pair: elder id survives, mass sums exactly, the count
    // drops. Time is driven by stepping; only positions are staged.
    sim::World w = arena();
    const auto parent = sim::spawn(w, sim::EntityKind::Cell, {4000.0f, 4000.0f}, 100.0f, 1);
    sim::apply_intent(w, sim::PlayerIntent{.player = 1, .split = true});
    const float dt = sim::tick_dt(w.tuning);
    sim::step(w, dt);
    const auto child = parent + 1;
    ASSERT_EQ(count_player_cells(w, 1), 2);

    // Mid-cooldown, force a full overlap: the pair must push apart, not merge.
    find_entity(w, child)->position = find_entity(w, parent)->position;
    sim::step(w, dt);
    ASSERT_EQ(count_player_cells(w, 1), 2);
    EXPECT_GT(find_entity(w, parent)->merge_cooldown, 0.0f);
    const math::Vec2 gap =
        find_entity(w, child)->position - find_entity(w, parent)->position;
    // Pushed back out to exactly touching — the overlap-created distance grew.
    EXPECT_NEAR(math::length(gap), 2.0f * sim::radius_for_mass(w.tuning, 50.0f), 0.01f);

    // Live out the timers (11 s at the defaults for 50-mass halves; the
    // margin covers float dust in the repeated 0.05 decrements). Parked at
    // touching distance the pair never merges on its own: the default window
    // demands deep overlap, not mere contact.
    for (int i = 0; i < 240; ++i) {
        sim::step(w, dt);
    }
    ASSERT_EQ(count_player_cells(w, 1), 2);
    EXPECT_EQ(find_entity(w, parent)->merge_cooldown, 0.0f);   // floored exactly, by assignment
    EXPECT_EQ(find_entity(w, child)->merge_cooldown, 0.0f);

    // Same overlap as before, now with both timers expired: fuse.
    find_entity(w, child)->position = find_entity(w, parent)->position;
    sim::step(w, dt);

    EXPECT_EQ(count_player_cells(w, 1), 1);
    const sim::Entity* survivor = find_entity(w, parent);
    ASSERT_NE(survivor, nullptr);
    EXPECT_EQ(find_entity(w, child), nullptr);   // the younger is gone for good
    EXPECT_EQ(survivor->mass, 100.0f);           // 50 + 50, exact
    EXPECT_TRUE(w.events.eats.empty());          // a merge is not an eat...
    EXPECT_TRUE(w.events.deaths.empty());        // ...and can never be a death
}

TEST(Merge, NeedsDeepOverlapNotMereTouch)
{
    // Unequal siblings pin the max(r_a, r_b) in the rule: 100 (r 40) and 25
    // (r 20) merge only within 0.25·40 = 10 units. Freshly spawned cells
    // carry expired cooldowns, so distance alone decides — and note the
    // ratio gate (100 ≥ 1.25·25) would make this pair a meal between rivals;
    // the shared owner is what turns eating into merging.
    {
        // A hair outside the window: two cells stay two cells, pushed back
        // to touching.
        sim::World w = arena();
        const auto a = sim::spawn(w, sim::EntityKind::Cell, {5000.0f, 5000.0f}, 100.0f, 1);
        const auto b = sim::spawn(w, sim::EntityKind::Cell, {5010.5f, 5000.0f}, 25.0f, 1);
        sim::step(w, sim::tick_dt(w.tuning));

        EXPECT_EQ(count_player_cells(w, 1), 2);
        EXPECT_TRUE(w.events.eats.empty());
        EXPECT_EQ(find_entity(w, a)->mass, 100.0f);
        EXPECT_EQ(find_entity(w, b)->mass, 25.0f);
        const float gap = find_entity(w, b)->position.x - find_entity(w, a)->position.x;
        EXPECT_NEAR(gap,
                    sim::radius_for_mass(w.tuning, 100.0f) + sim::radius_for_mass(w.tuning, 25.0f),
                    0.01f);
    }
    {
        // A hair inside: the elder absorbs, holds its ground, and no eat
        // event or death is recorded.
        sim::World w = arena();
        const auto a = sim::spawn(w, sim::EntityKind::Cell, {5000.0f, 5000.0f}, 100.0f, 1);
        const auto b = sim::spawn(w, sim::EntityKind::Cell, {5009.5f, 5000.0f}, 25.0f, 1);
        sim::step(w, sim::tick_dt(w.tuning));

        EXPECT_EQ(count_player_cells(w, 1), 1);
        const sim::Entity* survivor = find_entity(w, a);
        ASSERT_NE(survivor, nullptr);
        EXPECT_EQ(find_entity(w, b), nullptr);
        EXPECT_EQ(survivor->mass, 125.0f);
        EXPECT_EQ(survivor->position.x, 5000.0f);
        EXPECT_EQ(survivor->position.y, 5000.0f);
        EXPECT_TRUE(w.events.eats.empty());
        EXPECT_TRUE(w.events.deaths.empty());
    }
}

TEST(Replay, SplitEjectMergeReplayIdentically)
{
    // The M3 replay test's sibling (that one stands untouched next door),
    // extended with M4's whole vocabulary: splits — including refusals from
    // the under-mass starter cell — ejects, push-apart, merges, the planted
    // kill and mid-run lifecycle churn, all over a live pellet field. Exact
    // equality is same-binary determinism (invariant 3), and the comparison
    // covers the two new Entity fields.
    //
    // Two knobs are widened for the script's sake, identically in both
    // worlds: full positional correction parks post-cooldown siblings at
    // touching distance, outside the default deep-overlap merge window, so
    // reaching a merge inside 300 scripted ticks takes a shorter cooldown
    // and a wider window — not mid-run teleports.
    struct Tally {
        std::size_t   eats{};
        std::size_t   deaths{};
        int           peak_cells{};
        int           peak_ejected{};
        sim::EntityId younger_half{};
        bool          younger_half_eaten{};
    };
    const auto run_script = [](sim::World& w) {
        Tally tally;
        w.tuning.merge_cooldown_base = 1.0f;
        w.tuning.merge_overlap = 2.5f;
        sim::spawn_player(w, 1);   // the mass-10 starter: refuses every split/eject
        sim::spawn_player(w, 2);
        // Player 1's war chest: heavy enough to split and eject.
        sim::spawn(w, sim::EntityKind::Cell, {4000.0f, 4000.0f}, 120.0f, 1);
        // The planted meal between rivals, as in the M3 script.
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
        for (int t = 0; t < 300; ++t) {
            const bool split = t == 10 || t == 60;
            const bool eject = t == 30 || t == 90;
            // The t == 10 split keeps its directed kick, so the replay also
            // exercises impulse decay — which glides that pair ~220 units
            // apart, where parallel steering never re-converges it: those
            // halves live on side by side. The t == 60 split is zero-intent
            // on purpose: its pairs park at touching distance under
            // push-apart, inside this script's widened merge window, and
            // fuse when the cooldowns floor — the merges the guard below
            // demands.
            const math::Vec2 steer = t == 60 ? math::Vec2{} : dir(t, 1);
            sim::apply_intent(w, sim::PlayerIntent{.player = 1,
                                                   .direction = steer,
                                                   .split = split,
                                                   .eject = eject});
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
            int cells = 0;
            int ejected = 0;
            sim::EntityId newest_cell = 0;
            for (const sim::Entity& e : w.entities) {
                if (e.kind == sim::EntityKind::Cell && e.owner == 1) {
                    ++cells;
                    newest_cell = std::max(newest_cell, e.id);
                }
                if (e.kind == sim::EntityKind::EjectedMass) {
                    ++ejected;
                }
            }
            tally.peak_cells = std::max(tally.peak_cells, cells);
            tally.peak_ejected = std::max(tally.peak_ejected, ejected);
            if (t == 60) {
                tally.younger_half = newest_cell;   // the last id the zero-intent split minted
            }
            for (const sim::EatEvent& eat : w.events.eats) {
                if (eat.eaten == tally.younger_half) {
                    tally.younger_half_eaten = true;
                }
            }
        }
        return tally;
    };

    sim::World a = sim::make_world(434343u);
    sim::World b = sim::make_world(434343u);
    const Tally tally_a = run_script(a);
    const Tally tally_b = run_script(b);

    // The script really did the things: a kill, splits past four cells, an
    // eject, and — the younger half being gone without ever being somebody's
    // meal — a merge.
    EXPECT_GT(tally_a.eats, 0u);
    EXPECT_GT(tally_a.deaths, 0u);
    EXPECT_GE(tally_a.peak_cells, 4);
    EXPECT_GE(tally_a.peak_ejected, 2);
    ASSERT_NE(tally_a.younger_half, 0u);
    EXPECT_EQ(find_entity(a, tally_a.younger_half), nullptr);
    EXPECT_FALSE(tally_a.younger_half_eaten);

    EXPECT_EQ(tally_a.eats, tally_b.eats);
    EXPECT_EQ(tally_a.deaths, tally_b.deaths);
    EXPECT_EQ(tally_a.peak_cells, tally_b.peak_cells);
    EXPECT_EQ(tally_a.peak_ejected, tally_b.peak_ejected);
    EXPECT_EQ(tally_a.younger_half, tally_b.younger_half);

    EXPECT_EQ(a.tick, b.tick);
    EXPECT_EQ(a.next_id, b.next_id);
    EXPECT_TRUE(a.rng == b.rng);   // the generators advanced in lockstep
    ASSERT_EQ(a.entities.size(), b.entities.size());
    for (std::size_t i = 0; i < a.entities.size(); ++i) {
        const sim::Entity& ea = a.entities[i];
        const sim::Entity& eb = b.entities[i];
        EXPECT_EQ(ea.id, eb.id) << "entity " << i;
        EXPECT_EQ(ea.owner, eb.owner) << "entity " << i;
        EXPECT_EQ(ea.kind, eb.kind) << "entity " << i;
        EXPECT_EQ(ea.position.x, eb.position.x) << "entity " << i;
        EXPECT_EQ(ea.position.y, eb.position.y) << "entity " << i;
        EXPECT_EQ(ea.velocity.x, eb.velocity.x) << "entity " << i;
        EXPECT_EQ(ea.velocity.y, eb.velocity.y) << "entity " << i;
        EXPECT_EQ(ea.impulse.x, eb.impulse.x) << "entity " << i;
        EXPECT_EQ(ea.impulse.y, eb.impulse.y) << "entity " << i;
        EXPECT_EQ(ea.mass, eb.mass) << "entity " << i;
        EXPECT_EQ(ea.merge_cooldown, eb.merge_cooldown) << "entity " << i;
    }
}
