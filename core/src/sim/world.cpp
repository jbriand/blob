#include <blob/sim/world.hpp>

#include <algorithm>
#include <cmath>
#include <type_traits>

namespace blob::sim {

// The plain-struct rule, asserted rather than assumed. World holds vectors so
// it is not trivially copyable, but it must stay aggregate-initializable.
static_assert(std::is_aggregate_v<World>);
static_assert(std::is_aggregate_v<Entity>);
static_assert(std::is_aggregate_v<PlayerIntent>);

float speed_for_mass(float mass) noexcept
{
    // Placeholder curve, deliberately simple: base speed scaled by m^-0.44,
    // which is roughly the shape the original uses. Tune once there is
    // something to play against.
    constexpr float base_speed = 720.0f;   // world units / second at mass 10
    const float m = std::max(mass, 10.0f);
    return base_speed * std::pow(m / 10.0f, -0.44f);
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
            e.velocity = intent.direction * speed_for_mass(e.mass);
        }
    }

    // 2. Integrate and clamp to the world square.
    for (Entity& e : world.entities) {
        e.position += e.velocity * dt;
        e.position.x = std::clamp(e.position.x, 0.0f, world_extent);
        e.position.y = std::clamp(e.position.y, 0.0f, world_extent);
    }

    // TODO(spatial): uniform grid rebuild + collision resolution goes here,
    // before eat/split/merge. O(n^2) over thousands of pellets is fatal, so
    // nothing broad-phase-free should ever land in this loop.

    ++world.tick;
}

} // namespace blob::sim
