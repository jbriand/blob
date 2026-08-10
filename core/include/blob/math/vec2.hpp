#pragma once

#include <cmath>

namespace blob::math {

/// Plain data. Every operation lives beside the struct as a free function, so
/// the type stays a trivial aggregate and new operations can be added without
/// reopening it.
struct Vec2 {
    float x{};
    float y{};
};

[[nodiscard]] constexpr Vec2 operator+(Vec2 a, Vec2 b) noexcept { return {a.x + b.x, a.y + b.y}; }
[[nodiscard]] constexpr Vec2 operator-(Vec2 a, Vec2 b) noexcept { return {a.x - b.x, a.y - b.y}; }
[[nodiscard]] constexpr Vec2 operator-(Vec2 v) noexcept { return {-v.x, -v.y}; }
[[nodiscard]] constexpr Vec2 operator*(Vec2 v, float s) noexcept { return {v.x * s, v.y * s}; }
[[nodiscard]] constexpr Vec2 operator*(float s, Vec2 v) noexcept { return {v.x * s, v.y * s}; }

constexpr Vec2& operator+=(Vec2& a, Vec2 b) noexcept { a = a + b; return a; }
constexpr Vec2& operator-=(Vec2& a, Vec2 b) noexcept { a = a - b; return a; }
constexpr Vec2& operator*=(Vec2& v, float s) noexcept { v = v * s; return v; }

/// Written out rather than defaulted: a defaulted operator== has to be a member
/// or a friend, and neither belongs on a plain data struct. C++20 still
/// synthesises != from this one.
[[nodiscard]] constexpr bool operator==(Vec2 a, Vec2 b) noexcept
{
    return a.x == b.x && a.y == b.y;
}

[[nodiscard]] constexpr float dot(Vec2 a, Vec2 b) noexcept { return a.x * b.x + a.y * b.y; }
[[nodiscard]] constexpr float length_sq(Vec2 v) noexcept { return dot(v, v); }
[[nodiscard]] inline float length(Vec2 v) noexcept { return std::sqrt(length_sq(v)); }

/// Zero-safe normalize: returns {0,0} for a degenerate vector rather than NaN.
/// The simulation feeds cursor deltas straight into this, and a cursor exactly
/// on top of a cell is a normal occurrence, not an error.
[[nodiscard]] inline Vec2 normalized(Vec2 v) noexcept
{
    const float len = length(v);
    return len > 1e-6f ? v * (1.0f / len) : Vec2{};
}

} // namespace blob::math
