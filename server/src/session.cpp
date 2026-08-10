#include "session.hpp"

#include <algorithm>

namespace blob::server {

PlayerSession& add_session(std::vector<PlayerSession>& sessions, blob::sim::PlayerId id)
{
    return sessions.emplace_back(PlayerSession{.id = id});
}

bool remove_session(std::vector<PlayerSession>& sessions, blob::sim::PlayerId id)
{
    return std::erase_if(sessions,
                         [id](const PlayerSession& s) { return s.id == id; }) > 0;
}

PlayerSession* find_session(std::vector<PlayerSession>& sessions,
                            blob::sim::PlayerId id) noexcept
{
    const auto it = std::ranges::find(sessions, id, &PlayerSession::id);
    return it != sessions.end() ? &*it : nullptr;
}

} // namespace blob::server
