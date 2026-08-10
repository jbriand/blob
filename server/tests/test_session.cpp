#include "session.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace server = blob::server;

// constexpr means the guard is also checkable at compile time.
static_assert(server::sequence_newer(1, 0));
static_assert(!server::sequence_newer(0, 1));

TEST(Session, SequenceNewerHandlesWraparound)
{
    // Plain forward progress.
    EXPECT_TRUE(server::sequence_newer(1, 0));
    EXPECT_FALSE(server::sequence_newer(0, 1));
    EXPECT_TRUE(server::sequence_newer(1000, 999));

    // Equal is never newer — a duplicated datagram must not re-apply.
    EXPECT_FALSE(server::sequence_newer(0, 0));
    EXPECT_FALSE(server::sequence_newer(65535, 65535));

    // The reason for serial-number arithmetic at all: 0 follows 65535, so a
    // long session survives the u16 wrap without freezing the player.
    EXPECT_TRUE(server::sequence_newer(0, 65535));
    EXPECT_FALSE(server::sequence_newer(65535, 0));
    EXPECT_TRUE(server::sequence_newer(5, 65533));
    EXPECT_FALSE(server::sequence_newer(65533, 5));

    // Half-circle edges: 32767 ahead is the farthest "newer"; exactly 32768
    // apart is ambiguous by construction and lands on "not newer" both ways.
    EXPECT_TRUE(server::sequence_newer(32767, 0));
    EXPECT_FALSE(server::sequence_newer(32768, 0));
    EXPECT_FALSE(server::sequence_newer(0, 32768));
    EXPECT_TRUE(server::sequence_newer(32768, 1));
    EXPECT_TRUE(server::sequence_newer(40000, 39000));
    EXPECT_FALSE(server::sequence_newer(39000, 40000));
}

TEST(Session, AddFindRemove)
{
    std::vector<server::PlayerSession> sessions;

    const server::PlayerSession& added = server::add_session(sessions, 7);
    EXPECT_EQ(added.id, 7);
    EXPECT_EQ(added.last_sequence, 0);
    EXPECT_FALSE(added.received_input);
    server::add_session(sessions, 9);
    ASSERT_EQ(sessions.size(), 2u);

    server::PlayerSession* found = server::find_session(sessions, 7);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->id, 7);
    EXPECT_EQ(server::find_session(sessions, 8), nullptr);

    EXPECT_TRUE(server::remove_session(sessions, 7));
    EXPECT_EQ(sessions.size(), 1u);
    EXPECT_EQ(server::find_session(sessions, 7), nullptr);
    EXPECT_FALSE(server::remove_session(sessions, 7));   // already gone

    server::PlayerSession* other = server::find_session(sessions, 9);
    ASSERT_NE(other, nullptr);   // the neighbour survived the removal
    EXPECT_EQ(other->id, 9);
}
