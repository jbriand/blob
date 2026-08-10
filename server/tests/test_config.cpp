#include "config.hpp"

#include <gtest/gtest.h>

#include <string>

namespace server = blob::server;

namespace {

/// Field-by-field: Tuning is an aggregate without operator==, and spelling the
/// comparison out means a failure names the field that drifted.
void expect_config_eq(const server::ServerConfig& actual, const server::ServerConfig& expected)
{
    EXPECT_EQ(actual.port, expected.port);
    EXPECT_EQ(actual.max_clients, expected.max_clients);
    EXPECT_EQ(actual.tuning.tick_rate, expected.tuning.tick_rate);
    EXPECT_FLOAT_EQ(actual.tuning.world_extent, expected.tuning.world_extent);
    EXPECT_FLOAT_EQ(actual.tuning.base_speed, expected.tuning.base_speed);
    EXPECT_FLOAT_EQ(actual.tuning.speed_mass_exponent, expected.tuning.speed_mass_exponent);
    EXPECT_FLOAT_EQ(actual.tuning.radius_factor, expected.tuning.radius_factor);
    EXPECT_FLOAT_EQ(actual.tuning.grid_cell_size, expected.tuning.grid_cell_size);
    EXPECT_FLOAT_EQ(actual.tuning.eat_ratio, expected.tuning.eat_ratio);
    EXPECT_FLOAT_EQ(actual.tuning.eat_depth_factor, expected.tuning.eat_depth_factor);
    EXPECT_EQ(actual.tuning.target_pellet_count, expected.tuning.target_pellet_count);
    EXPECT_FLOAT_EQ(actual.tuning.pellet_mass, expected.tuning.pellet_mass);
    EXPECT_FLOAT_EQ(actual.tuning.spawn_mass, expected.tuning.spawn_mass);
    EXPECT_FLOAT_EQ(actual.tuning.decay_threshold, expected.tuning.decay_threshold);
    EXPECT_FLOAT_EQ(actual.tuning.decay_rate, expected.tuning.decay_rate);
    EXPECT_FLOAT_EQ(actual.tuning.min_split_mass, expected.tuning.min_split_mass);
    EXPECT_EQ(actual.tuning.max_cells_per_player, expected.tuning.max_cells_per_player);
    EXPECT_FLOAT_EQ(actual.tuning.split_impulse_speed, expected.tuning.split_impulse_speed);
    EXPECT_FLOAT_EQ(actual.tuning.impulse_damping_rate, expected.tuning.impulse_damping_rate);
    EXPECT_FLOAT_EQ(actual.tuning.merge_cooldown_base, expected.tuning.merge_cooldown_base);
    EXPECT_FLOAT_EQ(actual.tuning.merge_cooldown_per_mass, expected.tuning.merge_cooldown_per_mass);
    EXPECT_FLOAT_EQ(actual.tuning.merge_overlap, expected.tuning.merge_overlap);
    EXPECT_FLOAT_EQ(actual.tuning.min_eject_mass, expected.tuning.min_eject_mass);
    EXPECT_FLOAT_EQ(actual.tuning.eject_mass_cost, expected.tuning.eject_mass_cost);
    EXPECT_FLOAT_EQ(actual.tuning.ejected_mass, expected.tuning.ejected_mass);
    EXPECT_FLOAT_EQ(actual.tuning.eject_speed, expected.tuning.eject_speed);
    EXPECT_EQ(actual.tuning.target_virus_count, expected.tuning.target_virus_count);
    EXPECT_FLOAT_EQ(actual.tuning.virus_mass, expected.tuning.virus_mass);
    EXPECT_EQ(actual.tuning.virus_pop_pieces, expected.tuning.virus_pop_pieces);
    EXPECT_EQ(actual.tuning.virus_feed_count, expected.tuning.virus_feed_count);
    EXPECT_FLOAT_EQ(actual.tuning.safe_spawn_radius, expected.tuning.safe_spawn_radius);
    EXPECT_FLOAT_EQ(actual.tuning.safe_spawn_threat_mass, expected.tuning.safe_spawn_threat_mass);
    EXPECT_EQ(actual.tuning.safe_spawn_attempts, expected.tuning.safe_spawn_attempts);
    EXPECT_FLOAT_EQ(actual.tuning.view_base, expected.tuning.view_base);
    EXPECT_FLOAT_EQ(actual.tuning.view_mass_factor, expected.tuning.view_mass_factor);
    EXPECT_EQ(actual.snapshot_chunks_per_tick, expected.snapshot_chunks_per_tick);
}

} // namespace

TEST(Config, EmptyTextYieldsDefaults)
{
    const server::ParseResult result = server::parse_config("");
    EXPECT_TRUE(result.errors.empty());
    expect_config_eq(result.config, server::ServerConfig{});
}

TEST(Config, EveryKeyRoundTrips)
{
    const server::ParseResult result = server::parse_config(
        "port = 9001\n"
        "max_clients = 8\n"
        "tick_rate = 30\n"
        "world_extent = 4096\n"
        "base_speed = 500.5\n"
        "speed_mass_exponent = -0.5\n"
        "radius_factor = 6.25\n"
        "grid_cell_size = 128\n"
        "eat_ratio = 1.5\n"
        "eat_depth_factor = 0.5\n"
        "target_pellet_count = 500\n"
        "pellet_mass = 2\n"
        "spawn_mass = 15\n"
        "decay_threshold = 150\n"
        "decay_rate = 0.01\n"
        "min_split_mass = 40\n"
        "max_cells_per_player = 8\n"
        "split_impulse_speed = 800\n"
        "impulse_damping_rate = 4\n"
        "merge_cooldown_base = 12\n"
        "merge_cooldown_per_mass = 0.05\n"
        "merge_overlap = 0.3\n"
        "min_eject_mass = 40\n"
        "eject_mass_cost = 20\n"
        "ejected_mass = 15\n"
        "eject_speed = 1200\n"
        "target_virus_count = 10\n"
        "virus_mass = 120\n"
        "virus_pop_pieces = 6\n"
        "virus_feed_count = 5\n"
        "safe_spawn_radius = 500\n"
        "safe_spawn_threat_mass = 90\n"
        "safe_spawn_attempts = 8\n"
        "view_base = 700\n"
        "view_mass_factor = 30\n"
        "snapshot_chunks_per_tick = 2\n");
    ASSERT_TRUE(result.errors.empty());
    expect_config_eq(result.config,
                     server::ServerConfig{
                         .port                     = 9001,
                         .max_clients              = 8,
                         .snapshot_chunks_per_tick = 2,
                         .tuning = {.tick_rate               = 30,
                                    .world_extent            = 4096.0f,
                                    .base_speed              = 500.5f,
                                    .speed_mass_exponent     = -0.5f,
                                    .radius_factor           = 6.25f,
                                    .grid_cell_size          = 128.0f,
                                    .eat_ratio               = 1.5f,
                                    .eat_depth_factor        = 0.5f,
                                    .target_pellet_count     = 500,
                                    .pellet_mass             = 2.0f,
                                    .spawn_mass              = 15.0f,
                                    .decay_threshold         = 150.0f,
                                    .decay_rate              = 0.01f,
                                    .min_split_mass          = 40.0f,
                                    .max_cells_per_player    = 8,
                                    .split_impulse_speed     = 800.0f,
                                    .impulse_damping_rate    = 4.0f,
                                    .merge_cooldown_base     = 12.0f,
                                    .merge_cooldown_per_mass = 0.05f,
                                    .merge_overlap           = 0.3f,
                                    .min_eject_mass          = 40.0f,
                                    .eject_mass_cost         = 20.0f,
                                    .ejected_mass            = 15.0f,
                                    .eject_speed             = 1200.0f,
                                    .target_virus_count      = 10,
                                    .virus_mass              = 120.0f,
                                    .virus_pop_pieces        = 6,
                                    .virus_feed_count        = 5,
                                    .safe_spawn_radius       = 500.0f,
                                    .safe_spawn_threat_mass  = 90.0f,
                                    .safe_spawn_attempts     = 8,
                                    .view_base               = 700.0f,
                                    .view_mass_factor        = 30.0f},
                     });
}

TEST(Config, UnknownKeyErrorCarriesTheLine)
{
    // The typo'd key is the whole reason unknown keys are errors: `base_sped`
    // silently ignored would leave the server running on the default speed.
    const server::ParseResult result = server::parse_config(
        "port = 7777\n"
        "\n"
        "base_sped = 900\n");
    ASSERT_EQ(result.errors.size(), 1u);
    EXPECT_EQ(result.errors[0].line, 3);
    EXPECT_NE(result.errors[0].message.find("base_sped"), std::string::npos);
    EXPECT_EQ(result.config.port, 7777);   // the valid line still applied
}

TEST(Config, MalformedNumbersAreErrorsAndLeaveTheValueAlone)
{
    const server::ParseResult result = server::parse_config(
        "port = seven\n"          // not a number
        "tick_rate = 20.5\n"      // int key, fractional value
        "world_extent = 12abc\n"  // trailing garbage
        "base_speed = \n"         // missing value
        "grid_cell_size = inf\n"  // from_chars parses inf; a config never means it
    );
    ASSERT_EQ(result.errors.size(), 5u);
    EXPECT_EQ(result.errors[0].line, 1);
    EXPECT_EQ(result.errors[1].line, 2);
    EXPECT_EQ(result.errors[2].line, 3);
    EXPECT_EQ(result.errors[3].line, 4);
    EXPECT_EQ(result.errors[4].line, 5);
    expect_config_eq(result.config, server::ServerConfig{});   // nothing half-applied
}

TEST(Config, LineWithoutEqualsIsAnError)
{
    const server::ParseResult result = server::parse_config("port 7777\n");
    ASSERT_EQ(result.errors.size(), 1u);
    EXPECT_EQ(result.errors[0].line, 1);
}

TEST(Config, CommentsAndWhitespaceVariants)
{
    const server::ParseResult result = server::parse_config(
        "# full-line comment\n"
        "   port   =   9001   # trailing comment\n"
        "\t tick_rate\t=\t30 \n"
        "\n"
        "     \n"
        "max_clients = 2#glued comment\n");
    ASSERT_TRUE(result.errors.empty());
    EXPECT_EQ(result.config.port, 9001);
    EXPECT_EQ(result.config.tuning.tick_rate, 30);
    EXPECT_EQ(result.config.max_clients, 2u);
}

TEST(Config, CrlfInputParsesIdentically)
{
    // Notepad writes CRLF and the loader reads binary, so '\r' reaches the
    // parser. It must neither corrupt values nor shift line numbers.
    const server::ParseResult result = server::parse_config(
        "port = 9001\r\n"
        "tick_rate = 30\r\n"
        "bogus_key = 1\r\n");
    ASSERT_EQ(result.errors.size(), 1u);
    EXPECT_EQ(result.errors[0].line, 3);
    EXPECT_EQ(result.config.port, 9001);
    EXPECT_EQ(result.config.tuning.tick_rate, 30);
}

TEST(Config, LastLineWithoutNewlineStillParses)
{
    const server::ParseResult result = server::parse_config("port = 9001");
    EXPECT_TRUE(result.errors.empty());
    EXPECT_EQ(result.config.port, 9001);
}

TEST(Config, DuplicateKeyLastOneWins)
{
    const server::ParseResult result = server::parse_config(
        "port = 1111\n"
        "port = 2222\n");
    EXPECT_TRUE(result.errors.empty());
    EXPECT_EQ(result.config.port, 2222);
}

TEST(Config, DuplicateKeyValidationSeesOnlyTheFinalValue)
{
    // Last-wins extends to validation: an out-of-range value that a later
    // line overrides never existed as far as the final config is concerned.
    const server::ParseResult result = server::parse_config(
        "tick_rate = 0\n"
        "tick_rate = 30\n");
    EXPECT_TRUE(result.errors.empty());
    EXPECT_EQ(result.config.tuning.tick_rate, 30);
}

TEST(Config, TickRateWireBounds)
{
    // tick_rate crosses the wire as a u8 in Welcome — out of range would
    // silently truncate, so both edges are hard errors.
    EXPECT_EQ(server::parse_config("tick_rate = 0\n").errors.size(), 1u);
    EXPECT_EQ(server::parse_config("tick_rate = 256\n").errors.size(), 1u);
    EXPECT_TRUE(server::parse_config("tick_rate = 1\n").errors.empty());
    EXPECT_TRUE(server::parse_config("tick_rate = 255\n").errors.empty());

    const server::ParseResult rejected = server::parse_config("port = 1\ntick_rate = 300\n");
    ASSERT_EQ(rejected.errors.size(), 1u);
    EXPECT_EQ(rejected.errors[0].line, 2);   // points at the line that set it
}

TEST(Config, WorldExtentWireBounds)
{
    // world_extent crosses the wire as a u16 in Welcome.
    EXPECT_EQ(server::parse_config("world_extent = 0\n").errors.size(), 1u);
    EXPECT_EQ(server::parse_config("world_extent = -1\n").errors.size(), 1u);
    EXPECT_EQ(server::parse_config("world_extent = 65536\n").errors.size(), 1u);
    EXPECT_TRUE(server::parse_config("world_extent = 65535\n").errors.empty());
    EXPECT_TRUE(server::parse_config("world_extent = 0.5\n").errors.empty());
}

TEST(Config, PositivityBounds)
{
    EXPECT_EQ(server::parse_config("grid_cell_size = 0\n").errors.size(), 1u);
    EXPECT_EQ(server::parse_config("grid_cell_size = -8\n").errors.size(), 1u);
    EXPECT_TRUE(server::parse_config("grid_cell_size = 1\n").errors.empty());

    EXPECT_EQ(server::parse_config("base_speed = 0\n").errors.size(), 1u);
    EXPECT_EQ(server::parse_config("base_speed = -100\n").errors.size(), 1u);
    EXPECT_TRUE(server::parse_config("base_speed = 0.25\n").errors.empty());

    EXPECT_EQ(server::parse_config("radius_factor = 0\n").errors.size(), 1u);
    EXPECT_TRUE(server::parse_config("radius_factor = 0.1\n").errors.empty());

    EXPECT_EQ(server::parse_config("max_clients = 0\n").errors.size(), 1u);
    EXPECT_TRUE(server::parse_config("max_clients = 1\n").errors.empty());
}

TEST(Config, GameplayKnobBounds)
{
    // The M3 knobs: gates that would degenerate the game are rejected, and
    // the deliberate edge values are accepted.
    EXPECT_FALSE(server::parse_config("eat_ratio = 1.0\n").errors.empty());
    EXPECT_FALSE(server::parse_config("eat_ratio = 0.8\n").errors.empty());
    EXPECT_TRUE(server::parse_config("eat_ratio = 1.01\n").errors.empty());

    EXPECT_FALSE(server::parse_config("eat_depth_factor = 0\n").errors.empty());
    EXPECT_FALSE(server::parse_config("eat_depth_factor = 1.5\n").errors.empty());
    EXPECT_TRUE(server::parse_config("eat_depth_factor = 1\n").errors.empty());

    EXPECT_FALSE(server::parse_config("target_pellet_count = -1\n").errors.empty());
    EXPECT_TRUE(server::parse_config("target_pellet_count = 0\n").errors.empty());

    EXPECT_FALSE(server::parse_config("pellet_mass = 0\n").errors.empty());
    EXPECT_FALSE(server::parse_config("spawn_mass = 0\n").errors.empty());
    EXPECT_FALSE(server::parse_config("decay_threshold = -1\n").errors.empty());

    EXPECT_FALSE(server::parse_config("decay_rate = -0.1\n").errors.empty());
    EXPECT_TRUE(server::parse_config("decay_rate = 0\n").errors.empty());   // 0 = decay off
}

TEST(Config, SplitEjectVirusAndInterestBounds)
{
    // Spot checks over the M4/M5/M6 knobs — one per validation family.
    EXPECT_FALSE(server::parse_config("max_cells_per_player = 0\n").errors.empty());
    EXPECT_FALSE(server::parse_config("merge_overlap = 0\n").errors.empty());
    EXPECT_FALSE(server::parse_config("merge_overlap = 1.5\n").errors.empty());
    EXPECT_FALSE(server::parse_config("virus_pop_pieces = 1\n").errors.empty());
    EXPECT_FALSE(server::parse_config("snapshot_chunks_per_tick = 0\n").errors.empty());
    EXPECT_FALSE(server::parse_config("view_base = 0\n").errors.empty());

    // Cross-field rules: the mass ledger of eject cannot print mass, and a
    // minimum-mass cell must survive paying the cost. Each fires against the
    // *final* values, so a lone override that breaks a default pairing errs.
    EXPECT_FALSE(server::parse_config("ejected_mass = 30\neject_mass_cost = 20\n").errors.empty());
    EXPECT_FALSE(server::parse_config("eject_mass_cost = 50\n").errors.empty());   // > default min_eject_mass 35
    EXPECT_TRUE(server::parse_config("min_eject_mass = 60\neject_mass_cost = 50\nejected_mass = 45\n")
                    .errors.empty());
}

TEST(Config, EveryErrorIsReportedNotJustTheFirst)
{
    // main prints the lot and exits; a config with three problems must not
    // need three restarts to discover them all.
    const server::ParseResult result = server::parse_config(
        "prot = 7777\n"
        "tick_rate = nope\n"
        "world_extent = 100000\n");
    EXPECT_EQ(result.errors.size(), 3u);
}
