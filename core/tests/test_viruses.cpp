// M5's hazard layer: viruses and safe spawn. Every scenario runs through
// step() so the pop gates, the burst, the feeding phase and the refill are
// exercised end to end through the grid (invariant 6), never poked in
// isolation. Worlds run with BOTH maintained fields off (pellets and
// viruses) wherever counts or exact totals matter, spawning viruses by
// hand instead — the live-field case is the replay test at the bottom.

#include <blob/math/vec2.hpp>
#include <blob/sim/tuning.hpp>
#include <blob/sim/world.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <random>
#include <vector>

namespace sim = blob::sim;
namespace math = blob::math;

namespace {

/// A seeded world with both maintained fields switched off — an empty arena
/// that contains exactly what the test spawns (the same convention the
/// eating tests use for pellets, extended to M5's virus field).
sim::World arena(std::uint32_t seed = 1u)
{
    sim::World w = sim::make_world(seed);
    w.tuning.target_pellet_count = 0;
    w.tuning.target_virus_count = 0;
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

TEST(VirusField, RefillsToTheTargetAndNeverCulls)
{
    // The field is maintained exactly like pellets: one step sows the whole
    // target. Everything about a sown virus is pinned — unowned, virus_mass,
    // inside the square.
    sim::World w = arena();
    w.tuning.target_virus_count = 3;
    sim::step(w, sim::tick_dt(w.tuning));

    ASSERT_EQ(count_kind(w, sim::EntityKind::Virus), 3);
    for (const sim::Entity& e : w.entities) {
        EXPECT_EQ(e.kind, sim::EntityKind::Virus);
        EXPECT_EQ(e.owner, 0);
        EXPECT_FLOAT_EQ(e.mass, w.tuning.virus_mass);
        EXPECT_GE(e.position.x, 0.0f);
        EXPECT_LT(e.position.x, w.tuning.world_extent);
        EXPECT_GE(e.position.y, 0.0f);
        EXPECT_LT(e.position.y, w.tuning.world_extent);
    }

    // A fourth virus — standing in for a feed-split pushing the field over
    // target (the real split is VirusFeed's business) — must survive: refill
    // only ever ADDS. next_id pins that the refill spawned nothing either.
    sim::spawn(w, sim::EntityKind::Virus, {1000.0f, 1000.0f}, w.tuning.virus_mass);
    const sim::EntityId before = w.next_id;
    sim::step(w, sim::tick_dt(w.tuning));

    EXPECT_EQ(count_kind(w, sim::EntityKind::Virus), 4);
    EXPECT_EQ(w.next_id, before);
}

TEST(VirusPop, NeedsBothTheMassRatioAndTheDepth)
{
    // The pop gates are cell-vs-cell's gates aimed at a virus. Below the
    // ratio at full overlap (depth trivially met) nothing may happen: the
    // gate against virus_mass 100 sits at eat_ratio * 100 = 125.
    {
        sim::World w = arena();
        const auto cell = sim::spawn(w, sim::EntityKind::Cell, {2000.0f, 2000.0f}, 124.9f, 1);
        const auto virus =
            sim::spawn(w, sim::EntityKind::Virus, {2000.0f, 2000.0f}, w.tuning.virus_mass);
        sim::step(w, sim::tick_dt(w.tuning));

        EXPECT_NE(find_entity(w, virus), nullptr);
        EXPECT_EQ(count_player_cells(w, 1), 1);
        EXPECT_FLOAT_EQ(find_entity(w, cell)->mass, 124.9f);
        EXPECT_TRUE(w.events.eats.empty());
    }
    // Above the ratio but short of the depth: reach is r_e − depth·r_v =
    // 4·√300 − (1/3)·40 ≈ 55.95. A virus centre a hair outside must stand —
    // inside the query circle (r_e ≈ 69.3), so the depth rule is what saves
    // it — and a hair inside must pop. Margins ~0.5 dwarf the float noise.
    {
        sim::World w = arena();
        sim::spawn(w, sim::EntityKind::Cell, {2000.0f, 2000.0f}, 300.0f, 1);
        const auto virus =
            sim::spawn(w, sim::EntityKind::Virus, {2056.5f, 2000.0f}, w.tuning.virus_mass);
        sim::step(w, sim::tick_dt(w.tuning));

        ASSERT_NE(find_entity(w, virus), nullptr);
        EXPECT_FLOAT_EQ(find_entity(w, virus)->position.x, 2056.5f);   // not even shoved
        EXPECT_EQ(count_player_cells(w, 1), 1);
        EXPECT_TRUE(w.events.eats.empty());
    }
    {
        sim::World w = arena();
        const auto cell = sim::spawn(w, sim::EntityKind::Cell, {2000.0f, 2000.0f}, 300.0f, 1);
        const auto virus =
            sim::spawn(w, sim::EntityKind::Virus, {2055.4f, 2000.0f}, w.tuning.virus_mass);
        sim::step(w, sim::tick_dt(w.tuning));

        EXPECT_EQ(find_entity(w, virus), nullptr);
        ASSERT_EQ(w.events.eats.size(), 1u);
        EXPECT_EQ(w.events.eats.front().eater, cell);
        EXPECT_EQ(w.events.eats.front().eaten, virus);
    }
}

TEST(VirusPop, BurstsIntoEqualArmedPiecesAndRecordsTheMeal)
{
    // 300 + 100 = 400 divides over min(virus_pop_pieces, 16 − 1 + 1) = 8
    // pieces of exactly 50 — a power-of-two division, so the shares and
    // their sum compare with ==, not a tolerance.
    sim::World w = arena();
    const auto eater = sim::spawn(w, sim::EntityKind::Cell, {4000.0f, 4000.0f}, 300.0f, 1);
    const auto virus =
        sim::spawn(w, sim::EntityKind::Virus, {4000.0f, 4000.0f}, w.tuning.virus_mass);
    sim::step(w, sim::tick_dt(w.tuning));

    // A pop IS a meal: recorded like any other eat, and the virus is gone.
    EXPECT_EQ(find_entity(w, virus), nullptr);
    EXPECT_EQ(count_kind(w, sim::EntityKind::Virus), 0);
    ASSERT_EQ(w.events.eats.size(), 1u);
    EXPECT_EQ(w.events.eats.front().eater, eater);
    EXPECT_EQ(w.events.eats.front().eaten, virus);
    EXPECT_TRUE(w.events.deaths.empty());

    ASSERT_EQ(count_player_cells(w, 1), 8);
    EXPECT_EQ(total_mass(w), 400.0f);   // nothing minted, nothing lost
    // Every piece carries the mass-scaled cooldown. Burst pieces are born in
    // the eat pass — after this tick's integrate phase — so unlike an action
    // split's halves the timers have not started counting: exactly armed.
    const float armed =
        w.tuning.merge_cooldown_base + w.tuning.merge_cooldown_per_mass * 50.0f;
    for (const sim::Entity& e : w.entities) {
        EXPECT_EQ(e.mass, 50.0f) << "id " << e.id;
        EXPECT_FLOAT_EQ(e.merge_cooldown, armed) << "id " << e.id;
    }
}

TEST(VirusPop, CapLimitsThePiecesAndAtTheCapOnlyTheMassLands)
{
    // 15 cells: room for one more — N = min(8, 16 − 15 + 1) = 2, so 400
    // divides into exactly two pieces of 200 (spared by decay: the floor
    // taxes only mass strictly above the threshold).
    {
        sim::World w = arena();
        const auto popper = sim::spawn(w, sim::EntityKind::Cell, {4000.0f, 4000.0f}, 300.0f, 1);
        for (int i = 0; i < 14; ++i) {
            sim::spawn(w, sim::EntityKind::Cell,
                       {400.0f + 500.0f * static_cast<float>(i), 7000.0f}, 20.0f, 1);
        }
        const auto virus =
            sim::spawn(w, sim::EntityKind::Virus, {4000.0f, 4000.0f}, w.tuning.virus_mass);
        sim::step(w, sim::tick_dt(w.tuning));

        EXPECT_EQ(count_player_cells(w, 1), 16);
        ASSERT_EQ(w.events.eats.size(), 1u);
        const sim::Entity* elder = find_entity(w, popper);
        const sim::Entity* piece = find_entity(w, virus + 1);   // ids are monotonic: minted next
        ASSERT_NE(elder, nullptr);
        ASSERT_NE(piece, nullptr);
        EXPECT_EQ(elder->mass, 200.0f);
        EXPECT_EQ(piece->mass, 200.0f);
    }
    // At the cap the anti-snowball still bites the wallet, not the roster:
    // the meal happens — mass lands, event recorded — but nothing splits,
    // and no merge commitment is armed for a burst that never was.
    {
        sim::World w = arena();
        w.tuning.decay_rate = 0.0f;   // 400 sits above decay_threshold; keep the landed mass exact
        const auto popper = sim::spawn(w, sim::EntityKind::Cell, {4000.0f, 4000.0f}, 300.0f, 1);
        for (int i = 0; i < 15; ++i) {
            sim::spawn(w, sim::EntityKind::Cell,
                       {400.0f + 500.0f * static_cast<float>(i), 7000.0f}, 20.0f, 1);
        }
        const auto virus =
            sim::spawn(w, sim::EntityKind::Virus, {4000.0f, 4000.0f}, w.tuning.virus_mass);
        sim::step(w, sim::tick_dt(w.tuning));

        EXPECT_EQ(count_player_cells(w, 1), 16);
        EXPECT_EQ(find_entity(w, virus), nullptr);
        ASSERT_EQ(w.events.eats.size(), 1u);
        EXPECT_EQ(w.events.eats.front().eater, popper);
        const sim::Entity* fed = find_entity(w, popper);
        ASSERT_NE(fed, nullptr);
        EXPECT_EQ(fed->mass, 400.0f);
        EXPECT_EQ(fed->merge_cooldown, 0.0f);
    }
}

TEST(VirusPop, LaunchesPiecesAtFixedRadialAnglesIdenticallyEveryTime)
{
    const auto build_and_pop = [](sim::World& w) {
        sim::spawn(w, sim::EntityKind::Cell, {4000.0f, 4000.0f}, 300.0f, 1);
        sim::spawn(w, sim::EntityKind::Virus, {4000.0f, 4000.0f}, w.tuning.virus_mass);
        sim::step(w, sim::tick_dt(w.tuning));
    };
    sim::World w = arena();
    build_and_pop(w);

    // The N − 1 launched pieces sit at 2πk/N from +x — fixed angles, no rng —
    // and, born after this tick's integrate phase, their kicks are still
    // undamped: exactly split_impulse_speed along each spoke. Ids are
    // monotonic (eater 1, virus 2), so piece k took id 2 + k.
    ASSERT_EQ(count_player_cells(w, 1), 8);
    const float speed = w.tuning.split_impulse_speed;
    for (int k = 1; k <= 7; ++k) {
        const sim::Entity* piece = find_entity(w, static_cast<sim::EntityId>(2 + k));
        ASSERT_NE(piece, nullptr) << "piece " << k;
        const float angle =
            2.0f * std::numbers::pi_v<float> * static_cast<float>(k) / 8.0f;
        EXPECT_NEAR(piece->impulse.x, speed * std::cos(angle), 1e-2f) << "piece " << k;
        EXPECT_NEAR(piece->impulse.y, speed * std::sin(angle), 1e-2f) << "piece " << k;
    }
    // Compass spot checks: k = 2 launches +y, k = 4 launches −x, k = 6 −y.
    EXPECT_NEAR(find_entity(w, 4)->impulse.y, speed, 1e-3f);
    EXPECT_NEAR(find_entity(w, 6)->impulse.x, -speed, 1e-3f);
    EXPECT_NEAR(find_entity(w, 8)->impulse.y, -speed, 1e-3f);
    // The original keeps piece 0's share with no kick of its own.
    EXPECT_EQ(find_entity(w, 1)->impulse.x, 0.0f);
    EXPECT_EQ(find_entity(w, 1)->impulse.y, 0.0f);

    // Two same-seed worlds pop identically, field for field.
    sim::World twin = arena();
    build_and_pop(twin);
    ASSERT_EQ(w.entities.size(), twin.entities.size());
    for (std::size_t i = 0; i < w.entities.size(); ++i) {
        EXPECT_EQ(w.entities[i].id, twin.entities[i].id) << "entity " << i;
        EXPECT_EQ(w.entities[i].position.x, twin.entities[i].position.x) << "entity " << i;
        EXPECT_EQ(w.entities[i].position.y, twin.entities[i].position.y) << "entity " << i;
        EXPECT_EQ(w.entities[i].impulse.x, twin.entities[i].impulse.x) << "entity " << i;
        EXPECT_EQ(w.entities[i].impulse.y, twin.entities[i].impulse.y) << "entity " << i;
        EXPECT_EQ(w.entities[i].mass, twin.entities[i].mass) << "entity " << i;
    }
}

TEST(VirusFeed, SevenHitsSplitTheVirusAlongTheLastFeedDirection)
{
    sim::World w = arena();
    const auto virus =
        sim::spawn(w, sim::EntityKind::Virus, {4000.0f, 4000.0f}, w.tuning.virus_mass);

    // Six ejecta drifting +x inside the virus's radius (40 at virus_mass):
    // all consumed in one step, count visible, no split yet — and feeding is
    // terraforming, not a meal, so no eat events.
    for (int i = 0; i < 6; ++i) {
        const auto id = sim::spawn(w, sim::EntityKind::EjectedMass, {3970.0f, 4000.0f},
                                   w.tuning.ejected_mass, 1);
        find_entity(w, id)->velocity = {40.0f, 0.0f};
    }
    sim::step(w, sim::tick_dt(w.tuning));

    EXPECT_EQ(count_kind(w, sim::EntityKind::EjectedMass), 0);
    EXPECT_EQ(count_kind(w, sim::EntityKind::Virus), 1);
    EXPECT_TRUE(w.events.eats.empty());
    const sim::Entity* fed = find_entity(w, virus);
    ASSERT_NE(fed, nullptr);
    EXPECT_EQ(fed->feed_count, 6);
    EXPECT_FLOAT_EQ(fed->mass, w.tuning.virus_mass);   // absorbed feed mass evaporates
    EXPECT_FLOAT_EQ(fed->last_feed_dir.x, 1.0f);
    EXPECT_FLOAT_EQ(fed->last_feed_dir.y, 0.0f);

    // The seventh hit comes from below: the split fires along THIS feed.
    const auto last = sim::spawn(w, sim::EntityKind::EjectedMass, {4000.0f, 3975.0f},
                                 w.tuning.ejected_mass, 1);
    find_entity(w, last)->velocity = {0.0f, 60.0f};
    sim::step(w, sim::tick_dt(w.tuning));

    EXPECT_EQ(count_kind(w, sim::EntityKind::EjectedMass), 0);
    EXPECT_TRUE(w.events.eats.empty());
    ASSERT_EQ(count_kind(w, sim::EntityKind::Virus), 2);
    const sim::Entity* parent = find_entity(w, virus);
    const sim::Entity* child = find_entity(w, last + 1);   // minted right after the pellet
    ASSERT_NE(parent, nullptr);
    ASSERT_NE(child, nullptr);
    EXPECT_EQ(parent->feed_count, 0);   // both reset
    EXPECT_EQ(child->feed_count, 0);
    EXPECT_FLOAT_EQ(parent->mass, w.tuning.virus_mass);   // both exactly virus_mass:
    EXPECT_FLOAT_EQ(child->mass, w.tuning.virus_mass);    // feeding prints no mass
    // Born at the parent's position after this tick's integrate phase: not
    // yet moved, carrying the whole eject-speed kick straight up.
    EXPECT_FLOAT_EQ(child->position.x, 4000.0f);
    EXPECT_FLOAT_EQ(child->position.y, 4000.0f);
    EXPECT_FLOAT_EQ(child->impulse.x, 0.0f);
    EXPECT_FLOAT_EQ(child->impulse.y, w.tuning.eject_speed);

    // And it flies: +y, tick after tick (≈ eject_speed / impulse_damping_rate
    // ≈ 400 units all told — here just "the launch is real motion").
    float last_y = child->position.y;
    for (int i = 0; i < 3; ++i) {
        sim::step(w, sim::tick_dt(w.tuning));
        const float y = find_entity(w, last + 1)->position.y;
        EXPECT_GT(y, last_y);
        last_y = y;
    }
}

TEST(VirusFeed, DeadStillPelletFeedsAlongThePositionDelta)
{
    // A fully decayed pellet (velocity exactly {0,0}, built by hand — the
    // state a long-resting eject reaches) has no flight to aim along; the
    // deterministic fallback is the pellet→virus axis.
    sim::World w = arena();
    w.tuning.virus_feed_count = 1;   // one hit splits: the direction is the whole test
    sim::spawn(w, sim::EntityKind::Virus, {4000.0f, 4000.0f}, w.tuning.virus_mass);
    const auto pellet = sim::spawn(w, sim::EntityKind::EjectedMass, {3980.0f, 4000.0f},
                                   w.tuning.ejected_mass, 1);
    sim::step(w, sim::tick_dt(w.tuning));

    EXPECT_EQ(find_entity(w, pellet), nullptr);   // consumed all the same
    ASSERT_EQ(count_kind(w, sim::EntityKind::Virus), 2);
    const sim::Entity* child = find_entity(w, pellet + 1);
    ASSERT_NE(child, nullptr);
    EXPECT_FLOAT_EQ(child->impulse.x, w.tuning.eject_speed);   // virus − pellet points +x
    EXPECT_FLOAT_EQ(child->impulse.y, 0.0f);
}

TEST(VirusPop, BelowGateCellSitsOnAVirusForeverUntouched)
{
    // Below the gates the virus is terrain: fifty ticks of dead-centre
    // overlap move nothing, eat nothing, pop nothing — small cells hide on
    // viruses, they are never punished by them.
    sim::World w = arena();
    const auto cell = sim::spawn(w, sim::EntityKind::Cell, {4000.0f, 4000.0f}, 50.0f, 1);
    const auto virus =
        sim::spawn(w, sim::EntityKind::Virus, {4000.0f, 4000.0f}, w.tuning.virus_mass);
    std::size_t events = 0;
    for (int i = 0; i < 50; ++i) {
        sim::step(w, sim::tick_dt(w.tuning));
        events += w.events.eats.size() + w.events.deaths.size();
    }

    EXPECT_EQ(events, 0u);
    ASSERT_EQ(w.entities.size(), 2u);
    EXPECT_FLOAT_EQ(find_entity(w, cell)->mass, 50.0f);
    EXPECT_FLOAT_EQ(find_entity(w, virus)->mass, w.tuning.virus_mass);
    EXPECT_FLOAT_EQ(find_entity(w, cell)->position.x, 4000.0f);
    EXPECT_FLOAT_EQ(find_entity(w, cell)->position.y, 4000.0f);
    EXPECT_FLOAT_EQ(find_entity(w, virus)->position.x, 4000.0f);
    EXPECT_FLOAT_EQ(find_entity(w, virus)->position.y, 4000.0f);
}

TEST(SafeSpawn, RetriesPastAThreatenedDrawDeterministically)
{
    // Probe twin: same seed, same call sequence, no threat — its player
    // lands on the FIRST draw, which is exactly where the real world parks
    // the threat. That draw is then threatened by construction.
    sim::World probe = arena(31u);
    sim::step(probe, sim::tick_dt(probe.tuning));
    const auto probe_id = sim::spawn_player(probe, 1);
    const math::Vec2 first_draw = find_entity(probe, probe_id)->position;

    sim::World w = arena(31u);
    sim::spawn(w, sim::EntityKind::Cell, first_draw, 500.0f, 9);
    sim::step(w, sim::tick_dt(w.tuning));   // the standing grid now holds the threat
    const std::mt19937 rng_before = w.rng;
    const auto id = sim::spawn_player(w, 1);
    const math::Vec2 at = find_entity(w, id)->position;

    // Landed clear of the threat (which never moved: no intent)…
    EXPECT_GE(math::length(at - first_draw), w.tuning.safe_spawn_radius);
    // …and the generator advanced past one draw's worth (a placement draw is
    // two raw pulls, x then y): retries really ran, they were not skipped in
    // favour of some grid-free shortcut.
    std::mt19937 one_draw = rng_before;
    one_draw.discard(2);
    EXPECT_FALSE(w.rng == one_draw);

    // Same seed, same script, same landing: the retry count is a pure
    // function of world state, so lifecycle calls stay replayable.
    sim::World twin = arena(31u);
    sim::spawn(twin, sim::EntityKind::Cell, first_draw, 500.0f, 9);
    sim::step(twin, sim::tick_dt(twin.tuning));
    const auto twin_id = sim::spawn_player(twin, 1);
    EXPECT_EQ(find_entity(twin, twin_id)->position.x, at.x);
    EXPECT_EQ(find_entity(twin, twin_id)->position.y, at.y);
    EXPECT_TRUE(w.rng == twin.rng);
}

TEST(SafeSpawn, WhenNoDrawIsSafeTheLastOneStands)
{
    // An 800-unit lattice of mass-500 cells puts every point of the square
    // within ~566 (< safe_spawn_radius) of a threat: no draw can succeed.
    sim::World w = arena(6u);
    for (int gy = 0; gy <= 10; ++gy) {
        for (int gx = 0; gx <= 10; ++gx) {
            sim::spawn(w, sim::EntityKind::Cell,
                       {800.0f * static_cast<float>(gx), 800.0f * static_cast<float>(gy)},
                       500.0f, 9);
        }
    }
    sim::step(w, sim::tick_dt(w.tuning));

    std::mt19937 exhausted = w.rng;
    exhausted.discard(2ull * static_cast<unsigned long long>(w.tuning.safe_spawn_attempts));
    const auto id = sim::spawn_player(w, 1);

    // Every attempt was drawn and rejected — the generator sits exactly
    // safe_spawn_attempts placements ahead — and the player exists anyway,
    // on the last draw: bounded work, and spawning into danger beats not
    // spawning at all.
    EXPECT_TRUE(w.rng == exhausted);
    const sim::Entity* cell = find_entity(w, id);
    ASSERT_NE(cell, nullptr);
    EXPECT_EQ(cell->owner, 1);
    EXPECT_GE(cell->position.x, 0.0f);
    EXPECT_LT(cell->position.x, w.tuning.world_extent);
    EXPECT_GE(cell->position.y, 0.0f);
    EXPECT_LT(cell->position.y, w.tuning.world_extent);
}

TEST(Replay, VirusPopsFeedsAndChurnReplayIdentically)
{
    // The M3/M4 replays' sibling (both stand untouched next door), extended
    // with M5's vocabulary over LIVE pellet AND virus fields: a scripted pop
    // (a fattened cell walks east into a planted virus and bursts), a
    // scripted feed-split (a station-keeping feeder pumps ejecta into a
    // planted virus until it fires back), the planted kill, and lifecycle
    // churn — now through the safe-spawn retry loop, whose draw count is
    // itself part of the replayed sequence. Exact equality across two
    // same-seed runs is same-binary determinism (invariant 3), and the
    // comparison covers the two new feed fields.
    struct Tally {
        std::size_t eats{};
        std::size_t deaths{};
        bool        popped{};           // the planted pop virus became somebody's meal
        bool        virus_launched{};   // a kicked virus exists: only feed-splits launch viruses
        int         peak_owner5_cells{};
    };
    const auto run_script = [](sim::World& w) {
        Tally tally;
        // Planted props, spawned before the first sow so their ids are pinned.
        const auto pop_virus =
            sim::spawn(w, sim::EntityKind::Virus, {6000.0f, 4000.0f}, w.tuning.virus_mass);
        sim::spawn(w, sim::EntityKind::Virus, {2000.0f, 4000.0f}, w.tuning.virus_mass);
        sim::spawn(w, sim::EntityKind::Cell, {5900.0f, 4000.0f}, 300.0f, 5);
        sim::spawn(w, sim::EntityKind::Cell, {1507.0f, 4000.0f}, 400.0f, 6);
        // The planted meal between rivals, as in the M3/M4 scripts.
        sim::spawn(w, sim::EntityKind::Cell, {4096.0f, 4096.0f}, 150.0f, 3);
        sim::spawn(w, sim::EntityKind::Cell, {4096.0f, 4096.0f}, 20.0f, 4);
        sim::spawn_player(w, 1);
        sim::spawn_player(w, 2);
        const auto dir = [](int tick, int player) -> math::Vec2 {
            switch ((tick / 25 + player) % 4) {
            case 0:  return {1.0f, 0.0f};
            case 1:  return {0.0f, 1.0f};
            case 2:  return {-1.0f, 0.0f};
            default: return {0.0f, -1.0f};
            }
        };
        for (int t = 0; t < 300; ++t) {
            sim::apply_intent(w, sim::PlayerIntent{.player = 1, .direction = dir(t, 1)});
            sim::apply_intent(w, sim::PlayerIntent{.player = 2, .direction = dir(t, 2)});
            // The popper walks steadily east onto the planted virus (~6 ticks).
            sim::apply_intent(w, sim::PlayerIntent{.player = 5, .direction = {1.0f, 0.0f}});
            // The feeder oscillates in place: eject aims along intent, so it
            // faces +x on eject ticks, and the −x legs walk the drift back so
            // every pellet's ~493-unit flight ends on the planted virus.
            // Seven ejecta are up by t = 12; the split fires when the seventh
            // sails into the virus's radius (~13 ticks of flight later).
            if (t <= 12) {
                sim::apply_intent(
                    w, sim::PlayerIntent{.player = 6,
                                         .direction = t % 2 == 0 ? math::Vec2{1.0f, 0.0f}
                                                                 : math::Vec2{-1.0f, 0.0f},
                                         .eject = t % 2 == 0});
            } else {
                sim::apply_intent(w, sim::PlayerIntent{.player = 6});
            }
            if (t == 100) {
                sim::despawn_player(w, 2);   // lifecycle is part of the script
            }
            if (t == 120) {
                sim::spawn_player(w, 2);     // and so is the reconnect
            }
            sim::step(w, sim::tick_dt(w.tuning));

            tally.eats += w.events.eats.size();
            tally.deaths += w.events.deaths.size();
            for (const sim::EatEvent& eat : w.events.eats) {
                if (eat.eaten == pop_virus) {
                    tally.popped = true;
                }
            }
            int owner5 = 0;
            for (const sim::Entity& e : w.entities) {
                if (e.kind == sim::EntityKind::Cell && e.owner == 5) {
                    ++owner5;
                }
                if (e.kind == sim::EntityKind::Virus &&
                    (e.impulse.x != 0.0f || e.impulse.y != 0.0f)) {
                    tally.virus_launched = true;
                }
            }
            tally.peak_owner5_cells = std::max(tally.peak_owner5_cells, owner5);
        }
        return tally;
    };

    sim::World a = sim::make_world(454545u);
    sim::World b = sim::make_world(454545u);
    const Tally tally_a = run_script(a);
    const Tally tally_b = run_script(b);

    // The script really did the M5 things: the planted virus was popped, the
    // burst filled the pop arithmetic (8 pieces at some point; merges may
    // thin them later), and a fed virus launched.
    EXPECT_GT(tally_a.eats, 0u);
    EXPECT_GT(tally_a.deaths, 0u);
    EXPECT_TRUE(tally_a.popped);
    EXPECT_TRUE(tally_a.virus_launched);
    EXPECT_GE(tally_a.peak_owner5_cells, 8);

    EXPECT_EQ(tally_a.eats, tally_b.eats);
    EXPECT_EQ(tally_a.deaths, tally_b.deaths);
    EXPECT_EQ(tally_a.popped, tally_b.popped);
    EXPECT_EQ(tally_a.virus_launched, tally_b.virus_launched);
    EXPECT_EQ(tally_a.peak_owner5_cells, tally_b.peak_owner5_cells);

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
        EXPECT_EQ(ea.feed_count, eb.feed_count) << "entity " << i;
        EXPECT_EQ(ea.last_feed_dir.x, eb.last_feed_dir.x) << "entity " << i;
        EXPECT_EQ(ea.last_feed_dir.y, eb.last_feed_dir.y) << "entity " << i;
    }
}
