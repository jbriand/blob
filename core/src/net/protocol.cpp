#include <blob/net/protocol.hpp>

namespace blob::net {

// --- ByteWriter ------------------------------------------------------------

// Every wider write funnels through write_u8, so the bounds check lives in
// exactly one place and a write that straddles the end still lands the bytes
// that fit before raising the flag. The check must stay in `offset >= size`
// form: `offset + 1 > size` wraps to 0 at SIZE_MAX and would let a corrupted
// offset through into an out-of-bounds write.
void write_u8(ByteWriter& w, std::uint8_t v) noexcept
{
    if (w.offset >= w.buffer.size()) {
        w.overflowed = true;
        return;
    }
    w.buffer[w.offset++] = static_cast<std::byte>(v);
}

void write_i8(ByteWriter& w, std::int8_t v) noexcept
{
    write_u8(w, static_cast<std::uint8_t>(v));
}

void write_u16(ByteWriter& w, std::uint16_t v) noexcept
{
    write_u8(w, static_cast<std::uint8_t>(v & 0xFFu));
    write_u8(w, static_cast<std::uint8_t>((v >> 8) & 0xFFu));
}

void write_u32(ByteWriter& w, std::uint32_t v) noexcept
{
    write_u16(w, static_cast<std::uint16_t>(v & 0xFFFFu));
    write_u16(w, static_cast<std::uint16_t>((v >> 16) & 0xFFFFu));
}

// --- ByteReader ------------------------------------------------------------

// Same overflow-proof form as write_u8 above, for the same reason.
std::uint8_t read_u8(ByteReader& r) noexcept
{
    if (r.offset >= r.buffer.size()) {
        r.underflowed = true;
        return 0;
    }
    return static_cast<std::uint8_t>(r.buffer[r.offset++]);
}

std::int8_t read_i8(ByteReader& r) noexcept { return static_cast<std::int8_t>(read_u8(r)); }

std::uint16_t read_u16(ByteReader& r) noexcept
{
    const auto lo = static_cast<std::uint16_t>(read_u8(r));
    const auto hi = static_cast<std::uint16_t>(read_u8(r));
    return static_cast<std::uint16_t>(lo | static_cast<std::uint16_t>(hi << 8));
}

std::uint32_t read_u32(ByteReader& r) noexcept
{
    const auto lo = static_cast<std::uint32_t>(read_u16(r));
    const auto hi = static_cast<std::uint32_t>(read_u16(r));
    return lo | (hi << 16);
}

// --- Messages --------------------------------------------------------------

namespace {
constexpr std::uint8_t flag_split = 0b0000'0001;
constexpr std::uint8_t flag_eject = 0b0000'0010;
} // namespace

void write_input(ByteWriter& w, const InputCommand& cmd) noexcept
{
    write_u8(w, static_cast<std::uint8_t>(MessageId::Input));
    write_u16(w, cmd.sequence);
    write_i8(w, cmd.dir_x);
    write_i8(w, cmd.dir_y);
    write_u8(w, static_cast<std::uint8_t>((cmd.split ? flag_split : 0u) |
                                          (cmd.eject ? flag_eject : 0u)));
}

std::optional<InputCommand> read_input(ByteReader& r) noexcept
{
    if (remaining(r) < 6) {
        return std::nullopt;
    }
    if (static_cast<MessageId>(read_u8(r)) != MessageId::Input) {
        return std::nullopt;
    }
    InputCommand cmd{};
    cmd.sequence = read_u16(r);
    cmd.dir_x = read_i8(r);
    cmd.dir_y = read_i8(r);
    const std::uint8_t flags = read_u8(r);
    cmd.split = (flags & flag_split) != 0;
    cmd.eject = (flags & flag_eject) != 0;
    return r.underflowed ? std::nullopt : std::optional{cmd};
}

void write_welcome(ByteWriter& w, const WelcomePayload& payload) noexcept
{
    write_u8(w, static_cast<std::uint8_t>(MessageId::Welcome));
    write_u16(w, payload.version);
    write_u16(w, payload.player_id);
    write_u16(w, payload.world_extent);
    write_u8(w, payload.tick_rate);
}

std::optional<WelcomePayload> read_welcome(ByteReader& r) noexcept
{
    if (remaining(r) < 8) {
        return std::nullopt;
    }
    if (static_cast<MessageId>(read_u8(r)) != MessageId::Welcome) {
        return std::nullopt;
    }
    WelcomePayload p{};
    p.version = read_u16(r);
    p.player_id = read_u16(r);
    p.world_extent = read_u16(r);
    p.tick_rate = read_u8(r);
    return r.underflowed ? std::nullopt : std::optional{p};
}

} // namespace blob::net
