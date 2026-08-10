#pragma once

#include <chrono>
#include <cstdint>

namespace blob::server {

using clock = std::chrono::steady_clock;

/// Fixed-timestep driver.
///
/// The simulation must advance at exactly `tick_rate` regardless of how long a
/// tick actually took to compute, so real elapsed time goes into an
/// accumulator and comes back out in whole ticks. The clamp on `max_catch_up`
/// is the standard guard against the death spiral: if the process is stalled
/// (debugger break, GC pause in a tool, laptop lid) we drop the missed time
/// instead of trying to simulate minutes of backlog in one frame.
///
/// Build one with make_tick_loop() — it is what clamps the rate and derives
/// the step. `last` reads the clock on construction so a default-constructed
/// loop does not report the entire epoch as elapsed on its first pump().
struct TickLoop {
    std::chrono::nanoseconds step_duration{};
    std::chrono::nanoseconds accumulator{};
    clock::time_point        last{clock::now()};
    int                      max_catch_up{5};
    std::uint64_t            ticks_run{};
    std::uint64_t            ticks_dropped{};
};

[[nodiscard]] TickLoop make_tick_loop(int tick_rate, int max_catch_up = 5) noexcept;

/// Returns how many fixed steps are due since the last call.
[[nodiscard]] int pump(TickLoop& loop) noexcept;

/// How long the caller can sleep before the next tick is due.
[[nodiscard]] std::chrono::milliseconds time_to_next_tick(const TickLoop& loop) noexcept;

} // namespace blob::server
