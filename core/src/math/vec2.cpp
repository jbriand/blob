#include <blob/math/vec2.hpp>

#include <type_traits>

namespace blob::math {

// Vec2 is header-only by design. This TU exists so the invariants the rest of
// the simulation relies on are checked once, at build time, in core.
static_assert(sizeof(Vec2) == 8, "Vec2 must stay tightly packed");

// The plain-struct rule, made load-bearing rather than aspirational: no member
// functions, so aggregate initialisation and bytewise handling both stay valid.
static_assert(std::is_aggregate_v<Vec2>);
static_assert(std::is_trivially_copyable_v<Vec2>);

static_assert(Vec2{1.0f, 2.0f} + Vec2{3.0f, 4.0f} == Vec2{4.0f, 6.0f});
static_assert(dot(Vec2{1.0f, 0.0f}, Vec2{0.0f, 1.0f}) == 0.0f);
static_assert(length_sq(Vec2{3.0f, 4.0f}) == 25.0f);

} // namespace blob::math
