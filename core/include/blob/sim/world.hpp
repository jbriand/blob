#pragma once

#include <blob/math/vec2.hpp>
#include <blob/sim/spatial_grid.hpp>
#include <blob/sim/tuning.hpp>

#include <cstdint>
#include <random>
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
    /// Decaying launch velocity — the split kick. A separate field on
    /// purpose: intent overwrites `velocity` every tick, so an impulse
    /// stored there would vanish after one step. This one is ADDED during
    /// integration and damped by e^(−impulse_damping_rate·dt), which keeps
    /// it frame-rate independent (invariant 3). EjectedMass has no intent,
    /// so its flight lives in `velocity` instead and decays by the same λ.
    /// Server-side only — never crosses the wire (`EntityRecord` and
    /// `protocol_version` are untouched by M4).
    math::Vec2  impulse{};
    float       mass{};
    /// Seconds until this Cell may merge with a same-owner sibling. Armed by
    /// split (base + per-mass scale), dt-decremented by step() and floored
    /// at exactly 0.0f — which is what the merge gate tests. Server-side
    /// only, like `impulse`.
    float       merge_cooldown{};
    /// Ejected-mass hits this Virus has absorbed since its last feed-split
    /// (meaningful only while kind == Virus; reset to 0 by the split).
    /// Server-side only, like `impulse` — feed state never crosses the wire
    /// (`EntityRecord` and `protocol_version` are untouched by M5).
    std::uint8_t feed_count{};
    /// Direction of the most recent feed — the launch direction of the
    /// split a full feed_count fires, so the last feeder aims the virus.
    /// Same server-side-only rule as `feed_count`.
    math::Vec2  last_feed_dir{};
    /// Marked by eat resolution (and same-owner merges) inside step() and
    /// compacted away before the step returns — never true between steps,
    /// so it never crosses the wire (`EntityRecord` and `protocol_version`
    /// are untouched by M3).
    bool        dead{};
};

/// Per-tick intent for one player, already decoded from the wire.
struct PlayerIntent {
    PlayerId   player{};
    math::Vec2 direction{};   ///< unit vector, or {0,0} for "hold still"
    bool       split{};
    bool       eject{};
};

/// One meal, in the order it was resolved.
struct EatEvent {
    EntityId eater{};
    EntityId eaten{};
};

/// What the most recent step() removed or killed, for the layers above: the
/// server turns these into respawns and Goodbye messages, the client into
/// effects — core stays session-blind either way. Deliberately a field on
/// World rather than a step() return value: callers that ignore events keep
/// calling `step(world, dt)` unchanged, and the vectors' capacity is reused
/// tick to tick. Cleared at the top of every step.
struct StepEvents {
    std::vector<EatEvent> eats;     ///< every meal this step, in resolution order
    std::vector<PlayerId> deaths;   ///< players whose last Cell was eaten this step
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
    SpatialGrid               grid;       ///< broad phase, rebuilt by step() — derived scratch, never authoritative

    /// The injected PRNG invariant 3 demands (no global RNG). The seed is
    /// part of the replayed input — same seed + same call sequence gives an
    /// identical world — and every consumer (pellet and virus respawn,
    /// spawn_player) draws from here and nowhere else.
    std::mt19937              rng;

    StepEvents                events;     ///< what the last step() did (see StepEvents)

    /// step() scratch (owners holding an alive Cell before/after eat
    /// resolution). Fields purely for capacity reuse across ticks; the
    /// contents mean nothing outside step().
    std::vector<PlayerId>     owners_before_scratch;
    std::vector<PlayerId>     owners_after_scratch;

    /// step() scratch (alive Cell indices, grouped by owner) for same-owner
    /// resolution — same capacity-reuse-only contract as above.
    std::vector<std::uint32_t> cell_scratch;
};

/// A world whose PRNG starts from `seed`. The seed is part of the replayed
/// input: determinism (invariant 3) includes it, so a replay starts from
/// make_world(same seed). A default-constructed World{} stays legal too —
/// default-seeded rng — which is what most movement tests use.
[[nodiscard]] World make_world(std::uint32_t seed);

/// Ids are monotonic and never reused, and step()'s compaction (stable
/// erase_if) shifts indices and removes — nothing may hold an index across a
/// step; hold the id instead.
EntityId spawn(World& world, EntityKind kind, math::Vec2 position, float mass,
               PlayerId owner = 0);

/// Upserts the latest intent for that player: one intent per player per tick.
void apply_intent(World& world, const PlayerIntent& intent);

/// One starting Cell of `spawn_mass`, placed by the M5 safe-spawn rule: up
/// to safe_spawn_attempts PRNG draws, the first with no threat-sized Cell
/// (mass >= safe_spawn_threat_mass) within safe_spawn_radius wins, and when
/// nothing is safe the last draw stands. Threats are judged against the
/// standing grid — the world as of the last step(). Consumes world.rng — a
/// variable number of draws, but a pure function of world state — so
/// lifecycle calls stay part of the deterministic input sequence: a replay
/// must repeat them in the same order relative to step().
EntityId spawn_player(World& world, PlayerId player);

/// Removes everything the player owns, immediately, plus any pending intent.
/// Disconnect is not death: no death event is recorded. The grid keeps its
/// standing contract — it describes the world as of the last step() — so it
/// is stale (may hand out removed indices) until the next step.
void despawn_player(World& world, PlayerId player);

void step(World& world, float dt);

/// Speed falls off with mass — this is what makes big cells vulnerable and
/// keeps the game from degenerating into "biggest also fastest". Shape and
/// anchor come from `tuning` (base_speed, speed_mass_exponent).
[[nodiscard]] float speed_for_mass(const Tuning& tuning, float mass) noexcept;

} // namespace blob::sim
