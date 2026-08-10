#pragma once

#include <algorithm>
#include <cstdint>

namespace blob::net {

/// Bandwidth is the scaling limit of this genre, so every field that crosses
/// the wire is quantized. These helpers are the only place the packing rules
/// live, and they are shared verbatim by client and server.

[[nodiscard]] constexpr std::uint16_t quantize_unorm16(float v) noexcept
{
    const float clamped = std::clamp(v, 0.0f, 1.0f);
    return static_cast<std::uint16_t>(clamped * 65535.0f + 0.5f);
}

[[nodiscard]] constexpr float dequantize_unorm16(std::uint16_t q) noexcept
{
    return static_cast<float>(q) / 65535.0f;
}

/// World coordinates -> 16 bits, given a square world of `extent` units.
/// At extent = 8192 that is one step per ~0.125 units: far below the radius of
/// the smallest cell, so the quantization error is invisible after
/// interpolation.
[[nodiscard]] constexpr std::uint16_t quantize_position(float v, float extent) noexcept
{
    return quantize_unorm16(v / extent);
}

[[nodiscard]] constexpr float dequantize_position(std::uint16_t q, float extent) noexcept
{
    return dequantize_unorm16(q) * extent;
}

/// Mass crosses the wire as a whole number of units, saturating at 65535 —
/// linear on purpose. Wire mass is display-only (the server keeps the
/// authoritative float), and 1-unit steps keep the small end — HUD score,
/// pellets — exact where a byte-ranged sqrt would step visibly. If bytes get
/// tight: per-kind records first, sqrt packing (uniform radius resolution,
/// r = k*sqrt(m)) second — either is a protocol_version bump. See README
/// "Quantization".
[[nodiscard]] constexpr std::uint16_t quantize_mass(float mass) noexcept
{
    const float clamped = std::clamp(mass, 0.0f, 65535.0f);
    return static_cast<std::uint16_t>(clamped + 0.5f);
}

[[nodiscard]] constexpr float dequantize_mass(std::uint16_t q) noexcept
{
    return static_cast<float>(q);
}

/// Cursor direction is a unit vector; 8 bits per axis is plenty since the
/// server clamps speed anyway.
[[nodiscard]] constexpr std::int8_t quantize_direction(float v) noexcept
{
    const float clamped = std::clamp(v, -1.0f, 1.0f);
    return static_cast<std::int8_t>(clamped * 127.0f + (v < 0.0f ? -0.5f : 0.5f));
}

[[nodiscard]] constexpr float dequantize_direction(std::int8_t q) noexcept
{
    return std::clamp(static_cast<float>(q) / 127.0f, -1.0f, 1.0f);
}

} // namespace blob::net
