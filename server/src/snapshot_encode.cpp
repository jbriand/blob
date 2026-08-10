#include "snapshot_encode.hpp"

#include <blob/net/quantize.hpp>
#include <blob/sim/interest.hpp>

#include <algorithm>
#include <limits>

namespace blob::server {

namespace {

[[nodiscard]] blob::net::EntityRecord record_for(const blob::sim::Entity& e,
                                                 float world_extent)
{
    return blob::net::EntityRecord{
        .id    = e.id,
        .owner = e.owner,
        .kind  = static_cast<std::uint8_t>(e.kind),
        .x     = blob::net::quantize_position(e.position.x, world_extent),
        .y     = blob::net::quantize_position(e.position.y, world_extent),
        .mass  = blob::net::quantize_mass(e.mass),
    };
}

} // namespace

void collect_records(const blob::sim::World& world,
                     std::vector<blob::net::EntityRecord>& out)
{
    out.clear();
    out.reserve(world.entities.size());
    for (const blob::sim::Entity& e : world.entities) {
        out.push_back(record_for(e, world.tuning.world_extent));
    }
}

void collect_records_for(const blob::sim::World& world,
                         std::span<const std::uint32_t> indices,
                         std::vector<blob::net::EntityRecord>& out)
{
    out.clear();
    out.reserve(indices.size());
    for (const std::uint32_t index : indices) {
        if (index >= world.entities.size()) {
            continue;   // stale index: a missing record, never a stray read
        }
        out.push_back(record_for(world.entities[index], world.tuning.world_extent));
    }
}

void collect_visible_records(const blob::sim::World& world, blob::math::Vec2 centre,
                             float radius, std::size_t budget,
                             std::vector<std::uint32_t>& visible,
                             std::vector<blob::net::EntityRecord>& out)
{
    blob::sim::collect_visible(world, centre, radius, visible);
    if (visible.size() > budget) {
        // Over budget: keep the nearest — the far rim of the view is the part
        // whose absence a player notices least. The sort key is (dist² to the
        // centre, then index ascending): fully deterministic, so what a peer
        // receives is a pure function of the world, never of anything
        // unordered (allocator layout, hash order, ...).
        const auto dist_sq = [&](std::uint32_t index) {
            // Defensive on public data: an out-of-range index (a stale or
            // hand-corrupted grid) sorts past everything real and falls to
            // the truncation — wrong-but-defined, never a stray read.
            if (index >= world.entities.size()) {
                return std::numeric_limits<float>::infinity();
            }
            return blob::math::length_sq(world.entities[index].position - centre);
        };
        std::sort(visible.begin(), visible.end(), [&](std::uint32_t a, std::uint32_t b) {
            const float da = dist_sq(a);
            const float db = dist_sq(b);
            return da != db ? da < db : a < b;
        });
        visible.resize(budget);
    }
    collect_records_for(world, visible, out);
}

} // namespace blob::server
