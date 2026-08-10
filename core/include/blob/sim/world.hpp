#pragma once

#include <blob/math/vec2.hpp>
#include <blob/sim/tuning.hpp>

#include <cstdint>
#include <vector>

namespace blob::sim {

using EntityId = std::uint32_t;
using PlayerId = std::uint16_t;

enum class EntityKind : std::uint8_t { Cell, Pellet, Virus, EjectedMass };

struct Entity {
    EntityId    id{};
    PlayerId    owner{};
    EntityKind  kind{EntityKind::Pellet};
    math::Vec2  position{};
    math::Vec2  velocity{};
    float       mass{};
};

/// Per-tick intent for one player, already decoded from the wire.
struct PlayerIntent {
    PlayerId   player{};
    math::Vec2 direction{};   ///< unit vector, or {0,0} for "hold still"
    bool       split{};
    bool       eject{};
};

/// The authoritative world. Frame-rate independent: `step` takes an explicit
/// dt and nothing here reads a clock, so the same call sequence replays
/// identically on the server, in tests, and in the client's prediction.
struct World {
    std::vector<Entity>       entities;
    std::vector<PlayerIntent> intents;
    EntityId                  next_id{1};
    std::uint64_t             tick{};
    Tuning                    tuning{};   ///< single source for every gameplay constant (see tuning.hpp)
};

/// Ids are monotonic and never reused, so nothing may hold an index across a
/// step once M3's compaction lands — hold the id instead.
EntityId spawn(World& world, EntityKind kind, math::Vec2 position, float mass,
               PlayerId owner = 0);

/// Upserts the latest intent for that player: one intent per player per tick.
void apply_intent(World& world, const PlayerIntent& intent);

void step(World& world, float dt);

/// Speed falls off with mass — this is what makes big cells vulnerable and
/// keeps the game from degenerating into "biggest also fastest". Shape and
/// anchor come from `tuning` (base_speed, speed_mass_exponent).
[[nodiscard]] float speed_for_mass(const Tuning& tuning, float mass) noexcept;

} // namespace blob::sim
