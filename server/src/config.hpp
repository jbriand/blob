#pragma once

#include <blob/sim/tuning.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace blob::server {

/// Everything the server reads at startup. Tuning overrides live in the same
/// file as host settings so a game-server box has exactly one thing to edit;
/// parsing stays here in server/ because core is I/O-free by invariant 1.
struct ServerConfig {
    std::uint16_t     port        = 7777;
    std::size_t       max_clients = 64;
    /// M6: per-peer snapshot budget, in chunks per tick — operational (like
    /// port), not gameplay, so it lives here rather than in Tuning.
    int               snapshot_chunks_per_tick = 3;
    blob::sim::Tuning tuning{};
};

/// One diagnostic, tied to the config line that caused it (1-based; parse
/// errors always carry a line, and validation errors point at the line that
/// last set the offending key).
struct ConfigError {
    int         line{};
    std::string message;
};

/// parse_config never fails "halfway": config always holds defaults plus
/// whatever parsed cleanly, and errors holds every problem found — so main
/// can print them all in one go instead of one per restart.
struct ParseResult {
    ServerConfig             config{};
    std::vector<ConfigError> errors;
};

/// Pure text -> config. Flat `key = value` lines named exactly after the
/// ServerConfig/Tuning fields, `#` comments (full-line or trailing), blank
/// lines and stray whitespace fine, CRLF tolerated (files edited in Notepad).
/// Numbers go through std::from_chars only — no locale surprises, no
/// exceptions. Duplicate key: last one wins (INI convention). Unknown key is
/// an error, not a skip — a silently ignored typo like `base_sped` is a
/// debugging trap.
[[nodiscard]] ParseResult parse_config(std::string_view text);

/// "Absent" and "Unreadable" are different situations for main: a missing
/// default-path file means "run on defaults", while a file that exists but
/// cannot be read must never be silently ignored.
enum class LoadStatus : std::uint8_t { Loaded, Absent, Unreadable };

struct LoadResult {
    LoadStatus  status{LoadStatus::Absent};
    ParseResult parsed{};   ///< meaningful only when status == Loaded
};

/// Thin fopen/read shell around parse_config — the one place config I/O lives.
[[nodiscard]] LoadResult load_config_file(const char* path);

} // namespace blob::server
