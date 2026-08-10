#pragma once

#include <blob/math/vec2.hpp>
#include <blob/sim/world.hpp>

#include <cstdint>
#include <string>
#include <vector>

// Session bookkeeping for connected peers. Deliberately no <enet/enet.h> in
// here: the peer <-> session link is the PlayerId stashed in ENet's peer user
// pointer (main.cpp's business), so this layer stays socket-free and
// unit-tests inside blob_server_tests.

namespace blob::server {

struct PlayerSession {
    blob::sim::PlayerId id{};
    std::uint16_t       last_sequence{};    ///< highest Input sequence applied so far
    bool                received_input{};   ///< false until the first Input, when last_sequence is meaningless

    // Pre-staged for the parallel M4/M6 branches — fields land here once so
    // neither branch edits this file.
    bool pending_split{};   ///< M4: OR-latched from every Input, injected into one tick, then cleared
    bool pending_eject{};   ///< M4: same latch, eject flag
    bool received_hello{};  ///< M6: handshake state — no spawn/Welcome until the Hello arrives
    std::string      nickname;         ///< M6: from Hello (≤ 16 bytes)
    blob::math::Vec2 last_centroid{};  ///< M6: view centre for interest queries; survives brief cell-less moments
};

/// Serial-number compare on the u16 circle: is `a` newer than `b`?
/// The difference is taken mod 2^16 and read as signed, so 1 > 0, 0 > 65535
/// (wraparound), equal is never newer, and anything more than half the circle
/// "ahead" counts as older. Exactly 32768 apart is ambiguous by construction
/// and lands on "not newer" — the safe side for an input guard.
[[nodiscard]] constexpr bool sequence_newer(std::uint16_t a, std::uint16_t b) noexcept
{
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(a - b)) > 0;
}

// Linear scans are right at 64 peers — a map would cost more in constants
// than it saves in asymptotics.

PlayerSession& add_session(std::vector<PlayerSession>& sessions, blob::sim::PlayerId id);

/// Returns whether a session with that id existed.
bool remove_session(std::vector<PlayerSession>& sessions, blob::sim::PlayerId id);

/// nullptr when absent. The pointer is invalidated by add/remove — use it
/// immediately, never across an event.
[[nodiscard]] PlayerSession* find_session(std::vector<PlayerSession>& sessions,
                                          blob::sim::PlayerId id) noexcept;

} // namespace blob::server
