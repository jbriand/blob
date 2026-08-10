cmake_minimum_required(VERSION 3.25)

project(blob
    VERSION 0.1.0
    DESCRIPTION "agar.io clone — authoritative server, hand-rolled UDP protocol"
    LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

# ---------------------------------------------------------------------------
# Global language settings. C++23 everywhere, no compiler extensions.
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT DEFINED CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(BLOB_BUILD_SERVER "Build the headless authoritative server" ON)
option(BLOB_BUILD_CLIENT "Build the SFML client (needs a GPU/X11 box)" ON)
option(BLOB_BUILD_TESTS  "Build core unit tests (GoogleTest)"        ON)
option(BLOB_WARNINGS_AS_ERRORS "Treat warnings as errors"            OFF)

include(blob_compiler)   # blob_set_target_defaults()
include(blob_checks)     # blob_assert_no_transitive_deps()
include(blob_externals)  # blob_bring_in_externals()

# Resolves ENet / SFML 3 / GoogleTest into imported targets, honouring the
# enabled options above. See cmake/blob_externals.cmake for the contract.
blob_bring_in_externals()

if(BLOB_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(core)

if(BLOB_BUILD_SERVER)
    add_subdirectory(server)
endif()

if(BLOB_BUILD_CLIENT)
    add_subdirectory(client)
endif()

# ---------------------------------------------------------------------------
# Structural invariant: `core` is pure simulation + protocol. If SFML, ENet or
# any other I/O dependency ever shows up in its transitive link closure, fail
# at configure time rather than discovering it in a headless deploy.
# ---------------------------------------------------------------------------
blob_assert_no_transitive_deps(blob_core
    FORBIDDEN
        enet ENet::ENet blob::enet blob::sfml blob_sfml_iface
        sfml-system sfml-window sfml-graphics sfml-audio sfml-network
        SFML::System SFML::Window SFML::Graphics SFML::Audio SFML::Network)

message(STATUS "blob ${PROJECT_VERSION} | C++${CMAKE_CXX_STANDARD} | ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  server=${BLOB_BUILD_SERVER} client=${BLOB_BUILD_CLIENT} tests=${BLOB_BUILD_TESTS}")
