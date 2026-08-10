#include <blob/sim/spatial_grid.hpp>

#include <blob/sim/world.hpp>

#include <algorithm>
#include <cmath>
#include <type_traits>

namespace blob::sim {

// The plain-struct rule (CLAUDE.md § Conventions), asserted rather than
// assumed. SpatialGrid holds vectors so aggregate is all it can promise.
static_assert(std::is_aggregate_v<SpatialGrid>);
static_assert(std::is_aggregate_v<GridEntry>);
static_assert(std::is_trivially_copyable_v<GridEntry>);

void rebuild(SpatialGrid& grid, std::span<const Entity> entities, float extent,
             float cell_size)
{
    // Geometry first: cell_coord() below reads these fields. Degenerate
    // tuning (non-positive extent or cell) collapses to a single bucket, and
    // the cap keeps a garbage extent/cell ratio from a float->int cast that
    // would be UB and from a multi-gigabyte starts array — wrong-but-defined,
    // as everywhere else with public fields. Sane tunings sit far below it
    // (the default is 8192/256 = 32 columns).
    constexpr float max_cols = 4096.0f;
    grid.cell_size = cell_size;
    const float cells = (cell_size > 0.0f && extent > 0.0f)
                            ? std::min(std::ceil(extent / cell_size), max_cols)
                            : 1.0f;
    grid.cols = std::max(1, static_cast<std::int32_t>(cells));

    const auto cols = static_cast<std::size_t>(grid.cols);
    const std::size_t bucket_count = cols * cols;

    // assign/resize instead of fresh vectors: after the first few steps both
    // capacities plateau and rebuilding allocates nothing.
    grid.starts.assign(bucket_count + 1, 0);
    grid.entries.resize(entities.size());

    const auto bucket_of = [&grid, cols](math::Vec2 p) noexcept {
        return static_cast<std::size_t>(cell_coord(grid, p.y)) * cols +
               static_cast<std::size_t>(cell_coord(grid, p.x));
    };

    // Pass 1: bucket sizes into starts[b + 1]...
    for (const Entity& e : entities) {
        ++grid.starts[bucket_of(e.position) + 1];
    }
    // ...prefix-summed into CSR offsets: starts[b] = first entry of bucket b.
    for (std::size_t b = 1; b <= bucket_count; ++b) {
        grid.starts[b] += grid.starts[b - 1];
    }

    // Pass 2: place entries, using starts[b] as bucket b's write cursor.
    // Walking the input span in order keeps every bucket in input order, so
    // the layout is a pure function of the input — deterministic, same
    // input -> bit-identical grid (invariant 3).
    for (std::size_t i = 0; i < entities.size(); ++i) {
        const std::size_t b = bucket_of(entities[i].position);
        grid.entries[static_cast<std::size_t>(grid.starts[b]++)] = GridEntry{
            .index = static_cast<std::uint32_t>(i),
            .position = entities[i].position,
        };
    }

    // The cursor pass left starts[b] holding bucket b's *end*; rotating right
    // by one restores begin offsets without a scratch array (bucket b's begin
    // is bucket b-1's end, and bucket 0 begins at 0).
    for (std::size_t b = bucket_count; b > 0; --b) {
        grid.starts[b] = grid.starts[b - 1];
    }
    grid.starts[0] = 0;
}

} // namespace blob::sim
