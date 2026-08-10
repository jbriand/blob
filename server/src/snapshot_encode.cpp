#include "snapshot_encode.hpp"

#include <blob/net/quantize.hpp>

namespace blob::server {

void collect_records(const blob::sim::World& world,
                     std::vector<blob::net::EntityRecord>& out)
{
    out.clear();
    out.reserve(world.entities.size());
    for (const blob::sim::Entity& e : world.entities) {
        out.push_back(blob::net::EntityRecord{
            .id    = e.id,
            .owner = e.owner,
            .kind  = static_cast<std::uint8_t>(e.kind),
            .x     = blob::net::quantize_position(e.position.x, world.tuning.world_extent),
            .y     = blob::net::quantize_position(e.position.y, world.tuning.world_extent),
            .mass  = blob::net::quantize_mass(e.mass),
        });
    }
}

} // namespace blob::server
