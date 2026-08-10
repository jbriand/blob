#include <blob/net/protocol.hpp>
#include <blob/net/quantize.hpp>

// The one place net and sim are allowed to meet: protocol.hpp stays standalone
// (no <blob/sim/...> include), so the numeric agreement between
// EntityRecord::kind and sim::EntityKind is enforced here, where the test
// target links both modules.
#include <blob/sim/world.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace net = blob::net;

static_assert(net::entity_kind_count ==
                  static_cast<std::uint8_t>(blob::sim::EntityKind::EjectedMass) + 1,
              "EntityRecord::kind mirrors sim::EntityKind numerically — adding a kind is a "
              "wire change: extend the mirror and bump protocol_version");

TEST(Snapshot, ProtocolVersionPinnedAtThree)
{
    // M6 batched the Hello payload and the Goodbye reasons into one bump. If
    // this fails, the wire format changed: bump consciously, then re-pin here.
    EXPECT_EQ(net::protocol_version, 3);
}

TEST(Snapshot, ChunkArithmeticFitsTheMtuBudget)
{
    // Wire-size promises, pinned with literal numbers so a drive-by field
    // addition cannot silently resize the packet.
    EXPECT_EQ(net::snapshot_header_bytes, 7u);
    EXPECT_EQ(net::entity_record_bytes, 13u);
    EXPECT_EQ(net::max_entities_per_chunk, 91u);
    EXPECT_LE(91u * 13u + 7u, net::snapshot_soft_mtu);   // a full chunk fits the budget
    EXPECT_GT(92u * 13u + 7u, net::snapshot_soft_mtu);   // one more record would not
}

TEST(Snapshot, RecordsSurviveTheWire)
{
    std::array<std::byte, 128> buffer{};
    net::ByteWriter w{buffer};

    const std::array<net::EntityRecord, 3> sent{{
        {.id = 0xDEADBEEFu, .owner = 7, .kind = 0, .x = 0x1234, .y = 0xFEDC, .mass = 4242},
        {.id = 1, .owner = 0, .kind = 3, .x = 0, .y = 65535, .mass = 0},
        {.id = 0xFFFFFFFFu, .owner = 65535, .kind = 2, .x = 32768, .y = 1, .mass = 65535},
    }};
    net::write_snapshot(w, 0xA1B2C3D4u, sent);
    ASSERT_FALSE(w.overflowed);
    EXPECT_EQ(w.offset, net::snapshot_header_bytes + 3u * net::entity_record_bytes);

    net::ByteReader r{net::written(w)};
    std::array<net::EntityRecord, 8> out{};
    const auto header = net::read_snapshot(r, out);
    ASSERT_TRUE(header.has_value());
    EXPECT_EQ(header->tick, 0xA1B2C3D4u);
    EXPECT_EQ(header->count, 3u);
    for (std::size_t i = 0; i < sent.size(); ++i) {
        EXPECT_EQ(out[i].id, sent[i].id) << "record " << i;
        EXPECT_EQ(out[i].owner, sent[i].owner) << "record " << i;
        EXPECT_EQ(out[i].kind, sent[i].kind) << "record " << i;
        EXPECT_EQ(out[i].x, sent[i].x) << "record " << i;
        EXPECT_EQ(out[i].y, sent[i].y) << "record " << i;
        EXPECT_EQ(out[i].mass, sent[i].mass) << "record " << i;
    }
    EXPECT_TRUE(net::exhausted(r));
    EXPECT_FALSE(r.underflowed);
}

TEST(Snapshot, QuantizedFieldsStayWithinErrorBounds)
{
    // End-to-end with the real quantizers: float world state -> wire -> float,
    // landing within the error the packing promises (round-to-nearest, so half
    // a position step and half a mass unit).
    constexpr float extent = 8192.0f;
    constexpr float half_step = 0.5f * extent / 65535.0f;

    struct Source {
        float x, y, mass;
    };
    const std::array<Source, 3> sources{{
        {0.0f, extent, 1.0f},
        {123.456f, 4096.0f, 250.75f},
        {8000.25f, 0.125f, 65534.5f},
    }};

    std::array<net::EntityRecord, 3> sent{};
    for (std::size_t i = 0; i < sources.size(); ++i) {
        sent[i] = net::EntityRecord{
            .id = static_cast<std::uint32_t>(i + 1),
            .owner = 1,
            .kind = 1,
            .x = net::quantize_position(sources[i].x, extent),
            .y = net::quantize_position(sources[i].y, extent),
            .mass = net::quantize_mass(sources[i].mass),
        };
    }

    std::array<std::byte, 128> buffer{};
    net::ByteWriter w{buffer};
    net::write_snapshot(w, 1u, sent);
    ASSERT_FALSE(w.overflowed);

    net::ByteReader r{net::written(w)};
    std::array<net::EntityRecord, 3> out{};
    const auto header = net::read_snapshot(r, out);
    ASSERT_TRUE(header.has_value());
    ASSERT_EQ(header->count, 3u);

    for (std::size_t i = 0; i < sources.size(); ++i) {
        EXPECT_NEAR(net::dequantize_position(out[i].x, extent), sources[i].x, half_step)
            << "record " << i;
        EXPECT_NEAR(net::dequantize_position(out[i].y, extent), sources[i].y, half_step)
            << "record " << i;
        EXPECT_NEAR(net::dequantize_mass(out[i].mass), sources[i].mass, 0.5f) << "record " << i;
    }
}

TEST(Snapshot, EmptySnapshotIsLegalAndExactlySevenBytes)
{
    // "Nothing visible" is a valid world state and must not need a special
    // case anywhere — and an empty out span is enough to receive it.
    std::array<std::byte, 16> buffer{};
    net::ByteWriter w{buffer};
    net::write_snapshot(w, 77u, {});
    ASSERT_FALSE(w.overflowed);
    EXPECT_EQ(w.offset, net::snapshot_header_bytes);

    net::ByteReader r{net::written(w)};
    const auto header = net::read_snapshot(r, std::span<net::EntityRecord>{});
    ASSERT_TRUE(header.has_value());
    EXPECT_EQ(header->tick, 77u);
    EXPECT_EQ(header->count, 0u);
    EXPECT_TRUE(net::exhausted(r));
}

TEST(Snapshot, EveryTruncationIsRejectedNotGuessed)
{
    // The unreliable channel can hand the reader any prefix of a datagram;
    // all 46 proper prefixes of a 3-record snapshot must be rejected, whether
    // they cut the header, a record boundary, or mid-record.
    std::array<std::byte, 64> buffer{};
    net::ByteWriter w{buffer};
    const std::array<net::EntityRecord, 3> sent{{
        {.id = 10, .owner = 1, .kind = 1, .x = 100, .y = 200, .mass = 300},
        {.id = 11, .owner = 2, .kind = 2, .x = 400, .y = 500, .mass = 600},
        {.id = 12, .owner = 3, .kind = 3, .x = 700, .y = 800, .mass = 900},
    }};
    net::write_snapshot(w, 9u, sent);
    ASSERT_FALSE(w.overflowed);
    const auto full = net::written(w);
    ASSERT_EQ(full.size(), 46u);

    for (std::size_t len = 0; len < full.size(); ++len) {
        net::ByteReader r{full.first(len)};
        std::array<net::EntityRecord, 3> out{};
        EXPECT_FALSE(net::read_snapshot(r, out).has_value()) << "length " << len;
    }
}

TEST(Snapshot, WrongMessageIdIsRejected)
{
    std::array<std::byte, 16> buffer{};
    net::ByteWriter w{buffer};
    net::write_snapshot(w, 5u, {});
    ASSERT_FALSE(w.overflowed);
    buffer[0] = static_cast<std::byte>(net::MessageId::Welcome);

    net::ByteReader r{net::written(w)};
    EXPECT_FALSE(net::read_snapshot(r, std::span<net::EntityRecord>{}).has_value());
}

TEST(Snapshot, OutOfRangeKindIsRejected)
{
    // kind is the one field with unused encodings, so it is the one field a
    // corrupt or hostile packet can use to smuggle garbage past the decoder.
    const std::array<net::EntityRecord, 1> sent{{
        {.id = 1, .owner = 0, .kind = 0, .x = 1, .y = 2, .mass = 3},
    }};

    for (const std::uint8_t bad : {net::entity_kind_count, std::uint8_t{0xFF}}) {
        std::array<std::byte, 32> buffer{};
        net::ByteWriter w{buffer};
        net::write_snapshot(w, 1u, sent);
        ASSERT_FALSE(w.overflowed);
        // kind sits 6 bytes into the record (after id u32 + owner u16), and
        // the first record starts right after the header.
        buffer[net::snapshot_header_bytes + 6] = static_cast<std::byte>(bad);

        net::ByteReader r{net::written(w)};
        std::array<net::EntityRecord, 1> out{};
        EXPECT_FALSE(net::read_snapshot(r, out).has_value())
            << "kind " << static_cast<int>(bad);
    }
}

TEST(Snapshot, CountLyingBeyondTheBytesIsRejected)
{
    std::array<std::byte, 64> buffer{};
    net::ByteWriter w{buffer};
    const std::array<net::EntityRecord, 2> sent{{
        {.id = 1, .owner = 1, .kind = 1, .x = 10, .y = 20, .mass = 30},
        {.id = 2, .owner = 2, .kind = 2, .x = 40, .y = 50, .mass = 60},
    }};
    net::write_snapshot(w, 3u, sent);
    ASSERT_FALSE(w.overflowed);

    // count sits at bytes 5..6, little endian: claim a third record that the
    // buffer does not contain.
    buffer[5] = std::byte{3};
    buffer[6] = std::byte{0};

    net::ByteReader r{net::written(w)};
    std::array<net::EntityRecord, 4> out{};
    EXPECT_FALSE(net::read_snapshot(r, out).has_value());
}

TEST(Snapshot, CountAboveChunkLimitIsRejected)
{
    // Hand-assemble what write_snapshot refuses to produce: a header claiming
    // 92 records with all 92 genuinely present. The out span is big enough,
    // so the chunk limit itself is the only thing this packet violates.
    std::array<std::byte, 2048> buffer{};
    net::ByteWriter w{buffer};
    net::write_u8(w, static_cast<std::uint8_t>(net::MessageId::Snapshot));
    net::write_u32(w, 1u);
    net::write_u16(w, std::uint16_t{92});
    for (std::uint32_t i = 0; i < 92; ++i) {
        net::write_entity_record(w, net::EntityRecord{.id = i, .kind = 1});
    }
    ASSERT_FALSE(w.overflowed);

    net::ByteReader r{net::written(w)};
    std::array<net::EntityRecord, 128> out{};
    EXPECT_FALSE(net::read_snapshot(r, out).has_value());
}

TEST(Snapshot, UndersizedOutSpanIsRejected)
{
    // A well-formed packet the receiver has no room for is the receiver's
    // problem — reject, never spill past the span.
    std::array<std::byte, 64> buffer{};
    net::ByteWriter w{buffer};
    const std::array<net::EntityRecord, 3> sent{{
        {.id = 1, .owner = 1, .kind = 1, .x = 1, .y = 1, .mass = 1},
        {.id = 2, .owner = 2, .kind = 2, .x = 2, .y = 2, .mass = 2},
        {.id = 3, .owner = 3, .kind = 3, .x = 3, .y = 3, .mass = 3},
    }};
    net::write_snapshot(w, 8u, sent);
    ASSERT_FALSE(w.overflowed);

    net::ByteReader r{net::written(w)};
    std::array<net::EntityRecord, 2> out{};
    EXPECT_FALSE(net::read_snapshot(r, out).has_value());
}

TEST(Snapshot, OversizedSpanIsFlaggedMisuseNotAScribble)
{
    // One record over the chunk limit, plenty of buffer: the failure is the
    // caller's chunking, so the flag goes up and not a single byte lands.
    const std::array<net::EntityRecord, 92> records{};
    std::array<std::byte, 2048> buffer{};
    net::ByteWriter w{buffer};
    net::write_snapshot(w, 1u, records);
    EXPECT_TRUE(w.overflowed);
    EXPECT_EQ(w.offset, 0u);
}

TEST(Snapshot, WriterOverflowSetsFlagInsteadOfScribbling)
{
    // Header fits, the record does not: the writer lands what fits, raises
    // the flag, and never runs past the buffer.
    std::array<std::byte, 10> tiny{};
    net::ByteWriter w{tiny};
    const std::array<net::EntityRecord, 1> sent{{
        {.id = 1, .owner = 1, .kind = 1, .x = 1, .y = 1, .mass = 1},
    }};
    net::write_snapshot(w, 1u, sent);
    EXPECT_TRUE(w.overflowed);
    EXPECT_LE(w.offset, tiny.size());
}

// --- Hello (M6) -------------------------------------------------------------

TEST(Hello, RoundTripsAtEveryInterestingNameLength)
{
    // 0 (anonymous default), 7 (typical), 16 (exactly the cap) — the cap
    // itself must round-trip, not just lengths below it.
    const std::string_view names[] = {"", "player7", "sixteen-bytes-xy"};
    for (const std::string_view name : names) {
        std::array<std::byte, net::hello_header_bytes + net::max_hello_name_bytes> buffer{};
        net::ByteWriter w{buffer};
        net::write_hello(w, net::protocol_version, name);
        ASSERT_FALSE(w.overflowed) << "name '" << name << "'";
        EXPECT_EQ(w.offset, net::hello_header_bytes + name.size());

        net::ByteReader r{net::written(w)};
        const std::optional<net::HelloPayload> hello = net::read_hello(r);
        ASSERT_TRUE(hello.has_value()) << "name '" << name << "'";
        EXPECT_EQ(hello->version, net::protocol_version);
        ASSERT_EQ(hello->name_len, name.size());
        EXPECT_EQ(std::string_view(hello->name.data(), hello->name_len), name);
        EXPECT_TRUE(net::exhausted(r));
        EXPECT_FALSE(r.underflowed);
    }
}

TEST(Hello, NameLengthAboveTheCapIsRejectedEvenWithTheBytesPresent)
{
    // Hand-assemble what write_hello refuses to produce: name_len 17 with all
    // 17 bytes genuinely there. The length cap itself is the violation.
    std::array<std::byte, 64> buffer{};
    net::ByteWriter w{buffer};
    net::write_u8(w, static_cast<std::uint8_t>(net::MessageId::Hello));
    net::write_u16(w, net::protocol_version);
    net::write_u8(w, std::uint8_t{17});
    for (int i = 0; i < 17; ++i) {
        net::write_u8(w, std::uint8_t{'a'});
    }
    ASSERT_FALSE(w.overflowed);

    net::ByteReader r{net::written(w)};
    EXPECT_FALSE(net::read_hello(r).has_value());
}

TEST(Hello, EveryTruncationIsRejectedNotGuessed)
{
    // Every proper prefix of an 11-byte Hello (7-byte name): cutting the
    // fixed prefix and cutting into the claimed name bytes must both fail.
    std::array<std::byte, 32> buffer{};
    net::ByteWriter w{buffer};
    net::write_hello(w, net::protocol_version, "player7");
    ASSERT_FALSE(w.overflowed);
    const auto full = net::written(w);
    ASSERT_EQ(full.size(), net::hello_header_bytes + 7u);

    for (std::size_t len = 0; len < full.size(); ++len) {
        net::ByteReader r{full.first(len)};
        EXPECT_FALSE(net::read_hello(r).has_value()) << "length " << len;
    }
}

TEST(Hello, WrongMessageIdIsRejected)
{
    std::array<std::byte, 32> buffer{};
    net::ByteWriter w{buffer};
    net::write_hello(w, net::protocol_version, "player7");
    ASSERT_FALSE(w.overflowed);
    buffer[0] = static_cast<std::byte>(net::MessageId::Input);

    net::ByteReader r{net::written(w)};
    EXPECT_FALSE(net::read_hello(r).has_value());
}

TEST(Hello, OversizedNameIsFlaggedMisuseNotTruncated)
{
    // One byte over the cap, plenty of buffer: the failure is the caller's
    // (truncating is a UI decision, never the codec's), so the flag goes up
    // and not a single byte lands — mirroring write_snapshot's span rule.
    std::array<std::byte, 64> buffer{};
    net::ByteWriter w{buffer};
    net::write_hello(w, net::protocol_version, "seventeen-bytes-x");
    EXPECT_TRUE(w.overflowed);
    EXPECT_EQ(w.offset, 0u);
}

// --- Goodbye (M6) -----------------------------------------------------------

TEST(Goodbye, RoundTripsEveryKnownReason)
{
    for (const net::GoodbyeReason reason :
         {net::GoodbyeReason::VersionMismatch, net::GoodbyeReason::ServerFull,
          net::GoodbyeReason::Shutdown}) {
        std::array<std::byte, net::goodbye_bytes> buffer{};
        net::ByteWriter w{buffer};
        net::write_goodbye(w, reason);
        ASSERT_FALSE(w.overflowed);
        EXPECT_EQ(w.offset, net::goodbye_bytes);

        net::ByteReader r{net::written(w)};
        const std::optional<net::GoodbyeReason> read = net::read_goodbye(r);
        ASSERT_TRUE(read.has_value());
        EXPECT_EQ(*read, reason);
        EXPECT_TRUE(net::exhausted(r));
    }
}

TEST(Goodbye, UnknownReasonIsRejected)
{
    // 0 is deliberately unused (a zeroed buffer must not decode), 4 is the
    // first value past the enum, 0xFF is arbitrary garbage.
    for (const std::uint8_t bad : {std::uint8_t{0}, std::uint8_t{4}, std::uint8_t{0xFF}}) {
        const std::array<std::byte, 2> raw{
            static_cast<std::byte>(net::MessageId::Goodbye), static_cast<std::byte>(bad)};
        net::ByteReader r{raw};
        EXPECT_FALSE(net::read_goodbye(r).has_value()) << "reason " << static_cast<int>(bad);
    }
}

TEST(Goodbye, TruncationAndWrongMessageIdAreRejected)
{
    std::array<std::byte, net::goodbye_bytes> buffer{};
    net::ByteWriter w{buffer};
    net::write_goodbye(w, net::GoodbyeReason::Shutdown);
    ASSERT_FALSE(w.overflowed);
    const auto full = net::written(w);

    for (std::size_t len = 0; len < full.size(); ++len) {
        net::ByteReader r{full.first(len)};
        EXPECT_FALSE(net::read_goodbye(r).has_value()) << "length " << len;
    }

    buffer[0] = static_cast<std::byte>(net::MessageId::Welcome);
    net::ByteReader r{net::written(w)};
    EXPECT_FALSE(net::read_goodbye(r).has_value());
}
