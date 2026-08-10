#include "tick_loop.hpp"

#include <algorithm>

namespace blob::server {

using namespace std::chrono;

TickLoop make_tick_loop(int tick_rate, int max_catch_up) noexcept
{
    // Both clamps matter: a zero or negative rate would divide by zero here,
    // and a zero catch-up budget would stall the simulation forever.
    return TickLoop{
        .step_duration = duration_cast<nanoseconds>(seconds{1}) / std::max(tick_rate, 1),
        .max_catch_up  = std::max(max_catch_up, 1),
    };
}

int pump(TickLoop& loop) noexcept
{
    // make_tick_loop never yields a zero step, but TickLoop is a plain struct
    // and `TickLoop{}` is therefore constructible — treat that as "no ticks
    // due" rather than dividing by zero in the drop path below.
    if (loop.step_duration <= nanoseconds::zero()) {
        return 0;
    }

    const clock::time_point now = clock::now();
    loop.accumulator += duration_cast<nanoseconds>(now - loop.last);
    loop.last = now;

    int steps = 0;
    while (loop.accumulator >= loop.step_duration && steps < loop.max_catch_up) {
        loop.accumulator -= loop.step_duration;
        ++steps;
    }

    if (loop.accumulator >= loop.step_duration) {
        // Still behind after the catch-up budget: discard the backlog.
        const auto dropped = loop.accumulator / loop.step_duration;
        loop.ticks_dropped += static_cast<std::uint64_t>(dropped);
        loop.accumulator %= loop.step_duration;
    }

    loop.ticks_run += static_cast<std::uint64_t>(steps);
    return steps;
}

milliseconds time_to_next_tick(const TickLoop& loop) noexcept
{
    const nanoseconds remaining = loop.step_duration - loop.accumulator;
    if (remaining <= nanoseconds::zero()) {
        return milliseconds::zero();
    }
    return std::max(duration_cast<milliseconds>(remaining), milliseconds::zero());
}

} // namespace blob::server
