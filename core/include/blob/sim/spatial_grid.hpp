#pragma once

#include <blob/math/vec2.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace blob::sim {

// Only rebuild()'s declaration needs Entity, and world.hpp includes this
// header (World carries a grid), so a forward declaration breaks the cycle.
struct Entity;

/// One bucketed copy: enough to answer both queries without touching the
/// entity array, which keeps the hot loops walking one contiguous allocation.
struct GridEntry {
    std::uint32_t index{};      ///< position in the span handed to rebuild()
    math::Vec2    position{};
};

/// Uniform broad-phase grid over the world square, flat CSR layout: bucket b
/// owns entries[starts[b] .. starts[b + 1]). Rebuilt from scratch every step —
/// everything moves every tick, so a rebuild beats incremental maintenance.
///
/// Entry indices are positions in the span passed to rebuild() and are valid
/// only until the next rebuild: step()'s compaction (stable erase_if) shifts
/// indices between steps, so nothing may hold one longer (hold the EntityId
/// instead).
struct SpatialGrid {
    float                      cell_size{};   ///< bucket side, world units
    std::int32_t               cols{};        ///< square grid: cols x cols buckets
    std::vector<std::uint32_t> starts;        ///< cols*cols + 1 CSR offsets
    std::vector<GridEntry>     entries;       ///< bucket-sorted copies
};

/// Rebuilds the grid over `entities` (two-pass counting sort, O(n), reusing
/// both vectors' capacity — steady-state zero allocation). Positions are
/// clamped into the grid, so `extent` itself — which step()'s world clamp
/// produces — lands in the last cell, not out of bounds. An empty span is
/// legal and yields a grid every query treats as empty.
void rebuild(SpatialGrid& grid, std::span<const Entity> entities, float extent,
             float cell_size);

/// Cell coordinate of one axis value, clamped into [0, cols - 1]. Written so
/// that any input — a position exactly at cols*cell_size, an out-of-world
/// float, even NaN — lands in a valid cell: fields are public, so this must
/// give a wrong-but-defined answer rather than an out-of-range cast.
[[nodiscard]] inline std::int32_t cell_coord(const SpatialGrid& grid, float v) noexcept
{
    const float f  = v / grid.cell_size;
    const float hi = static_cast<float>(grid.cols - 1);
    const float clamped = f >= hi ? hi : (f >= 0.0f ? f : 0.0f);   // NaN -> 0
    return static_cast<std::int32_t>(clamped);
}

/// Clamped [begin, end) entry range of bucket `bucket` (which must be
/// < cols*cols; queries verify the CSR shape before calling). The clamps are
/// invariant-7-style defensiveness: with public fields a hand-corrupted
/// `starts` must degrade to a wrong-but-defined traversal, never a stray read.
[[nodiscard]] inline std::pair<std::uint32_t, std::uint32_t>
bucket_range(const SpatialGrid& grid, std::size_t bucket) noexcept
{
    const auto limit = static_cast<std::uint32_t>(grid.entries.size());
    const std::uint32_t begin = std::min(grid.starts[bucket], limit);
    const std::uint32_t end   = std::min(std::max(grid.starts[bucket + 1], begin), limit);
    return {begin, end};
}

/// True when the CSR arrays describe a queryable grid; false for a
/// default-constructed (never rebuilt) or resized-by-hand one, which the
/// queries below treat as empty instead of indexing out of bounds.
[[nodiscard]] inline bool is_rebuilt(const SpatialGrid& grid) noexcept
{
    if (grid.cols <= 0) {
        return false;
    }
    const auto cols = static_cast<std::size_t>(grid.cols);
    return grid.starts.size() == cols * cols + 1;
}

/// Calls fn(index, position) for every entry with
/// length_sq(position - center) <= radius², boundary inclusive. Walks the
/// covered cell rectangle (clamped to the grid), so a radius larger than
/// cell_size works — this is the query M3's eating must use, because a big
/// cell's eat radius exceeds a bucket.
template <typename Fn>
void for_each_in_circle(const SpatialGrid& grid, math::Vec2 center, float radius, Fn&& fn)
{
    if (!is_rebuilt(grid)) {
        return;
    }
    const auto cols = static_cast<std::size_t>(grid.cols);

    const std::int32_t min_cx = cell_coord(grid, center.x - radius);
    const std::int32_t max_cx = cell_coord(grid, center.x + radius);
    const std::int32_t min_cy = cell_coord(grid, center.y - radius);
    const std::int32_t max_cy = cell_coord(grid, center.y + radius);
    const float r_sq = radius * radius;

    for (std::int32_t cy = min_cy; cy <= max_cy; ++cy) {
        for (std::int32_t cx = min_cx; cx <= max_cx; ++cx) {
            const std::size_t bucket =
                static_cast<std::size_t>(cy) * cols + static_cast<std::size_t>(cx);
            const auto [begin, end] = bucket_range(grid, bucket);
            for (std::uint32_t k = begin; k < end; ++k) {
                const GridEntry& entry = grid.entries[k];
                if (math::length_sq(entry.position - center) <= r_sq) {
                    fn(entry.index, entry.position);
                }
            }
        }
    }
}

/// Calls fn(i, j) for every unordered candidate pair exactly once: all pairs
/// within one bucket, plus pairs across the half neighbourhood
/// (+1,0),(0,+1),(-1,+1),(+1,+1) — each adjacent bucket pair is scanned from
/// exactly one side, so no pair is reported twice and never i == j.
///
/// Candidates are complete only for pair distance <= cell_size: two points
/// that close can sit at most one bucket apart on each axis. Anything with a
/// larger interaction radius must use for_each_in_circle instead (M3's eating
/// will — a big cell's radius exceeds cell_size).
template <typename Fn>
void for_each_candidate_pair(const SpatialGrid& grid, Fn&& fn)
{
    if (!is_rebuilt(grid)) {
        return;
    }
    const auto cols = static_cast<std::size_t>(grid.cols);
    constexpr std::int32_t offsets[4][2] = {{1, 0}, {0, 1}, {-1, 1}, {1, 1}};

    for (std::int32_t cy = 0; cy < grid.cols; ++cy) {
        for (std::int32_t cx = 0; cx < grid.cols; ++cx) {
            const std::size_t bucket =
                static_cast<std::size_t>(cy) * cols + static_cast<std::size_t>(cx);
            const auto [begin, end] = bucket_range(grid, bucket);
            if (begin == end) {
                continue;   // an empty bucket contributes to no pair in any direction
            }
            for (std::uint32_t a = begin; a < end; ++a) {
                for (std::uint32_t b = a + 1; b < end; ++b) {
                    fn(grid.entries[a].index, grid.entries[b].index);
                }
            }
            for (const auto& off : offsets) {
                const std::int32_t nx = cx + off[0];
                const std::int32_t ny = cy + off[1];
                if (nx < 0 || nx >= grid.cols || ny < 0 || ny >= grid.cols) {
                    continue;
                }
                const std::size_t neighbour =
                    static_cast<std::size_t>(ny) * cols + static_cast<std::size_t>(nx);
                const auto [nbegin, nend] = bucket_range(grid, neighbour);
                for (std::uint32_t a = begin; a < end; ++a) {
                    for (std::uint32_t b = nbegin; b < nend; ++b) {
                        fn(grid.entries[a].index, grid.entries[b].index);
                    }
                }
            }
        }
    }
}

} // namespace blob::sim
