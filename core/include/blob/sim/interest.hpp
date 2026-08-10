#pragma once

#include <blob/math/vec2.hpp>
#include <blob/sim/tuning.hpp>

#include <cstdint>
#include <vector>

// Interest management: what one player can see, answered by the grid. Pure
// queries — the per-peer budgeting and encoding that consume them live in
// server/ (this module has no idea sessions or packets exist).

namespace blob::sim {

struct World;

/// How far a player sees: view_base + view_mass_factor · √(total cell mass).
/// The √ law is deliberate — drawn radius is r = k·√m, so the view widens at
/// exactly the rate the player's own cells grow on screen; anything shallower
/// and a big cell would outgrow its camera. view_base keeps a fresh spawn
/// seeing something, and max(mass, 0) floors the degenerate inputs a session
/// can produce (mass 0 mid-respawn, or a negative from a bug) at view_base.
[[nodiscard]] float view_radius(const Tuning& tuning, float total_mass) noexcept;

/// Clears `out` and fills it with the indices (into world.entities) of every
/// entity within `radius` of `centre`, boundary inclusive, answered by
/// world.grid. Order is the grid's bucket order, which is deterministic — a
/// pure function of the entity array — so per-peer snapshot content never
/// depends on anything unordered. Indices are valid only until the next
/// step() (compaction shifts them; hold EntityIds across steps, never
/// indices), and the grid describes the world as of the last step().
void collect_visible(const World& world, math::Vec2 centre, float radius,
                     std::vector<std::uint32_t>& out);

} // namespace blob::sim
