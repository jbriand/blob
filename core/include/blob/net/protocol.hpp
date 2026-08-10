#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace blob::net {

/// Bumped whenever the packing of any message changes. Client and server
/// exchange it in the handshake and refuse to talk across a mismatch.
inline constexpr std::uint16_t protocol_version = 1;

enum class MessageId : std::uint8_t {
    // client -> server
    Hello        = 0x01,
    Input        = 0x02,
    // server -> client
    Welcome      = 0x81,
    Snapshot     = 0x82,
    Goodbye      = 0x83,
};

/// ENet channel assignment. Reliable ordered for anything that changes session
/// state, unreliable for the 20 Hz firehose — a dropped snapshot is always
/// better than a late one.
enum class Channel : std::uint8_t {
    Control  = 0,  // reliable, ordered
    Snapshot = 1,  // unreliable
    Count    = 2,
};

/// Sent every client tick. The client never sends a position — only intent.
struct InputCommand {
    std::uint16_t sequence{};   ///< for the server's last-acked bookkeeping
    std::int8_t   dir_x{};      ///< quantized unit vector toward the cursor
    std::int8_t   dir_y{};
    bool          split{};
    bool          eject{};
};

struct WelcomePayload {
    std::uint16_t version{protocol_version};
    std::uint16_t player_id{};
    std::uint16_t world_extent{};   ///< square world side, in world units
    std::uint8_t  tick_rate{};      ///< server ticks per second
};

// ---------------------------------------------------------------------------
// Byte cursors. Deliberately thin: no allocation, no exceptions, no streams.
// Both sides use these so there is exactly one definition of the wire format.
//
// `offset` and the overflow/underflow flags are maintained exclusively by the
// write_/read_ functions below — that is what keeps invariant 7 (codecs never
// throw and never scribble) true. With the state public that is a convention
// rather than something the type system forbids, so nothing here may assume a
// sane offset: bounds checks are spelled `offset >= size` and never
// `offset + 1 > size`, which wraps to zero at SIZE_MAX and would sail straight
// through into an out-of-bounds access; the accessors saturate for the same
// reason. A corrupted offset yields a wrong-but-defined answer and a flag,
// never a stray read or write.
// ---------------------------------------------------------------------------

struct ByteWriter {
    std::span<std::byte> buffer{};
    std::size_t          offset{};
    bool                 overflowed{};
};

void write_u8(ByteWriter& w, std::uint8_t v) noexcept;
void write_u16(ByteWriter& w, std::uint16_t v) noexcept;   ///< little endian on the wire
void write_u32(ByteWriter& w, std::uint32_t v) noexcept;
void write_i8(ByteWriter& w, std::int8_t v) noexcept;

[[nodiscard]] inline std::span<const std::byte> written(const ByteWriter& w) noexcept
{
    return w.buffer.first(w.offset < w.buffer.size() ? w.offset : w.buffer.size());
}

struct ByteReader {
    std::span<const std::byte> buffer{};
    std::size_t                offset{};
    bool                       underflowed{};
};

[[nodiscard]] std::uint8_t  read_u8(ByteReader& r) noexcept;
[[nodiscard]] std::uint16_t read_u16(ByteReader& r) noexcept;
[[nodiscard]] std::uint32_t read_u32(ByteReader& r) noexcept;
[[nodiscard]] std::int8_t   read_i8(ByteReader& r) noexcept;

[[nodiscard]] inline bool exhausted(const ByteReader& r) noexcept
{
    return r.offset >= r.buffer.size();
}

[[nodiscard]] inline std::size_t remaining(const ByteReader& r) noexcept
{
    return r.offset < r.buffer.size() ? r.buffer.size() - r.offset : 0;
}

void write_input(ByteWriter& w, const InputCommand& cmd) noexcept;
[[nodiscard]] std::optional<InputCommand> read_input(ByteReader& r) noexcept;

void write_welcome(ByteWriter& w, const WelcomePayload& payload) noexcept;
[[nodiscard]] std::optional<WelcomePayload> read_welcome(ByteReader& r) noexcept;

} // namespace blob::net
