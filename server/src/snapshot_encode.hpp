#pragma once

#include <blob/net/protocol.hpp>
#include <blob/sim/world.hpp>

#include <algorithm>
#include <cstddef>
#include <span>
#include <vector>

// The designated Entity -> wire edge. core/net is standalone by decision (its
// wire types are raw integers and it includes no <blob/sim/...>), so the
// sim -> net casts and quantization calls live here — in server/, next to the
// broadcast that consumes them — and nowhere else.

namespace blob::server {

/// Clears and refills `out` with one wire record per world entity, in
/// entity-array order. The vector is caller-owned so the 20 Hz loop reuses
/// one allocation instead of churning per tick.
void collect_records(const blob::sim::World& world,
                     std::vector<blob::net::EntityRecord>& out);

/// Slices `records` into subspans of at most net::max_entities_per_chunk and
/// calls fn(chunk) for each; returns the chunk count. Zero records means zero
/// chunks — with no entities there is nothing worth a datagram. This is the
/// shipped slicing (main.cpp broadcasts through it), which is what makes the
/// chunk arithmetic unit-testable.
template <typename Fn>
std::size_t for_each_chunk(std::span<const blob::net::EntityRecord> records, Fn&& fn)
{
    std::size_t chunks = 0;
    for (std::size_t offset = 0; offset < records.size();
         offset += blob::net::max_entities_per_chunk) {
        const std::size_t count =
            std::min(blob::net::max_entities_per_chunk, records.size() - offset);
        fn(records.subspan(offset, count));
        ++chunks;
    }
    return chunks;
}

} // namespace blob::server
