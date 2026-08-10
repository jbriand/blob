#include <blob/sim/interest.hpp>

#include <blob/sim/spatial_grid.hpp>
#include <blob/sim/world.hpp>

#include <algorithm>
#include <cmath>

namespace blob::sim {

float view_radius(const Tuning& tuning, float total_mass) noexcept
{
    return tuning.view_base +
           tuning.view_mass_factor * std::sqrt(std::max(total_mass, 0.0f));
}

void collect_visible(const World& world, math::Vec2 centre, float radius,
                     std::vector<std::uint32_t>& out)
{
    out.clear();
    for_each_in_circle(world.grid, centre, radius,
                       [&out](std::uint32_t index, math::Vec2) { out.push_back(index); });
}

} // namespace blob::sim
