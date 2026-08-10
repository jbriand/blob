#include "tick_loop.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace server = blob::server;
using namespace std::chrono_literals;

TEST(TickLoop, MakeDerivesTheStepFromTheRate)
{
    EXPECT_EQ(server::make_tick_loop(20).step_duration, 50ms);
    EXPECT_EQ(server::make_tick_loop(50).step_duration, 20ms);
    EXPECT_EQ(server::make_tick_loop(20).max_catch_up, 5);
}

TEST(TickLoop, MakeClampsDegenerateArguments)
{
    // The clamps used to live in a constructor; they have to survive the move
    // into the factory or a zero rate divides by zero.
    EXPECT_GT(server::make_tick_loop(0).step_duration, 0ns);
    EXPECT_GT(server::make_tick_loop(-20).step_duration, 0ns);
    EXPECT_EQ(server::make_tick_loop(20, 0).max_catch_up, 1);
    EXPECT_EQ(server::make_tick_loop(20, -3).max_catch_up, 1);
}

TEST(TickLoop, DefaultConstructedLoopPumpsNothing)
{
    // TickLoop is a plain struct, so `TickLoop{}` is constructible with a zero
    // step even though make_tick_loop never produces one. That must be inert,
    // not a division by zero in the drop path.
    server::TickLoop loop{};
    EXPECT_EQ(server::pump(loop), 0);
    EXPECT_EQ(loop.ticks_run, 0u);
    EXPECT_EQ(loop.ticks_dropped, 0u);
}

TEST(TickLoop, PumpYieldsWholeTicksForElapsedTime)
{
    // Backdating `last` is the whole point of the state being public: the
    // elapsed time is controllable without injecting a clock. steady_clock is
    // monotonic, so at least 250 ms have passed by the time pump() samples it
    // — and the catch-up budget caps the answer at exactly 5 either way.
    auto loop = server::make_tick_loop(20, 5);
    loop.last = server::clock::now() - 250ms;

    EXPECT_EQ(server::pump(loop), 5);
    EXPECT_EQ(loop.ticks_run, 5u);
}

TEST(TickLoop, CatchUpClampDropsTheBacklogInsteadOfSpiralling)
{
    auto loop = server::make_tick_loop(20, 5);
    loop.last = server::clock::now() - 2s;   // 40 ticks of backlog

    EXPECT_EQ(server::pump(loop), 5);         // never more than the budget
    EXPECT_EQ(loop.ticks_run, 5u);
    EXPECT_GE(loop.ticks_dropped, 34u);       // ~35, plus anything the OS added
    EXPECT_LT(loop.accumulator, loop.step_duration);   // backlog is gone, not banked
}

TEST(TickLoop, TimeToNextTickShrinksAsTheAccumulatorFills)
{
    auto loop = server::make_tick_loop(20);   // 50 ms step
    EXPECT_EQ(server::time_to_next_tick(loop), 50ms);

    loop.accumulator = 30ms;
    EXPECT_EQ(server::time_to_next_tick(loop), 20ms);

    loop.accumulator = loop.step_duration;    // a tick is already due
    EXPECT_EQ(server::time_to_next_tick(loop), 0ms);

    loop.accumulator = 5s;                    // and it never goes negative
    EXPECT_EQ(server::time_to_next_tick(loop), 0ms);
}
