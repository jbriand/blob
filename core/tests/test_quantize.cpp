#include <blob/net/protocol.hpp>
#include <blob/net/quantize.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace net = blob::net;

TEST(Quantize, PositionRoundTripStaysUnderHalfAStep)
{
    constexpr float extent = 8192.0f;
    constexpr float step = extent / 65535.0f;

    for (float v : {0.0f, 1.0f, 123.456f, 4096.0f, extent}) {
        const float back = net::dequantize_position(net::quantize_position(v, extent), extent);
        EXPECT_NEAR(back, v, 0.5f * step) << "value " << v;   // round-to-nearest: half a step
    }
}

TEST(Quantize, DirectionRoundTripPreservesSign)
{
    EXPECT_NEAR(net::dequantize_direction(net::quantize_direction(1.0f)), 1.0f, 1e-2f);
    EXPECT_NEAR(net::dequantize_direction(net::quantize_direction(-1.0f)), -1.0f, 1e-2f);
    EXPECT_EQ(net::quantize_direction(0.0f), 0);
}

TEST(Quantize, ValuesAreClampedNotWrapped)
{
    EXPECT_EQ(net::quantize_unorm16(2.0f), 65535);
    EXPECT_EQ(net::quantize_unorm16(-1.0f), 0);
    EXPECT_EQ(net::quantize_direction(5.0f), 127);
    EXPECT_EQ(net::quantize_direction(-5.0f), -127);
}

TEST(Protocol, InputCommandSurvivesTheWire)
{
    std::array<std::byte, 32> buffer{};
    net::ByteWriter w{buffer};

    const net::InputCommand sent{
        .sequence = 4242, .dir_x = -100, .dir_y = 77, .split = true, .eject = false};
    net::write_input(w, sent);
    ASSERT_FALSE(w.overflowed);
    EXPECT_EQ(w.offset, 6u);

    net::ByteReader r{net::written(w)};
    const auto got = net::read_input(r);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->sequence, sent.sequence);
    EXPECT_EQ(got->dir_x, sent.dir_x);
    EXPECT_EQ(got->dir_y, sent.dir_y);
    EXPECT_TRUE(got->split);
    EXPECT_FALSE(got->eject);
    EXPECT_TRUE(net::exhausted(r));
    EXPECT_EQ(net::remaining(r), 0u);
}

TEST(Protocol, WelcomePayloadSurvivesTheWire)
{
    std::array<std::byte, 32> buffer{};
    net::ByteWriter w{buffer};

    const net::WelcomePayload sent{.version = net::protocol_version,
                                   .player_id = 7,
                                   .world_extent = 8192,
                                   .tick_rate = 20};
    net::write_welcome(w, sent);
    ASSERT_FALSE(w.overflowed);
    EXPECT_EQ(w.offset, 8u);

    net::ByteReader r{net::written(w)};
    const auto got = net::read_welcome(r);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->version, sent.version);
    EXPECT_EQ(got->player_id, sent.player_id);
    EXPECT_EQ(got->world_extent, sent.world_extent);
    EXPECT_EQ(got->tick_rate, sent.tick_rate);
}

TEST(Protocol, WideWritesAreLittleEndianOnTheWire)
{
    // Nothing sends a u32 yet, but M1's snapshot header carries the tick as
    // one, and byte order is a wire-format promise rather than an
    // implementation detail — pin it before a second implementation exists.
    std::array<std::byte, 8> buffer{};
    net::ByteWriter w{buffer};
    net::write_u32(w, 0x11223344u);
    ASSERT_FALSE(w.overflowed);
    ASSERT_EQ(w.offset, 4u);
    EXPECT_EQ(buffer[0], std::byte{0x44});
    EXPECT_EQ(buffer[1], std::byte{0x33});
    EXPECT_EQ(buffer[2], std::byte{0x22});
    EXPECT_EQ(buffer[3], std::byte{0x11});

    net::ByteReader r{net::written(w)};
    EXPECT_EQ(net::read_u32(r), 0x11223344u);
    EXPECT_FALSE(r.underflowed);
}

TEST(Protocol, TruncatedPacketIsRejectedNotGuessed)
{
    std::array<std::byte, 32> buffer{};
    net::ByteWriter w{buffer};
    net::write_input(w, net::InputCommand{.sequence = 1});

    net::ByteReader r{net::written(w).first(3)};
    EXPECT_FALSE(net::read_input(r).has_value());
}

TEST(Protocol, WriterReportsOverflowInsteadOfScribbling)
{
    std::array<std::byte, 3> tiny{};
    net::ByteWriter w{tiny};
    net::write_input(w, net::InputCommand{});
    EXPECT_TRUE(w.overflowed);
    EXPECT_LE(w.offset, tiny.size());
}

TEST(Protocol, ReaderPastTheEndUnderflowsInsteadOfWrapping)
{
    // The cursor state is public now, so the accessors that would otherwise
    // turn a bad offset into undefined behaviour saturate instead.
    std::array<std::byte, 4> buffer{};
    net::ByteReader r{buffer};
    r.offset = 99;   // what a careless caller could do

    EXPECT_EQ(net::remaining(r), 0u);
    EXPECT_TRUE(net::exhausted(r));
    EXPECT_EQ(net::read_u8(r), std::uint8_t{0});
    EXPECT_TRUE(r.underflowed);
}

TEST(Protocol, MaximalOffsetCannotWrapPastTheBoundsCheck)
{
    // The nastier half of the case above: at SIZE_MAX an `offset + 1 > size`
    // guard wraps to `0 > size`, passes, and indexes out of bounds. The checks
    // are spelled `offset >= size` so this stays a flag, not a stray access.
    constexpr auto max_offset = std::numeric_limits<std::size_t>::max();
    std::array<std::byte, 4> buffer{};

    net::ByteWriter w{buffer};
    w.offset = max_offset;
    net::write_u8(w, 0xABu);
    EXPECT_TRUE(w.overflowed);
    EXPECT_EQ(w.offset, max_offset);                   // nothing was consumed
    EXPECT_LE(net::written(w).size(), buffer.size());  // and nothing escapes the buffer

    net::ByteReader r{buffer};
    r.offset = max_offset;
    EXPECT_EQ(net::read_u8(r), std::uint8_t{0});
    EXPECT_TRUE(r.underflowed);
    EXPECT_EQ(net::remaining(r), 0u);
}
