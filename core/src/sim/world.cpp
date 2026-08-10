#include <blob/sim/world.hpp>

#include <blob/sim/spatial_grid.hpp>

#include <algorithm>
#include <cmath>
#include <type_traits>

namespace blob::sim {

// The plain-struct rule, asserted rather than assumed. World holds vectors so
// it is not trivially copyable, but it must stay aggregate-initializable.
static_assert(std::is_aggregate_v<World>);
static_assert(std::is_aggregate_v<Entity>);
static_assert(std::is_aggregate_v<PlayerIntent>);

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

void step(World& world, float dt)
{
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

    // 3. Broad phase over the post-integration positions. Everything moves
    //    every tick, so a rebuild beats incremental maintenance; nothing
    //    consumes the grid inside step() yet.
    rebuild(world.grid, world.entities, world.tuning.world_extent,
            world.tuning.grid_cell_size);

    // TODO(collision): eat/overlap resolution goes here, off the grid only —
    // for_each_candidate_pair / for_each_in_circle, never raw O(n^2)
    // (invariant 6).

    ++world.tick;
}

} // namespace blob::sim
