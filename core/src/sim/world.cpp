#include <blob/sim/world.hpp>

#include <blob/sim/spatial_grid.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
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

/// Eat resolution, against the grid built over this tick's positions. Marks
/// victims dead and transfers mass; removal waits for compaction so entity
/// indices stay aligned with the grid throughout the scan. Eaters run in
/// array order — that *is* the tie-break rule, and since spawn order fixes
/// array order, resolution is deterministic (invariant 3).
void resolve_eating(World& world)
{
    // Deaths are detected by difference: who owned an alive Cell before,
    // minus who still does after. Eating is the only killer in M3 — decay
    // floors above zero and despawn is explicitly not death.
    collect_cell_owners(world, world.owners_before_scratch);

    const std::size_t count = world.entities.size();
    for (std::size_t i = 0; i < count; ++i) {
        Entity& eater = world.entities[i];
        if (eater.kind != EntityKind::Cell || eater.dead) {
            continue;   // only live Cells eat; food and viruses never do
        }

        // Reach is the eater's radius at scan start: mass gained mid-scan
        // does not extend this arm until the next tick. Either convention
        // would be deterministic — this one just keeps one meal from
        // cascading into a longer reach within a single tick. The ratio gate
        // below *does* use live mass, so growth still counts there.
        const float r_e = radius_for_mass(world.tuning, eater.mass);

        // Both eat rules imply centre distance <= r_e, so one circle query
        // covers them. It must be for_each_in_circle, not candidate pairs:
        // a big eater's reach exceeds grid_cell_size, and this query is the
        // documented big-eater path (invariant 6 — never a raw pair scan).
        for_each_in_circle(
            world.grid, eater.position, r_e,
            [&world, &eater, i, r_e](std::uint32_t index, math::Vec2) {
                if (static_cast<std::size_t>(index) == i) {
                    return;   // self
                }
                Entity& victim = world.entities[index];
                if (victim.dead) {
                    return;   // already eaten earlier this tick
                }
                bool eaten = false;
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
                    break;   // inert until M5's pop rule
                }
                if (eaten) {
                    victim.dead = true;
                    eater.mass += victim.mass;   // mass moves, never vanishes
                    world.events.eats.push_back(
                        EatEvent{.eater = eater.id, .eaten = victim.id});
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
        .mass = mass,
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
    // Naive placement anywhere in the square, big-cell danger ignored —
    // safe-spawn is M5's job. The two draws consume world.rng, so lifecycle
    // calls are part of the deterministic input sequence (see the header).
    const float x = rand_unit(world.rng) * world.tuning.world_extent;
    const float y = rand_unit(world.rng) * world.tuning.world_extent;
    return spawn(world, EntityKind::Cell, {x, y}, world.tuning.spawn_mass, player);
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

    // 2. Integrate and clamp to the world square.
    for (Entity& e : world.entities) {
        e.position += e.velocity * dt;
        e.position.x = std::clamp(e.position.x, 0.0f, world.tuning.world_extent);
        e.position.y = std::clamp(e.position.y, 0.0f, world.tuning.world_extent);
    }

    // 3. Broad phase over the post-integration positions — what eat
    //    resolution queries. Everything moves every tick, so a rebuild beats
    //    incremental maintenance.
    rebuild(world.grid, world.entities, world.tuning.world_extent,
            world.tuning.grid_cell_size);

    // 4. Eating — marks only, no removal, so indices stay aligned with (3).
    resolve_eating(world);

    // 5. Decay, dt-scaled (see apply_decay).
    apply_decay(world, dt);

    // 6. Pellet respawn, from the injected PRNG.
    respawn_pellets(world);

    // 7. Compact. std::erase_if is the stable O(n) walk; the ROADMAP sketched
    //    swap-remove, but this array is walked wholesale every tick anyway,
    //    so stability costs nothing extra and keeps spawn order — which is
    //    the eat-resolution order — intact across compactions.
    std::erase_if(world.entities, [](const Entity& e) { return e.dead; });

    // 8. Rebuild over the *final* array: between-steps queries must see what
    //    the world now contains (indices shifted in (7), pellets arrived in
    //    (6)). Two O(n) rebuilds per tick is the obviously-correct choice
    //    over cleverness about staleness — an optimization candidate, not
    //    debt.
    rebuild(world.grid, world.entities, world.tuning.world_extent,
            world.tuning.grid_cell_size);

    ++world.tick;
}

} // namespace blob::sim
