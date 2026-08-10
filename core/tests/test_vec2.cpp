#include <blob/math/vec2.hpp>

#include <gtest/gtest.h>

using blob::math::Vec2;

TEST(Vec2, ArithmeticIsConstexpr)
{
    constexpr Vec2 a{1.0f, 2.0f};
    constexpr Vec2 b{3.0f, 4.0f};
    static_assert(a + b == Vec2{4.0f, 6.0f});
    static_assert(b - a == Vec2{2.0f, 2.0f});
    static_assert(a * 2.0f == Vec2{2.0f, 4.0f});
    SUCCEED();
}

TEST(Vec2, CompoundAssignmentMatchesTheBinaryForms)
{
    // These no longer delegate to the binary operators the way the member
    // versions did, so they are worth pinning down independently.
    Vec2 v{1.0f, 2.0f};
    v += Vec2{3.0f, 4.0f};
    EXPECT_EQ(v, (Vec2{4.0f, 6.0f}));
    v -= Vec2{1.0f, 1.0f};
    EXPECT_EQ(v, (Vec2{3.0f, 5.0f}));
    v *= 2.0f;
    EXPECT_EQ(v, (Vec2{6.0f, 10.0f}));
}

TEST(Vec2, NegationAndScalarOrderAndInequality)
{
    constexpr Vec2 v{1.0f, -2.0f};
    static_assert(-v == Vec2{-1.0f, 2.0f});
    static_assert(2.0f * v == v * 2.0f);
    EXPECT_NE(v, (Vec2{1.0f, 2.0f}));   // != is synthesised from operator==
}

TEST(Vec2, LengthOfThreeFourIsFive)
{
    EXPECT_FLOAT_EQ(blob::math::length(Vec2{3.0f, 4.0f}), 5.0f);
}

TEST(Vec2, NormalizeOfZeroIsZeroNotNaN)
{
    const Vec2 n = blob::math::normalized(Vec2{0.0f, 0.0f});
    EXPECT_EQ(n, (Vec2{0.0f, 0.0f}));
}

TEST(Vec2, NormalizeProducesUnitLength)
{
    EXPECT_NEAR(blob::math::length(blob::math::normalized(Vec2{7.0f, -3.0f})), 1.0f, 1e-5f);
}
