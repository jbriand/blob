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
        "grid_cell_size = 128\n");
    ASSERT_TRUE(result.errors.empty());
    expect_config_eq(result.config,
                     server::ServerConfig{
                         .port        = 9001,
                         .max_clients = 8,
                         .tuning      = {.tick_rate           = 30,
                                         .world_extent        = 4096.0f,
                                         .base_speed          = 500.5f,
                                         .speed_mass_exponent = -0.5f,
                                         .radius_factor       = 6.25f,
                                         .grid_cell_size      = 128.0f},
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
