#pragma once

#include <algorithm>
#include <cmath>
#include <type_traits>

namespace blob::sim {

/// Every gameplay constant in one aggregate — data, not scattered `constexpr`,
/// so the server-side config file (server/src/config.cpp) overrides values at
/// startup while
/// `core` stays free of I/O (the parsing will live in server/, never here).
/// Derived values (`tick_dt`) are functions of the struct rather than stored
/// fields, so an override can never leave a stale companion constant behind.
struct Tuning {
    /// Simulation rate, Hz. Server-authoritative: announced to clients in
    /// Welcome, never negotiated — the client adapts to whatever this says.
    int tick_rate = 20;

    /// Side of the square world, world units. Doubles as the denominator of
    /// the wire's position quantization, so changing it changes how coarse a
    /// u16 position step looks on screen (see CLAUDE.md § Quantization).
    float world_extent = 8192.0f;

    /// Speed of a mass-10 cell, world units/s — the anchor of the speed
    /// curve. Tune this one for overall game pace.
    float base_speed = 720.0f;

    /// Exponent of the mass→speed falloff (agar-like shape). Must stay
    /// negative: big has to be slow, or the biggest cell would also be the
    /// fastest and nothing could ever escape it.
    float speed_mass_exponent = -0.44f;

    /// r = radius_factor·√mass. √mass keeps drawn *area* proportional to
    /// mass; the factor is a placeholder until a playtest tunes it.
    float radius_factor = 4.0f;

    /// Broad-phase bucket side, world units. Must stay >= the largest
    /// pair-interaction distance, or `for_each_candidate_pair` silently
    /// misses pairs — anything with a longer reach (a big cell's eat radius)
    /// must go through `for_each_in_circle` instead.
    float grid_cell_size = 256.0f;

    /// A cell eats another cell only above this mass ratio. Strictly > 1 on
    /// purpose: at 1.0 near-equal cells would eat each other on touch and
    /// every skirmish would end in a coin flip — "bigger" has to be a real,
    /// earnable edge before it grants a kill.
    float eat_ratio = 1.25f;

    /// How deep the victim's centre must sit inside the eater before a
    /// cell-vs-cell eat lands: dist <= r_eater − factor·r_victim. Rim contact
    /// is deliberately not enough — committing to the overlap is what makes
    /// near-misses readable and escapes possible.
    float eat_depth_factor = 1.0f / 3.0f;

    /// Pellet population step() maintains; eaten pellets respawn the same
    /// tick. This is map density (idle income everywhere), not an economy —
    /// the field never runs dry.
    int target_pellet_count = 2000;

    /// Mass of one pellet. Exactly 1 keeps early growth countable, and the
    /// linear wire encoding shows it exactly (see CLAUDE.md § Quantization).
    float pellet_mass = 1.0f;

    /// Starting Cell mass for a fresh player: heavy enough to not be instant
    /// food for another spawn, light enough that the first minute is spent
    /// grazing pellets. (The speed curve's anchor is a separate literal 10 in
    /// speed_for_mass — changing this does not move that curve.)
    float spawn_mass = 10.0f;

    /// Decay taxes only mass above this line and never drags a cell below it
    /// (the decay floor). Small cells keep everything, and starvation deaths
    /// are impossible — dying takes being eaten.
    float decay_threshold = 200.0f;

    /// Exponential decay rate λ, per second, applied as e^(−λ·dt) so the loss
    /// over a second is the same at any tick rate (invariant 3 — a per-tick
    /// factor would silently break frame-rate independence). Gentle by
    /// design: an anti-snowball drag, not a diet.
    float decay_rate = 0.002f;

    // -- M4: split / eject / merge -------------------------------------------
    // Knobs pre-staged with the M4 iteration's pinned placeholders so parallel
    // branches never touch this file concurrently; the mechanics land in M4.

    /// A cell may split only at or above this mass, so each half stays a
    /// viable cell rather than instant food.
    float min_split_mass = 36.0f;

    /// Hard ceiling on cells per player (the classic 16). Also what keeps a
    /// virus pop punishing instead of infinite.
    int max_cells_per_player = 16;

    /// Initial speed of the launched split half along the intent direction,
    /// world units/s; decays via impulse_damping_rate.
    float split_impulse_speed = 780.0f;

    /// Exponential damping λ (per second) for Entity impulse velocity and for
    /// EjectedMass flight — e^(−λ·dt) form, invariant 3, same as decay_rate.
    float impulse_damping_rate = 3.5f;

    /// Merge cooldown = base + per_mass · mass-at-split, seconds. Mass-scaled
    /// so big splits stay committed longer.
    float merge_cooldown_base = 10.0f;
    float merge_cooldown_per_mass = 0.02f;

    /// Same-owner cells merge when centres are closer than
    /// merge_overlap · max(r_a, r_b), once both cooldowns have expired.
    float merge_overlap = 0.25f;

    /// Eject: the cell pays eject_mass_cost, the pellet carries ejected_mass,
    /// and the difference evaporates — ejecting must never print mass. Cells
    /// below min_eject_mass cannot eject at all.
    float min_eject_mass = 35.0f;
    float eject_mass_cost = 18.0f;
    float ejected_mass = 14.0f;

    /// Launch speed of an ejected pellet, world units/s; damped by
    /// impulse_damping_rate.
    float eject_speed = 1400.0f;

    // -- M5: viruses & safe spawn ----------------------------------------------
    // Pre-staged like the M4 block; the mechanics land in M5.

    /// Virus population is maintained like pellets: refilled to this count
    /// from the injected PRNG.
    int target_virus_count = 40;

    /// Mass of a fresh virus, and the reference the pop gate scales from.
    float virus_mass = 100.0f;

    /// A pop tries to burst the cell into this many pieces, capped by
    /// max_cells_per_player.
    int virus_pop_pieces = 8;

    /// Ejected-mass hits a virus absorbs before it splits toward the feeder.
    int virus_feed_count = 7;

    /// Safe spawn: try up to safe_spawn_attempts PRNG positions keeping
    /// safe_spawn_radius clear of any cell at or above safe_spawn_threat_mass;
    /// the last attempt stands regardless (bounded work, deterministic).
    float safe_spawn_radius = 600.0f;
    float safe_spawn_threat_mass = 80.0f;
    int   safe_spawn_attempts = 16;

    // -- M6: interest management -----------------------------------------------
    // Pre-staged like the blocks above; the queries land in M6.

    /// Visible radius = view_base + view_mass_factor · √(total player mass):
    /// everyone sees something at spawn, and growth widens the view following
    /// the same √ law as drawn radius.
    float view_base = 640.0f;
    float view_mass_factor = 24.0f;
};

/// What the game ships with; also the values the tests pin against.
inline constexpr Tuning default_tuning{};

// Plain-struct rule (CLAUDE.md § Conventions), asserted rather than assumed.
static_assert(std::is_aggregate_v<Tuning>);
static_assert(std::is_trivially_copyable_v<Tuning>);

/// Seconds per simulated tick, derived on demand — 1 / tick_rate. Deriving
/// instead of storing means a config override of tick_rate cannot leave a
/// stale dt behind.
[[nodiscard]] constexpr float tick_dt(const Tuning& tuning) noexcept
{
    return 1.0f / static_cast<float>(tuning.tick_rate);
}

/// Collision/drawn radius for a mass. Zero-safe: a negative mass is a bug,
/// but with public fields it is a representable one, so it yields radius 0
/// rather than NaN.
[[nodiscard]] inline float radius_for_mass(const Tuning& tuning, float mass) noexcept
{
    return tuning.radius_factor * std::sqrt(std::max(mass, 0.0f));
}

} // namespace blob::sim
