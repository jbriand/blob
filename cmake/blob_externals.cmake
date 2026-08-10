include_guard(GLOBAL)

# ===========================================================================
# Externals resolution — FetchContent
# ===========================================================================
#
# Three dependencies, all with CMake that exports real targets, so FetchContent
# carries its weight here. Two deliberate choices below:
#
# 1. The download cache lives OUTSIDE the build trees. By default FetchContent
#    populates ${CMAKE_BINARY_DIR}/_deps, which would mean one clone of SFML per
#    preset and a re-download every time a build tree is wiped. Pointing
#    FETCHCONTENT_BASE_DIR at <source>/ext gives every preset one shared
#    checkout that survives `rm -rf build/`. It is gitignored.
#
# 2. Escape hatches are kept, so letting CMake own the checkout never means
#    losing control of it:
#
#      -DFETCHCONTENT_SOURCE_DIR_ENET=C:/cpp/enet
#          build against your own clone; no download, your edits are live
#      -DFETCHCONTENT_FULLY_DISCONNECTED=ON
#          never touch the network; assume the cache is populated
#      -DBLOB_USE_SYSTEM_PACKAGES=ON
#          try find_package() first, download only if it fails
#
# Downstream targets always link the normalized names, never the upstream ones:
#   blob::enet   blob::sfml   GTest::gtest_main
# ---------------------------------------------------------------------------

# Must be set before FetchContent is included, otherwise the module's own
# default wins.
set(FETCHCONTENT_BASE_DIR "${CMAKE_SOURCE_DIR}/ext"
    CACHE PATH "Shared download cache for vendored dependencies")

include(FetchContent)

option(BLOB_USE_SYSTEM_PACKAGES
    "Try find_package() before downloading each dependency" OFF)

# Pins. Tags rather than commit SHAs on purpose: GIT_SHALLOW cannot fetch a
# bare SHA against most servers.
set(BLOB_ENET_TAG       "v1.3.18" CACHE STRING "ENet git tag")
set(BLOB_SFML_TAG       "3.0.1"   CACHE STRING "SFML git tag")
set(BLOB_GOOGLETEST_TAG "v1.15.2" CACHE STRING "GoogleTest git tag")

function(blob_bring_in_externals)
    set(_declared "")

    # -- ENet ---------------------------------------------------------------
    # Server and client both need it; core must never see it.
    if(BLOB_BUILD_SERVER OR BLOB_BUILD_CLIENT)
        set(_enet_find "")
        if(BLOB_USE_SYSTEM_PACKAGES)
            set(_enet_find FIND_PACKAGE_ARGS NAMES unofficial-enet)
        endif()
        FetchContent_Declare(enet
            GIT_REPOSITORY https://github.com/lsalzman/enet.git
            GIT_TAG        ${BLOB_ENET_TAG}
            GIT_SHALLOW    ON
            GIT_PROGRESS   ON
            SYSTEM
            ${_enet_find})
        list(APPEND _declared enet)
    endif()

    # -- SFML 3 -------------------------------------------------------------
    # Client only. Audio and network modules are off: ENet owns the wire.
    if(BLOB_BUILD_CLIENT)
        set(SFML_BUILD_AUDIO    OFF CACHE BOOL "" FORCE)
        set(SFML_BUILD_NETWORK  OFF CACHE BOOL "" FORCE)
        set(SFML_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(SFML_BUILD_DOC      OFF CACHE BOOL "" FORCE)
        set(SFML_INSTALL_PKGCONFIG_FILES OFF CACHE BOOL "" FORCE)
        set(BUILD_SHARED_LIBS   OFF CACHE BOOL "" FORCE)

        set(_sfml_find "")
        if(BLOB_USE_SYSTEM_PACKAGES)
            set(_sfml_find FIND_PACKAGE_ARGS 3 COMPONENTS Graphics Window System)
        endif()
        FetchContent_Declare(SFML
            GIT_REPOSITORY https://github.com/SFML/SFML.git
            GIT_TAG        ${BLOB_SFML_TAG}
            GIT_SHALLOW    ON
            GIT_PROGRESS   ON
            SYSTEM
            ${_sfml_find})
        list(APPEND _declared SFML)
    endif()

    # -- GoogleTest ---------------------------------------------------------
    if(BLOB_BUILD_TESTS)
        set(gtest_force_shared_crt ON  CACHE BOOL "" FORCE)
        set(BUILD_GMOCK            OFF CACHE BOOL "" FORCE)
        set(INSTALL_GTEST          OFF CACHE BOOL "" FORCE)

        set(_gtest_find "")
        if(BLOB_USE_SYSTEM_PACKAGES)
            set(_gtest_find FIND_PACKAGE_ARGS NAMES GTest)
        endif()
        FetchContent_Declare(googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG        ${BLOB_GOOGLETEST_TAG}
            GIT_SHALLOW    ON
            GIT_PROGRESS   ON
            SYSTEM
            ${_gtest_find})
        list(APPEND _declared googletest)
    endif()

    if(_declared)
        message(STATUS "externals: ${_declared} (cache: ${FETCHCONTENT_BASE_DIR})")
        FetchContent_MakeAvailable(${_declared})
    endif()

    # -- Normalized targets -------------------------------------------------
    # One place to absorb upstream renames, and the only names server/ and
    # client/ are allowed to link.
    if(TARGET enet)
        # ENet 1.3.x still uses directory-scoped include_directories(), so its
        # target carries no usable INTERFACE include path. Patch it here rather
        # than sprinkling include dirs across server/ and client/.
        get_target_property(_enet_inc enet INTERFACE_INCLUDE_DIRECTORIES)
        if(NOT _enet_inc AND EXISTS "${enet_SOURCE_DIR}/include/enet/enet.h")
            target_include_directories(enet SYSTEM PUBLIC "${enet_SOURCE_DIR}/include")
        endif()
        if(NOT TARGET blob::enet)
            add_library(blob::enet ALIAS enet)
        endif()
    elseif(TARGET unofficial::enet::enet AND NOT TARGET blob::enet)
        add_library(blob::enet ALIAS unofficial::enet::enet)
    endif()

    # GoogleTest builds itself with warnings-as-errors, and its char8_t printer
    # overload (gtest-printers.h, guarded by __cpp_lib_char8_t so it only exists
    # from C++20 on) does an implicit char8_t -> char32_t conversion that newer
    # clang flags as -Wcharacter-conversion. Raising the standard cannot help —
    # C++23 is what enables the overload in the first place — and lowering it for
    # gtest alone would leave our C++23 test TUs declaring an overload the
    # library does not define. Scoped to the dependency; our own targets keep
    # every warning.
    foreach(_gtest_target IN ITEMS gtest gtest_main)
        if(TARGET ${_gtest_target})
            target_compile_options(${_gtest_target} PRIVATE
                $<$<CXX_COMPILER_ID:Clang>:-Wno-character-conversion>)
        endif()
    endforeach()

    if(BLOB_BUILD_CLIENT AND NOT TARGET blob::sfml)
        add_library(blob_sfml_iface INTERFACE)
        target_link_libraries(blob_sfml_iface INTERFACE
            SFML::Graphics SFML::Window SFML::System)
        add_library(blob::sfml ALIAS blob_sfml_iface)
    endif()
endfunction()
