#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace blob::net {

/// Bumped whenever the packing of any message changes. The check is one-way
/// today: Welcome carries the server's version and the client refuses on a
/// mismatch; symmetric refusal waits for M6's Hello payload.
inline constexpr std::uint16_t protocol_version = 2;   // 2: snapshot messages (M1)

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
// Snapshot wire format. net describes the wire, not the simulation, so these
// stay raw integers and this header includes no <blob/sim/...> header —
// quantize.hpp holds the float<->wire conversions, and the numeric mirror of
// sim::EntityKind is asserted in the tests, which link both modules.
// ---------------------------------------------------------------------------

/// One entity as it crosses the wire — see entity_record_bytes for the layout.
struct EntityRecord {
    std::uint32_t id{};
    std::uint16_t owner{};   ///< PlayerId — lets the client tell "mine" from "theirs"
    std::uint8_t  kind{};    ///< mirrors sim::EntityKind numerically
    std::uint16_t x{};       ///< quantize_position against the Welcome extent
    std::uint16_t y{};
    std::uint16_t mass{};    ///< quantize_mass
};

/// Decoded chunk header. Every chunk is self-contained — it repeats the tick
/// and carries its own count — so a chunk lost on the unreliable channel
/// degrades gracefully: records are absolute state, partial application is
/// benign.
struct SnapshotHeader {
    std::uint32_t tick{};    ///< u64 sim tick truncated — ~6.8 years at 20 Hz
    std::uint16_t count{};   ///< records in THIS chunk, not in the world
};

/// MessageId u8 + tick u32 + count u16 — the reader's upfront length check.
inline constexpr std::size_t snapshot_header_bytes = 7;

/// id u32 + owner u16 + kind u8 + x u16 + y u16 + mass u16.
inline constexpr std::size_t entity_record_bytes = 13;

/// Soft single-UDP-datagram budget: comfortably under the common 1500 B path
/// MTU with room for IP/UDP/ENet framing, so a chunk never fragments.
inline constexpr std::size_t snapshot_soft_mtu = 1200;

/// = 91. Chunking the entity list is the caller's job; write_snapshot treats
/// a larger span as misuse (flag set, nothing written).
inline constexpr std::size_t max_entities_per_chunk =
    (snapshot_soft_mtu - snapshot_header_bytes) / entity_record_bytes;

/// Mirrors sim::EntityKind so readers can reject garbage kinds without net
/// including sim. The mirror is enforced by a static_assert in the tests,
/// which link both modules; adding a kind is a wire change and bumps
/// protocol_version anyway.
inline constexpr std::uint8_t entity_kind_count = 4;

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

void write_entity_record(ByteWriter& w, const EntityRecord& record) noexcept;
/// Rejects kind >= entity_kind_count.
[[nodiscard]] std::optional<EntityRecord> read_entity_record(ByteReader& r) noexcept;

/// One self-contained chunk: Snapshot id, tick, count, then the records.
/// A span larger than max_entities_per_chunk is flagged misuse — the overflow
/// flag is set and nothing at all is written.
void write_snapshot(ByteWriter& w, std::uint32_t tick,
                    std::span<const EntityRecord> records) noexcept;

/// On success the first `count` entries of `out` are valid. Rejects a wrong
/// MessageId, a count over max_entities_per_chunk or over out.size(), any
/// out-of-range kind, and any truncation.
[[nodiscard]] std::optional<SnapshotHeader> read_snapshot(ByteReader& r,
                                                          std::span<EntityRecord> out) noexcept;

} // namespace blob::net
