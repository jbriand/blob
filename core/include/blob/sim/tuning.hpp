#pragma once

#include <algorithm>
#include <cmath>
#include <type_traits>

namespace blob::sim {

/// Every gameplay constant in one aggregate — data, not scattered `constexpr`,
/// so a future server-side config file can override values at startup while
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
    /// grazing pellets. Also the anchor mass of the speed curve.
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
