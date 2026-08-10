#include "config.hpp"

#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <type_traits>

namespace blob::server {

namespace {

[[nodiscard]] std::string_view trim(std::string_view s) noexcept
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.remove_suffix(1);
    }
    return s;
}

/// std::from_chars, but strict: the whole value must be consumed ("20abc" and
/// "20.5" for an int are typos, not numbers), and a float must be finite —
/// from_chars happily parses "inf"/"nan", which no config ever means.
template <typename T>
[[nodiscard]] bool parse_number(std::string_view text, T& out) noexcept
{
    const char* const last = text.data() + text.size();
    T value{};
    const auto [ptr, ec] = std::from_chars(text.data(), last, value);
    if (ec != std::errc{} || ptr != last) {
        return false;
    }
    if constexpr (std::is_floating_point_v<T>) {
        if (!std::isfinite(value)) {
            return false;
        }
    }
    out = value;
    return true;
}

/// Line each key was last set on, so a validation error can point at the line
/// that produced the final (last-wins) value. 0 = never set — and since the
/// defaults are all valid, a check can only fire for a key that was set.
struct KeyLines {
    int port{};
    int max_clients{};
    int tick_rate{};
    int world_extent{};
    int base_speed{};
    int speed_mass_exponent{};
    int radius_factor{};
    int grid_cell_size{};
};

/// Semantic bounds, run once over the final values rather than per
/// assignment — so `tick_rate = 0` overridden by a later `tick_rate = 20`
/// passes, consistent with last-wins.
void validate(const ServerConfig& config, const KeyLines& at, std::vector<ConfigError>& errors)
{
    const sim::Tuning& t = config.tuning;
    if (t.tick_rate < 1 || t.tick_rate > 255) {
        errors.push_back({at.tick_rate,
                          "tick_rate must be in [1, 255] — it crosses the wire as a u8 in "
                          "Welcome, so anything else would silently truncate"});
    }
    if (!(t.world_extent > 0.0f) || t.world_extent > 65535.0f) {
        errors.push_back({at.world_extent,
                          "world_extent must be in (0, 65535] — it crosses the wire as a "
                          "u16 in Welcome, so anything else would silently truncate"});
    }
    if (!(t.grid_cell_size > 0.0f)) {
        errors.push_back({at.grid_cell_size, "grid_cell_size must be > 0"});
    }
    if (!(t.base_speed > 0.0f)) {
        errors.push_back({at.base_speed, "base_speed must be > 0"});
    }
    if (!(t.radius_factor > 0.0f)) {
        errors.push_back({at.radius_factor, "radius_factor must be > 0"});
    }
    if (config.max_clients < 1) {
        errors.push_back({at.max_clients, "max_clients must be >= 1"});
    }
}

} // namespace

ParseResult parse_config(std::string_view text)
{
    ParseResult result{};
    KeyLines at{};

    int line_no = 0;
    for (std::size_t pos = 0; pos <= text.size();) {
        std::size_t nl = text.find('\n', pos);
        if (nl == std::string_view::npos) {
            nl = text.size();
        }
        std::string_view line = text.substr(pos, nl - pos);
        pos = nl + 1;   // past the '\n'; overshoots size() on the last line, ending the loop
        ++line_no;

        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);   // CRLF input — Notepad's default
        }
        if (const std::size_t hash = line.find('#'); hash != std::string_view::npos) {
            line = line.substr(0, hash);   // full-line and trailing comments alike
        }
        line = trim(line);
        if (line.empty()) {
            continue;
        }

        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            result.errors.push_back({line_no, "expected 'key = value'"});
            continue;
        }
        const std::string_view key   = trim(line.substr(0, eq));
        const std::string_view value = trim(line.substr(eq + 1));
        if (key.empty()) {
            result.errors.push_back({line_no, "missing key before '='"});
            continue;
        }
        if (value.empty()) {
            result.errors.push_back({line_no, "missing value after '='"});
            continue;
        }

        // Assign into a parsed-value slot per key; a malformed number leaves
        // the previous value in place and records the error instead.
        const auto malformed = [&] {
            result.errors.push_back(
                {line_no, "malformed number '" + std::string{value} + "' for key '" +
                              std::string{key} + "'"});
        };

        ServerConfig& c = result.config;
        if (key == "port") {
            if (std::uint16_t v{}; parse_number(value, v)) { c.port = v; at.port = line_no; }
            else { malformed(); }
        } else if (key == "max_clients") {
            if (std::size_t v{}; parse_number(value, v)) { c.max_clients = v; at.max_clients = line_no; }
            else { malformed(); }
        } else if (key == "tick_rate") {
            if (int v{}; parse_number(value, v)) { c.tuning.tick_rate = v; at.tick_rate = line_no; }
            else { malformed(); }
        } else if (key == "world_extent") {
            if (float v{}; parse_number(value, v)) { c.tuning.world_extent = v; at.world_extent = line_no; }
            else { malformed(); }
        } else if (key == "base_speed") {
            if (float v{}; parse_number(value, v)) { c.tuning.base_speed = v; at.base_speed = line_no; }
            else { malformed(); }
        } else if (key == "speed_mass_exponent") {
            if (float v{}; parse_number(value, v)) { c.tuning.speed_mass_exponent = v; at.speed_mass_exponent = line_no; }
            else { malformed(); }
        } else if (key == "radius_factor") {
            if (float v{}; parse_number(value, v)) { c.tuning.radius_factor = v; at.radius_factor = line_no; }
            else { malformed(); }
        } else if (key == "grid_cell_size") {
            if (float v{}; parse_number(value, v)) { c.tuning.grid_cell_size = v; at.grid_cell_size = line_no; }
            else { malformed(); }
        } else {
            // An unknown key is an error, never a skip: `base_sped = 900`
            // silently running on the default speed is a debugging trap.
            result.errors.push_back({line_no, "unknown key '" + std::string{key} + "'"});
        }
    }

    validate(result.config, at, result.errors);
    return result;
}

LoadResult load_config_file(const char* path)
{
    // "rb" so CRLF reaches parse_config untranslated — the parser owns line
    // endings, and the tests exercise exactly what a file would contain.
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        // ENOENT is the one benign failure: main treats an absent default
        // file as "run on defaults". Anything else (permissions, a directory,
        // I/O error) must surface, never be mistaken for "no config".
        return {errno == ENOENT ? LoadStatus::Absent : LoadStatus::Unreadable, {}};
    }

    std::string text;
    char buffer[4096];
    std::size_t n = 0;
    while ((n = std::fread(buffer, 1, sizeof buffer, file)) > 0) {
        text.append(buffer, n);
    }
    const bool read_error = std::ferror(file) != 0;
    std::fclose(file);
    if (read_error) {
        return {LoadStatus::Unreadable, {}};
    }
    return {LoadStatus::Loaded, parse_config(text)};
}

} // namespace blob::server
