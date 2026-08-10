#include "config.hpp"

#include <algorithm>
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
    int eat_ratio{};
    int eat_depth_factor{};
    int target_pellet_count{};
    int pellet_mass{};
    int spawn_mass{};
    int decay_threshold{};
    int decay_rate{};
    int min_split_mass{};
    int max_cells_per_player{};
    int split_impulse_speed{};
    int impulse_damping_rate{};
    int merge_cooldown_base{};
    int merge_cooldown_per_mass{};
    int merge_overlap{};
    int min_eject_mass{};
    int eject_mass_cost{};
    int ejected_mass{};
    int eject_speed{};
    int target_virus_count{};
    int virus_mass{};
    int virus_pop_pieces{};
    int virus_feed_count{};
    int safe_spawn_radius{};
    int safe_spawn_threat_mass{};
    int safe_spawn_attempts{};
    int view_base{};
    int view_mass_factor{};
    int snapshot_chunks_per_tick{};
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
    if (!(t.eat_ratio > 1.0f)) {
        errors.push_back({at.eat_ratio,
                          "eat_ratio must be > 1 — at 1 or below, equal-mass cells could "
                          "eat each other"});
    }
    if (!(t.eat_depth_factor > 0.0f) || t.eat_depth_factor > 1.0f) {
        errors.push_back({at.eat_depth_factor,
                          "eat_depth_factor must be in (0, 1] — it is a fraction of the "
                          "victim's radius"});
    }
    if (t.target_pellet_count < 0) {
        errors.push_back({at.target_pellet_count, "target_pellet_count must be >= 0"});
    }
    if (!(t.pellet_mass > 0.0f)) {
        errors.push_back({at.pellet_mass, "pellet_mass must be > 0"});
    }
    if (!(t.spawn_mass > 0.0f)) {
        errors.push_back({at.spawn_mass, "spawn_mass must be > 0"});
    }
    if (!(t.decay_threshold >= 0.0f)) {
        errors.push_back({at.decay_threshold, "decay_threshold must be >= 0"});
    }
    if (!(t.decay_rate >= 0.0f)) {
        errors.push_back({at.decay_rate, "decay_rate must be >= 0 (0 disables decay)"});
    }
    if (!(t.min_split_mass > 0.0f)) {
        errors.push_back({at.min_split_mass, "min_split_mass must be > 0"});
    }
    if (t.max_cells_per_player < 1) {
        errors.push_back({at.max_cells_per_player, "max_cells_per_player must be >= 1"});
    }
    if (!(t.split_impulse_speed >= 0.0f)) {
        errors.push_back({at.split_impulse_speed, "split_impulse_speed must be >= 0"});
    }
    if (!(t.impulse_damping_rate >= 0.0f)) {
        errors.push_back({at.impulse_damping_rate, "impulse_damping_rate must be >= 0"});
    }
    if (!(t.merge_cooldown_base >= 0.0f)) {
        errors.push_back({at.merge_cooldown_base, "merge_cooldown_base must be >= 0"});
    }
    if (!(t.merge_cooldown_per_mass >= 0.0f)) {
        errors.push_back({at.merge_cooldown_per_mass, "merge_cooldown_per_mass must be >= 0"});
    }
    if (!(t.merge_overlap > 0.0f) || t.merge_overlap > 1.0f) {
        errors.push_back({at.merge_overlap, "merge_overlap must be in (0, 1]"});
    }
    if (!(t.min_eject_mass > 0.0f)) {
        errors.push_back({at.min_eject_mass, "min_eject_mass must be > 0"});
    }
    if (!(t.eject_mass_cost > 0.0f)) {
        errors.push_back({at.eject_mass_cost, "eject_mass_cost must be > 0"});
    }
    if (!(t.ejected_mass > 0.0f)) {
        errors.push_back({at.ejected_mass, "ejected_mass must be > 0"});
    }
    if (t.ejected_mass > t.eject_mass_cost) {
        errors.push_back({std::max(at.ejected_mass, at.eject_mass_cost),
                          "ejected_mass must not exceed eject_mass_cost — ejecting must "
                          "never print mass"});
    }
    if (t.eject_mass_cost > t.min_eject_mass) {
        errors.push_back({std::max(at.eject_mass_cost, at.min_eject_mass),
                          "eject_mass_cost must not exceed min_eject_mass — a "
                          "minimum-mass cell must survive its own eject"});
    }
    if (!(t.eject_speed >= 0.0f)) {
        errors.push_back({at.eject_speed, "eject_speed must be >= 0"});
    }
    if (t.target_virus_count < 0) {
        errors.push_back({at.target_virus_count, "target_virus_count must be >= 0"});
    }
    if (!(t.virus_mass > 0.0f)) {
        errors.push_back({at.virus_mass, "virus_mass must be > 0"});
    }
    if (t.virus_pop_pieces < 2) {
        errors.push_back({at.virus_pop_pieces,
                          "virus_pop_pieces must be >= 2 — a pop that cannot split is "
                          "just a meal"});
    }
    if (t.virus_feed_count < 1) {
        errors.push_back({at.virus_feed_count, "virus_feed_count must be >= 1"});
    }
    if (!(t.safe_spawn_radius >= 0.0f)) {
        errors.push_back({at.safe_spawn_radius, "safe_spawn_radius must be >= 0"});
    }
    if (!(t.safe_spawn_threat_mass > 0.0f)) {
        errors.push_back({at.safe_spawn_threat_mass, "safe_spawn_threat_mass must be > 0"});
    }
    if (t.safe_spawn_attempts < 1) {
        errors.push_back({at.safe_spawn_attempts, "safe_spawn_attempts must be >= 1"});
    }
    if (!(t.view_base > 0.0f)) {
        errors.push_back({at.view_base, "view_base must be > 0"});
    }
    if (!(t.view_mass_factor >= 0.0f)) {
        errors.push_back({at.view_mass_factor, "view_mass_factor must be >= 0"});
    }
    if (config.snapshot_chunks_per_tick < 1) {
        errors.push_back({at.snapshot_chunks_per_tick, "snapshot_chunks_per_tick must be >= 1"});
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
        } else if (key == "eat_ratio") {
            if (float v{}; parse_number(value, v)) { c.tuning.eat_ratio = v; at.eat_ratio = line_no; }
            else { malformed(); }
        } else if (key == "eat_depth_factor") {
            if (float v{}; parse_number(value, v)) { c.tuning.eat_depth_factor = v; at.eat_depth_factor = line_no; }
            else { malformed(); }
        } else if (key == "target_pellet_count") {
            if (int v{}; parse_number(value, v)) { c.tuning.target_pellet_count = v; at.target_pellet_count = line_no; }
            else { malformed(); }
        } else if (key == "pellet_mass") {
            if (float v{}; parse_number(value, v)) { c.tuning.pellet_mass = v; at.pellet_mass = line_no; }
            else { malformed(); }
        } else if (key == "spawn_mass") {
            if (float v{}; parse_number(value, v)) { c.tuning.spawn_mass = v; at.spawn_mass = line_no; }
            else { malformed(); }
        } else if (key == "decay_threshold") {
            if (float v{}; parse_number(value, v)) { c.tuning.decay_threshold = v; at.decay_threshold = line_no; }
            else { malformed(); }
        } else if (key == "decay_rate") {
            if (float v{}; parse_number(value, v)) { c.tuning.decay_rate = v; at.decay_rate = line_no; }
            else { malformed(); }
        } else if (key == "min_split_mass") {
            if (float v{}; parse_number(value, v)) { c.tuning.min_split_mass = v; at.min_split_mass = line_no; }
            else { malformed(); }
        } else if (key == "max_cells_per_player") {
            if (int v{}; parse_number(value, v)) { c.tuning.max_cells_per_player = v; at.max_cells_per_player = line_no; }
            else { malformed(); }
        } else if (key == "split_impulse_speed") {
            if (float v{}; parse_number(value, v)) { c.tuning.split_impulse_speed = v; at.split_impulse_speed = line_no; }
            else { malformed(); }
        } else if (key == "impulse_damping_rate") {
            if (float v{}; parse_number(value, v)) { c.tuning.impulse_damping_rate = v; at.impulse_damping_rate = line_no; }
            else { malformed(); }
        } else if (key == "merge_cooldown_base") {
            if (float v{}; parse_number(value, v)) { c.tuning.merge_cooldown_base = v; at.merge_cooldown_base = line_no; }
            else { malformed(); }
        } else if (key == "merge_cooldown_per_mass") {
            if (float v{}; parse_number(value, v)) { c.tuning.merge_cooldown_per_mass = v; at.merge_cooldown_per_mass = line_no; }
            else { malformed(); }
        } else if (key == "merge_overlap") {
            if (float v{}; parse_number(value, v)) { c.tuning.merge_overlap = v; at.merge_overlap = line_no; }
            else { malformed(); }
        } else if (key == "min_eject_mass") {
            if (float v{}; parse_number(value, v)) { c.tuning.min_eject_mass = v; at.min_eject_mass = line_no; }
            else { malformed(); }
        } else if (key == "eject_mass_cost") {
            if (float v{}; parse_number(value, v)) { c.tuning.eject_mass_cost = v; at.eject_mass_cost = line_no; }
            else { malformed(); }
        } else if (key == "ejected_mass") {
            if (float v{}; parse_number(value, v)) { c.tuning.ejected_mass = v; at.ejected_mass = line_no; }
            else { malformed(); }
        } else if (key == "eject_speed") {
            if (float v{}; parse_number(value, v)) { c.tuning.eject_speed = v; at.eject_speed = line_no; }
            else { malformed(); }
        } else if (key == "target_virus_count") {
            if (int v{}; parse_number(value, v)) { c.tuning.target_virus_count = v; at.target_virus_count = line_no; }
            else { malformed(); }
        } else if (key == "virus_mass") {
            if (float v{}; parse_number(value, v)) { c.tuning.virus_mass = v; at.virus_mass = line_no; }
            else { malformed(); }
        } else if (key == "virus_pop_pieces") {
            if (int v{}; parse_number(value, v)) { c.tuning.virus_pop_pieces = v; at.virus_pop_pieces = line_no; }
            else { malformed(); }
        } else if (key == "virus_feed_count") {
            if (int v{}; parse_number(value, v)) { c.tuning.virus_feed_count = v; at.virus_feed_count = line_no; }
            else { malformed(); }
        } else if (key == "safe_spawn_radius") {
            if (float v{}; parse_number(value, v)) { c.tuning.safe_spawn_radius = v; at.safe_spawn_radius = line_no; }
            else { malformed(); }
        } else if (key == "safe_spawn_threat_mass") {
            if (float v{}; parse_number(value, v)) { c.tuning.safe_spawn_threat_mass = v; at.safe_spawn_threat_mass = line_no; }
            else { malformed(); }
        } else if (key == "safe_spawn_attempts") {
            if (int v{}; parse_number(value, v)) { c.tuning.safe_spawn_attempts = v; at.safe_spawn_attempts = line_no; }
            else { malformed(); }
        } else if (key == "view_base") {
            if (float v{}; parse_number(value, v)) { c.tuning.view_base = v; at.view_base = line_no; }
            else { malformed(); }
        } else if (key == "view_mass_factor") {
            if (float v{}; parse_number(value, v)) { c.tuning.view_mass_factor = v; at.view_mass_factor = line_no; }
            else { malformed(); }
        } else if (key == "snapshot_chunks_per_tick") {
            if (int v{}; parse_number(value, v)) { c.snapshot_chunks_per_tick = v; at.snapshot_chunks_per_tick = line_no; }
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
