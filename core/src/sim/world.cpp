#include <blob/sim/world.hpp>

#include <blob/sim/spatial_grid.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <numbers>
#include <type_traits>

namespace blob::sim {

// The plain-struct rule, asserted rather than assumed. World holds vectors so
// it is not trivially copyable, but it must stay aggregate-initializable.
static_assert(std::is_aggregate_v<World>);
static_assert(std::is_aggregate_v<Entity>);
static_assert(std::is_aggregate_v<PlayerIntent>);
static_assert(std::is_aggregate_v<StepEvents>);
static_assert(std::is_aggregate_v<EatEvent>);
static_assert(std::is_trivially_copyable_v<EatEvent>);

namespace {

/// Uniform float in [0, 1) straight off the raw generator — deliberately not
/// a std distribution: distribution output is implementation-defined, so the
/// same seed would replay differently across standard libraries. Determinism
/// is only claimed same-binary today, but there is no reason to close the
/// cross-platform door for free. Top 24 bits of the draw -> float keeps every
/// value exactly representable (float carries a 24-bit significand).
[[nodiscard]] float rand_unit(std::mt19937& g) noexcept
{
    return static_cast<float>(g() >> 8) * (1.0f / 16777216.0f);
}

/// Owners holding at least one alive Cell, sorted-unique into `out` — which
/// is a reused scratch vector, so steady state allocates nothing.
void collect_cell_owners(const World& world, std::vector<PlayerId>& out)
{
    out.clear();
    for (const Entity& e : world.entities) {
        if (e.kind == EntityKind::Cell && !e.dead) {
            out.push_back(e.owner);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

/// The merge cooldown a piece of `piece_mass` carries out of any split.
/// M4's player split and M5's pop burst share this one formula on purpose:
/// the "mass-scaled commitment" rule must not be able to drift between them.
[[nodiscard]] float merge_cooldown_for(const Tuning& tuning, float piece_mass) noexcept
{
    return tuning.merge_cooldown_base + tuning.merge_cooldown_per_mass * piece_mass;
}

/// Spawns one launched split piece: a Cell at `at` carrying `mass`, kicked
/// along `impulse` and armed with the mass-scaled cooldown — the shared tail
/// of M4's perform_split and M5's pop burst, so both arm pieces identically.
/// Invalidates references into world.entities (spawn may reallocate).
void spawn_split_piece(World& world, PlayerId owner, math::Vec2 at, float mass,
                       math::Vec2 impulse)
{
    spawn(world, EntityKind::Cell, at, mass, owner);
    Entity& piece = world.entities.back();
    // The kick rides in `impulse`, not `velocity` — see the Entity field
    // comment: intent overwrites velocity every tick.
    piece.impulse        = impulse;
    piece.merge_cooldown = merge_cooldown_for(world.tuning, mass);
}

/// One split action for one player. Iterates only the cells that existed
/// before any splitting this tick (the pre-scan size bound): splitting
/// appends, and a fresh half must never re-split off the same keypress.
/// Array order — spawn order — is the deterministic priority when the cap
/// truncates a burst (invariant 3, same doctrine as eat resolution).
void perform_split(World& world, const PlayerIntent& intent)
{
    int live = 0;
    for (const Entity& e : world.entities) {
        if (e.kind == EntityKind::Cell && e.owner == intent.player && !e.dead) {
            ++live;
        }
    }

    const std::size_t count = world.entities.size();
    for (std::size_t i = 0; i < count && live < world.tuning.max_cells_per_player; ++i) {
        Entity& parent = world.entities[i];   // re-bound each round; spawn() below reallocates
        if (parent.kind != EntityKind::Cell || parent.owner != intent.player ||
            parent.dead || parent.mass < world.tuning.min_split_mass) {
            continue;
        }

        // Halving a float is exact (an exponent decrement), so half + half
        // reassembles the parent's mass to the bit — conservation by
        // construction, not by tolerance.
        const float half = parent.mass * 0.5f;
        // Mass-scaled commitment on BOTH halves: big splits stay split longer.
        parent.mass           = half;
        parent.merge_cooldown = merge_cooldown_for(world.tuning, half);
        const math::Vec2 at   = parent.position;

        // Zero intent means zero kick: the pair still splits, and same-owner
        // push-apart provides the separation. (`parent` dangles from here.)
        spawn_split_piece(world, intent.player, at, half,
                          intent.direction * world.tuning.split_impulse_speed);
        ++live;
    }
}

/// One eject action for one player: every sufficiently heavy cell fires one
/// pellet along the intent direction. The cost−carried difference evaporates
/// by design — ejecting must never print mass.
void perform_eject(World& world, const PlayerIntent& intent)
{
    if (intent.direction == math::Vec2{}) {
        return;   // "hold still" has no aim; there is nowhere to eject toward
    }

    const std::size_t count = world.entities.size();   // fresh pellets append past this bound
    for (std::size_t i = 0; i < count; ++i) {
        Entity& cell = world.entities[i];   // re-bound each round; spawn() below reallocates
        if (cell.kind != EntityKind::Cell || cell.owner != intent.player ||
            cell.dead || cell.mass < world.tuning.min_eject_mass) {
            continue;
        }

        cell.mass -= world.tuning.eject_mass_cost;
        // Rim-to-rim placement, derived rather than a new margin knob: the
        // pellet materializes tangent to the (post-cost) parent, fully
        // outside it, so this tick's eat pass (dist <= r_eater, inclusive)
        // cannot instantly re-eat it — and it only ever moves further away,
        // since eject_speed outruns any cell.
        const float r_parent = radius_for_mass(world.tuning, cell.mass);
        const float r_pellet = radius_for_mass(world.tuning, world.tuning.ejected_mass);
        const math::Vec2 at  = cell.position + intent.direction * (r_parent + r_pellet);

        // Owner kept: you may eat your own ejected mass (the eat pass already
        // allows it), and M5's viruses will attribute feeds by owner.
        spawn(world, EntityKind::EjectedMass, at, world.tuning.ejected_mass, intent.player);
        // EjectedMass has no intent, so its flight lives in `velocity` and
        // the integrate phase damps it by e^(−λ·dt) — see the Entity comment.
        world.entities.back().velocity = intent.direction * world.tuning.eject_speed;
    }
}

/// Phase 0 of step(): one-shot actions. The flags are edges, not levels —
/// the server latches a keypress into the stored intent, and this consumes
/// it: perform, then clear, so one latched press yields exactly one action
/// however many ticks follow. The direction half of the intent persists;
/// steering is a level. Split runs before eject (a fixed order, for
/// invariant 3), so on a both-flags tick the eject scan already sees the
/// fresh halves.
void resolve_actions(World& world)
{
    for (PlayerIntent& intent : world.intents) {
        if (intent.split) {
            perform_split(world, intent);
        }
        if (intent.eject) {
            perform_eject(world, intent);
        }
        intent.split = false;
        intent.eject = false;
    }
}

/// M5's pop burst: the cell at `eater_index` — which has just absorbed a
/// virus — force-splits into N equal pieces, N = min(virus_pop_pieces,
/// max_cells_per_player − owned + 1) with `owned` counted at burst time, so
/// back-to-back pops in one tick shrink each other's bursts and the cap is
/// never breached. At the cap N = 1: the mass lands and nothing splits (no
/// cooldown is armed for a burst that never was) — the anti-snowball still
/// bites everyone below cap. Reuses M4's split machinery (merge_cooldown_for
/// + spawn_split_piece), and launches the N − 1 new pieces radially at
/// 2πk/N from the +x axis — fixed, rng-free angles: a pop is already pure
/// punishment, so its scatter should be readable and replayable, never a
/// lottery (and invariant 3 stays untouched: no draw, no divergence).
/// Invalidates references into world.entities (spawn may reallocate).
void burst_cell(World& world, std::size_t eater_index)
{
    int owned = 0;
    for (const Entity& e : world.entities) {
        if (e.kind == EntityKind::Cell && e.owner == world.entities[eater_index].owner &&
            !e.dead) {
            ++owned;
        }
    }
    const int n = std::min(world.tuning.virus_pop_pieces,
                           world.tuning.max_cells_per_player - owned + 1);
    if (n <= 1) {
        return;   // at (or defensively past) the cap: the mass lands, nothing splits
    }

    // The post-gain mass divides equally; the original keeps piece 0's share
    // in place. Every piece — original included — carries the mass-scaled
    // merge cooldown, exactly like an action split's halves.
    Entity&          cell  = world.entities[eater_index];
    const float      share = cell.mass / static_cast<float>(n);
    const math::Vec2 at    = cell.position;
    const PlayerId   owner = cell.owner;
    cell.mass           = share;
    cell.merge_cooldown = merge_cooldown_for(world.tuning, share);
    for (int k = 1; k < n; ++k) {   // `cell` dangles once the first piece spawns
        const float frac  = static_cast<float>(k) / static_cast<float>(n);
        const float angle = 2.0f * std::numbers::pi_v<float> * frac;
        const math::Vec2 dir{std::cos(angle), std::sin(angle)};
        spawn_split_piece(world, owner, at, share, dir * world.tuning.split_impulse_speed);
    }
}

/// Eat resolution, against the grid built over this tick's positions. Marks
/// victims dead and transfers mass; removal waits for compaction so entity
/// indices stay aligned with the grid throughout the scan. Eaters run in
/// array order — that *is* the tie-break rule, and since spawn order fixes
/// array order, resolution is deterministic (invariant 3).
///
/// A pop burst appends entities mid-pass; the loop bound is snapshotted
/// (exactly as the action phase snapshots before splitting), so fresh burst
/// pieces are never iterated as eaters this tick — and they missed grid#1,
/// so no query can hand them out as victims either: new pieces sit the rest
/// of the tick out in both directions.
void resolve_eating(World& world)
{
    // Deaths are detected by difference: who owned an alive Cell before,
    // minus who still does after. Eating is the only killer — decay floors
    // above zero, a merge keeps the elder under the same owner, a pop splits
    // rather than kills, and despawn is explicitly not death.
    collect_cell_owners(world, world.owners_before_scratch);

    const std::size_t count = world.entities.size();
    for (std::size_t i = 0; i < count; ++i) {
        if (world.entities[i].kind != EntityKind::Cell || world.entities[i].dead) {
            continue;   // only live Cells eat; food and viruses never do
        }

        // Reach is the eater's radius at scan start: mass gained mid-scan
        // does not extend this arm until the next tick. Either convention
        // would be deterministic — this one just keeps one meal from
        // cascading into a longer reach within a single tick. The ratio gate
        // below *does* use live mass, so growth still counts there.
        const float r_e = radius_for_mass(world.tuning, world.entities[i].mass);
        const math::Vec2 eater_at = world.entities[i].position;

        // Both eat rules imply centre distance <= r_e, so one circle query
        // covers them. It must be for_each_in_circle, not candidate pairs:
        // a big eater's reach exceeds grid_cell_size, and this query is the
        // documented big-eater path (invariant 6 — never a raw pair scan).
        for_each_in_circle(
            world.grid, eater_at, r_e,
            [&world, i, r_e](std::uint32_t index, math::Vec2) {
                if (static_cast<std::size_t>(index) == i) {
                    return;   // self
                }
                // Re-acquired on every entry, never held across one: a pop
                // burst below appends entities and may reallocate the array,
                // so a reference captured once would dangle mid-query.
                Entity& eater = world.entities[i];
                Entity& victim = world.entities[index];
                if (victim.dead) {
                    return;   // already eaten earlier this tick
                }
                bool eaten = false;
                bool pop = false;   // an eaten Virus additionally bursts the eater
                switch (victim.kind) {
                case EntityKind::Pellet:
                case EntityKind::EjectedMass:
                    // The query's inclusive dist <= r_e filter *is* the rule
                    // for food — touching the rim counts, matching the
                    // grid's boundary convention.
                    eaten = true;
                    break;
                case EntityKind::Cell:
                    // Same-owner cells are completely inert in M3 (merge and
                    // push-apart are M4's). Different owners need the mass
                    // gate and real depth: the victim's centre must sit
                    // inside the eater by eat_depth_factor of the victim's
                    // radius, so a rim-grazing giant does not hoover up
                    // everything it touches. Mutual eating is impossible:
                    // the ratio gate cannot hold both ways.
                    if (victim.owner != eater.owner &&
                        eater.mass >= world.tuning.eat_ratio * victim.mass) {
                        const float dist = math::length(victim.position - eater.position);
                        const float r_v = radius_for_mass(world.tuning, victim.mass);
                        eaten = dist <= r_e - world.tuning.eat_depth_factor * r_v;
                    }
                    break;
                case EntityKind::Virus:
                    // M5's pop rule: cell-vs-cell's two gates aimed at a
                    // virus (viruses are unowned, so there is no owner
                    // clause) — heavy enough AND deep enough. Below either
                    // gate the virus is terrain: a small cell can sit on it
                    // indefinitely and nothing whatsoever happens.
                    if (eater.mass >= world.tuning.eat_ratio * victim.mass) {
                        const float dist = math::length(victim.position - eater.position);
                        const float r_v = radius_for_mass(world.tuning, victim.mass);
                        eaten = pop = dist <= r_e - world.tuning.eat_depth_factor * r_v;
                    }
                    break;
                }
                if (eaten) {
                    victim.dead = true;
                    eater.mass += victim.mass;   // mass moves, never vanishes —
                                                 // a pop included: the virus's mass
                                                 // lands before the burst divides it
                    world.events.eats.push_back(
                        EatEvent{.eater = eater.id, .eaten = victim.id});
                    if (pop) {
                        // A pop IS a meal — recorded above like any other,
                        // so the layers above need no special case — and
                        // then the burst. Last statement on purpose:
                        // `eater`/`victim` dangle past this call.
                        burst_cell(world, i);
                    }
                }
            });
    }

    // before \ after = players whose last Cell died this tick (both vectors
    // are sorted-unique, so one death per player however many cells fell).
    collect_cell_owners(world, world.owners_after_scratch);
    std::set_difference(world.owners_before_scratch.begin(), world.owners_before_scratch.end(),
                        world.owners_after_scratch.begin(), world.owners_after_scratch.end(),
                        std::back_inserter(world.events.deaths));
}

/// M5's feeding phase, right after eat resolution: every alive Virus absorbs
/// the alive EjectedMass whose centres lie inside its radius (the circle
/// query's inclusive boundary *is* the rule, same convention as food), and a
/// full feed count fires a split toward the last feeder. Running after the
/// eat pass means a cell racing a virus for the same pellet resolves to the
/// cell — array-order eaters first — and a virus popped this tick no longer
/// feeds. Consumed pellets are dead-marked with NO EatEvent: feeding is
/// terraforming, not a meal — nothing above core should react to it — and
/// the pellet's mass vanishes with it. The virus's own mass stays virus_mass
/// throughout: absorbed feed mass evaporates, the same anti-mass-printing
/// rule as eject.
void feed_viruses(World& world)
{
    // Snapshotted bound, exactly like the action phase and the eat pass:
    // feed-splits append, and a fresh virus must not feed (or be queried)
    // on the tick of its birth — it missed grid#1 anyway.
    const std::size_t count = world.entities.size();
    for (std::size_t i = 0; i < count; ++i) {
        if (world.entities[i].kind != EntityKind::Virus || world.entities[i].dead) {
            continue;
        }
        const float      r_v = radius_for_mass(world.tuning, world.entities[i].mass);
        const math::Vec2 at  = world.entities[i].position;
        for_each_in_circle(
            world.grid, at, r_v,
            [&world, i](std::uint32_t index, math::Vec2) {
                // Re-acquired on every entry, never held across one: a
                // feed-split below appends entities and may reallocate the
                // array, so a reference captured once would dangle.
                Entity& virus = world.entities[i];
                Entity& pellet = world.entities[index];
                if (pellet.kind != EntityKind::EjectedMass || pellet.dead) {
                    return;   // viruses eat only ejecta; eaten pellets are gone
                }
                pellet.dead = true;
                ++virus.feed_count;
                // Feed direction, a deterministic fallback chain (every
                // branch is a pure function of state, invariant 3): the
                // pellet's remaining flight while it still moves; the
                // pellet→virus axis once it has come to rest; +x if the
                // pellet sits exactly on the centre and neither exists.
                math::Vec2 dir = math::normalized(pellet.velocity);
                if (dir == math::Vec2{}) {
                    dir = math::normalized(virus.position - pellet.position);
                }
                if (dir == math::Vec2{}) {
                    dir = {1.0f, 0.0f};
                }
                virus.last_feed_dir = dir;
                if (virus.feed_count >= world.tuning.virus_feed_count) {
                    // The fed split: a NEW virus at the fed one's position,
                    // launched along the last feed at eject speed — it
                    // glides ~eject_speed/impulse_damping_rate ≈ 400 units,
                    // the genre's fed-virus lunge. Both parties restart
                    // their count (the newborn starts at zero by spawn).
                    virus.feed_count = 0;
                    const math::Vec2 kick = virus.last_feed_dir * world.tuning.eject_speed;
                    spawn(world, EntityKind::Virus, virus.position,
                          world.tuning.virus_mass);   // `virus`/`pellet` dangle from here
                    world.entities.back().impulse = kick;
                }
            });
    }
}

/// One same-owner pair. Merge is checked first: it is the stricter test, and
/// a pair deep enough to merge is certainly overlapping — checking push-apart
/// first would shove them apart on the very tick they earned the merge.
void resolve_sibling_pair(World& world, Entity& a, Entity& b)
{
    const math::Vec2 delta = b.position - a.position;
    const float      dist  = math::length(delta);
    const float      r_a   = radius_for_mass(world.tuning, a.mass);
    const float      r_b   = radius_for_mass(world.tuning, b.mass);

    // Cooldowns floor at exactly 0.0f (an assignment in the integrate
    // phase), so == 0 is the precise "expired" test, not a float hazard.
    if (a.merge_cooldown == 0.0f && b.merge_cooldown == 0.0f &&
        dist <= world.tuning.merge_overlap * std::max(r_a, r_b)) {
        // The elder (lower id) survives in place and absorbs the younger.
        // Deliberately NOT an EatEvent: a merge is the player's own mass
        // reassembling, not a meal — nothing above core should react to it,
        // and it can never be a death (the owner keeps the elder).
        Entity& elder   = a.id <= b.id ? a : b;
        Entity& younger = a.id <= b.id ? b : a;
        elder.mass += younger.mass;   // sums exactly; mass moves, never vanishes
        younger.dead = true;
        return;
    }

    if ((a.merge_cooldown > 0.0f || b.merge_cooldown > 0.0f) && dist < r_a + r_b) {
        // Push-apart runs only WHILE a cooldown runs (ROADMAP's "soft
        // push-apart while cooldowns run, merge on contact once both timers
        // expire"). Once both have expired the pair overlaps freely — that is
        // what lets deliberate convergent steering drive the centres into the
        // deep merge window above; unconditional correction would park them
        // at touching forever and make steered remerging unreachable.
        // Full positional correction, half the penetration each, along the
        // centre axis. Pairs resolve in index order (the caller's loop),
        // which is the deterministic tie-break doctrine.
        const float penetration = (r_a + r_b) - dist;
        math::Vec2  axis        = math::normalized(delta);
        if (axis == math::Vec2{}) {
            // Exactly coincident centres — a zero-intent split — have no
            // axis; separate along +x, the deterministic tiebreak.
            axis = {1.0f, 0.0f};
        }
        const math::Vec2 shift = axis * (penetration * 0.5f);
        a.position -= shift;
        b.position += shift;
        // The shove must not push anyone through the wall (the integrate
        // phase's clamp has already run this tick).
        a.position.x = std::clamp(a.position.x, 0.0f, world.tuning.world_extent);
        a.position.y = std::clamp(a.position.y, 0.0f, world.tuning.world_extent);
        b.position.x = std::clamp(b.position.x, 0.0f, world.tuning.world_extent);
        b.position.y = std::clamp(b.position.y, 0.0f, world.tuning.world_extent);
    }
}

/// Same-owner resolution: merge or push apart every pair of one player's
/// alive Cells. Runs right after the eat pass, so this tick's meals are
/// already dead-marked and skipped.
///
/// Deliberately all-pairs per player rather than grid-driven: the
/// max_cells_per_player cap makes O(k²) free, and two giant siblings can
/// rest centre-to-centre far beyond grid_cell_size — past the candidate-pair
/// walk's completeness limit — so the grid would silently miss exactly the
/// pairs this phase exists for. (Invariant 6 forbids *broad-phase-free*
/// pairwise work over the world; a roster bounded by the cap, not by n, is
/// the documented exception.)
void resolve_same_owner(World& world)
{
    // Alive Cell indices, sorted owner-major then index-minor — a strict
    // total order, so std::sort is deterministic. Index order within an
    // owner is array order is spawn order: the same tie-break as eating.
    std::vector<std::uint32_t>& cells = world.cell_scratch;
    cells.clear();
    for (std::uint32_t i = 0; i < world.entities.size(); ++i) {
        const Entity& e = world.entities[i];
        if (e.kind == EntityKind::Cell && !e.dead) {
            cells.push_back(i);
        }
    }
    std::sort(cells.begin(), cells.end(),
              [&world](std::uint32_t lhs, std::uint32_t rhs) {
                  const PlayerId lo = world.entities[lhs].owner;
                  const PlayerId ro = world.entities[rhs].owner;
                  return lo != ro ? lo < ro : lhs < rhs;
              });

    std::size_t run_begin = 0;
    while (run_begin < cells.size()) {
        const PlayerId owner   = world.entities[cells[run_begin]].owner;
        std::size_t    run_end = run_begin + 1;
        while (run_end < cells.size() && world.entities[cells[run_end]].owner == owner) {
            ++run_end;
        }
        for (std::size_t i = run_begin; i < run_end; ++i) {
            Entity& a = world.entities[cells[i]];
            // `a` can die mid-scan only if it was the younger of a merge,
            // which index order rules out — but fields are public, so the
            // guard is defensive, not decorative.
            for (std::size_t j = i + 1; j < run_end && !a.dead; ++j) {
                Entity& b = world.entities[cells[j]];
                if (!b.dead) {
                    resolve_sibling_pair(world, a, b);
                }
            }
        }
        run_begin = run_end;
    }
}

/// Mass decay. The e^(−λ·dt) form is what keeps invariant 3: the loss over a
/// second is identical however that second is sliced, where a per-tick
/// multiplicative constant would decay faster at higher tick rates.
void apply_decay(World& world, float dt)
{
    const float factor = std::exp(-world.tuning.decay_rate * dt);
    for (Entity& e : world.entities) {
        if (e.kind == EntityKind::Cell && !e.dead && e.mass > world.tuning.decay_threshold) {
            // Floored at the threshold: decay taxes snowballing, it never
            // starves anyone — deaths come only from being eaten.
            e.mass = std::max(world.tuning.decay_threshold, e.mass * factor);
        }
    }
}

/// Tops the pellet field back up to target count from world.rng — with the
/// seed fixed, the field is part of the replay (invariant 3). The very first
/// step sows the whole field, a one-time cost. New pellets are not in the
/// grid built earlier this step, which is harmless: nothing may eat them
/// until the next tick, and the end-of-step rebuild indexes them.
void respawn_pellets(World& world)
{
    int alive = 0;
    for (const Entity& e : world.entities) {
        if (e.kind == EntityKind::Pellet && !e.dead) {
            ++alive;
        }
    }
    for (int k = alive; k < world.tuning.target_pellet_count; ++k) {
        // Two named draws so the x-before-y order is explicit and replayable.
        const float x = rand_unit(world.rng) * world.tuning.world_extent;
        const float y = rand_unit(world.rng) * world.tuning.world_extent;
        spawn(world, EntityKind::Pellet, {x, y}, world.tuning.pellet_mass);
    }
}

/// Tops the virus field up to target, exactly like the pellet field and in
/// the same phase (pellet draws first, then virus draws — a fixed order the
/// replay depends on). Refill only ever ADDS: a feed-split can push the
/// population above target, and the surplus stands — despawning to correct
/// it would yank live terrain out from under the players hiding on it. Like
/// fresh pellets, new viruses miss this step's grid and are indexed by the
/// end-of-step rebuild; nothing can pop or feed them until the next tick.
void respawn_viruses(World& world)
{
    int alive = 0;
    for (const Entity& e : world.entities) {
        if (e.kind == EntityKind::Virus && !e.dead) {
            ++alive;
        }
    }
    for (int k = alive; k < world.tuning.target_virus_count; ++k) {
        const float x = rand_unit(world.rng) * world.tuning.world_extent;
        const float y = rand_unit(world.rng) * world.tuning.world_extent;
        spawn(world, EntityKind::Virus, {x, y}, world.tuning.virus_mass);
    }
}

} // namespace

World make_world(std::uint32_t seed)
{
    // Spelled as default-construct + seed() rather than a partial designated
    // initializer, which -Wmissing-designated-field-initializers rejects
    // under warnings-as-errors. Same result: every other field keeps its
    // NSDMI default.
    World world;
    world.rng.seed(seed);
    return world;
}

float speed_for_mass(const Tuning& tuning, float mass) noexcept
{
    // Placeholder curve, deliberately simple; the numbers live in Tuning now.
    // The clamp and the /10 pin the curve to its anchor: base_speed is
    // *defined* as the speed at mass 10, and nothing lighter moves faster.
    const float m = std::max(mass, 10.0f);
    return tuning.base_speed * std::pow(m / 10.0f, tuning.speed_mass_exponent);
}

EntityId spawn(World& world, EntityKind kind, math::Vec2 position, float mass, PlayerId owner)
{
    const EntityId id = world.next_id++;
    world.entities.push_back(Entity{
        .id = id,
        .owner = owner,
        .kind = kind,
        .position = position,
        .velocity = {},
        .impulse = {},
        .mass = mass,
        .merge_cooldown = 0.0f,
        .feed_count = 0,
        .last_feed_dir = {},
        .dead = false,
    });
    return id;
}

void apply_intent(World& world, const PlayerIntent& intent)
{
    const auto it = std::ranges::find(world.intents, intent.player, &PlayerIntent::player);
    if (it == world.intents.end()) {
        world.intents.push_back(intent);
    } else {
        *it = intent;
    }
}

EntityId spawn_player(World& world, PlayerId player)
{
    // M5 safe spawn: up to safe_spawn_attempts draws, first safe one wins.
    // "Safe" = no alive Cell at or above safe_spawn_threat_mass within
    // safe_spawn_radius, answered by the standing grid — which describes the
    // world as of the last step(), so a threat that moved since is judged
    // one tick stale. Accepted and documented: placement is a comfort rule,
    // not a correctness one, and the alternative is a mid-call rebuild.
    //
    // Draws stop at the first safe hit — each draw consumes world.rng only
    // when actually made — so the draw count varies, but it is a pure
    // function of world state (rng state + the last step's grid), and a
    // replay therefore repeats the exact same sequence (invariant 3:
    // lifecycle calls are part of the replayed input, see the header).
    //
    // When nothing is safe the LAST draw stands: bounded work by
    // construction — never an infinite loop on a crowded map — and spawning
    // into danger beats not spawning at all.
    const int attempts = std::max(1, world.tuning.safe_spawn_attempts);   // 0 would place no one
    math::Vec2 at{};
    for (int attempt = 0; attempt < attempts; ++attempt) {
        // Two named draws so the x-before-y order is explicit and replayable.
        const float x = rand_unit(world.rng) * world.tuning.world_extent;
        const float y = rand_unit(world.rng) * world.tuning.world_extent;
        at = {x, y};
        bool threatened = false;
        for_each_in_circle(
            world.grid, at, world.tuning.safe_spawn_radius,
            [&world, &threatened](std::uint32_t index, math::Vec2) {
                // The grid may be stale (see above): despawns since the last
                // step shrink the array, so an out-of-range index is a legal
                // wrong-but-defined answer here, skipped defensively.
                if (static_cast<std::size_t>(index) >= world.entities.size()) {
                    return;
                }
                const Entity& e = world.entities[index];
                if (e.kind == EntityKind::Cell && !e.dead &&
                    e.mass >= world.tuning.safe_spawn_threat_mass) {
                    threatened = true;
                }
            });
        if (!threatened) {
            break;
        }
    }
    return spawn(world, EntityKind::Cell, at, world.tuning.spawn_mass, player);
}

void despawn_player(World& world, PlayerId player)
{
    // Immediate and silent: disconnect is not death, so no event. The grid is
    // stale until the next step — its contract has always been "the world as
    // of the last step()".
    std::erase_if(world.entities, [player](const Entity& e) { return e.owner == player; });
    // The pending intent goes too, or a later spawn_player with a recycled
    // PlayerId would inherit a ghost cursor from the previous owner.
    std::erase_if(world.intents,
                  [player](const PlayerIntent& in) { return in.player == player; });
}

void step(World& world, float dt)
{
    // Events describe the most recent step only. clear() keeps capacity —
    // steady state allocates nothing here, same discipline as the grid.
    world.events.eats.clear();
    world.events.deaths.clear();

    // 0. One-shot actions (split/eject), before intent shapes velocity, so a
    //    fresh half steers on the very tick it is born. Consumes the flags —
    //    see resolve_actions.
    resolve_actions(world);

    // 1. Intent -> velocity. Cells drift toward the cursor at a mass-dependent
    //    speed; there is no acceleration term yet, which is what makes
    //    client-side prediction of your own cell near-trivial.
    for (const PlayerIntent& intent : world.intents) {
        for (Entity& e : world.entities) {
            if (e.kind != EntityKind::Cell || e.owner != intent.player) {
                continue;
            }
            e.velocity = intent.direction * speed_for_mass(world.tuning, e.mass);
        }
    }

    // 2. Integrate and clamp to the world square. Impulses (and EjectedMass
    //    flight, which lives in `velocity` — see the Entity field comment)
    //    decay exponentially, and their per-step displacement uses the
    //    *exact* integral of that decay,
    //        ∫₀^dt v·e^(−λt) dt  =  v·(1 − e^(−λ·dt))/λ,
    //    not the rectangle v·dt. The rectangle would break invariant 3: its
    //    travel sum depends on the slicing (~16% between dt 0.1 and 0.01 at
    //    λ = 3.5), while the exact form telescopes across any split of the
    //    same span — Σₖ v·e^(−λ·k·dt)·(1−e^(−λ·dt))/λ = v·(1−e^(−λT))/λ —
    //    the same composition argument apply_decay makes for mass.
    const float damping = std::exp(-world.tuning.impulse_damping_rate * dt);
    // λ → 0 would leave (1−1)/0; the limit of (1−e^(−λ·dt))/λ is dt, which
    // is also exactly right: no damping means plain rectangle integration.
    const float glide = world.tuning.impulse_damping_rate > 0.0f
                            ? (1.0f - damping) / world.tuning.impulse_damping_rate
                            : dt;
    for (Entity& e : world.entities) {
        const bool decaying_flight = e.kind == EntityKind::EjectedMass;
        e.position += e.velocity * (decaying_flight ? glide : dt) + e.impulse * glide;
        e.impulse *= damping;
        if (decaying_flight) {
            e.velocity *= damping;
        }
        e.position.x = std::clamp(e.position.x, 0.0f, world.tuning.world_extent);
        e.position.y = std::clamp(e.position.y, 0.0f, world.tuning.world_extent);
        // Floored by assignment at exactly 0.0f — the merge gate tests == 0 —
        // and dt-scaled like everything else (invariant 3).
        e.merge_cooldown = std::max(0.0f, e.merge_cooldown - dt);
    }

    // 3. Broad phase over the post-integration positions — what eat
    //    resolution queries. Everything moves every tick, so a rebuild beats
    //    incremental maintenance.
    rebuild(world.grid, world.entities, world.tuning.world_extent,
            world.tuning.grid_cell_size);

    // 4. Eating — marks only, no removal, so indices stay aligned with (3).
    //    Includes M5's pop rule, whose bursts append mid-pass (see
    //    resolve_eating for why that is safe).
    resolve_eating(world);

    // 5. Virus feeding (M5), after eating — cells outrank viruses for a
    //    contested pellet, and popped viruses no longer feed — and before
    //    same-owner resolution, mirroring the eat pass it extends.
    feed_viruses(world);

    // 6. Same-owner resolution (push-apart / merge), after eating so this
    //    tick's meals are out of the running. All-pairs per player, NOT via
    //    the grid — see resolve_same_owner for why.
    resolve_same_owner(world);

    // 7. Decay, dt-scaled (see apply_decay).
    apply_decay(world, dt);

    // 8. Pellet then virus respawn, from the injected PRNG in that fixed
    //    draw order.
    respawn_pellets(world);
    respawn_viruses(world);

    // 9. Compact. std::erase_if is the stable O(n) walk; the ROADMAP sketched
    //    swap-remove, but this array is walked wholesale every tick anyway,
    //    so stability costs nothing extra and keeps spawn order — which is
    //    the eat-resolution order — intact across compactions.
    std::erase_if(world.entities, [](const Entity& e) { return e.dead; });

    // 10. Rebuild over the *final* array: between-steps queries must see what
    //    the world now contains (indices shifted in (9), pellets and viruses
    //    arrived in (8)). Two O(n) rebuilds per tick is the obviously-correct
    //    choice over cleverness about staleness — an optimization candidate,
    //    not debt.
    rebuild(world.grid, world.entities, world.tuning.world_extent,
            world.tuning.grid_cell_size);

    ++world.tick;
}

} // namespace blob::sim
