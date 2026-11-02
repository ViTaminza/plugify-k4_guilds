# k4_guilds — in-game runtime for the site-owned clans_* database (vcs2.ru clans2 module).
#
#   k4_guilds_core       engine-free core (config, KV1 reader and file resolution, phrase tables,
#                        colour tags, DB section parsing, level derivation, kill/death accounting,
#                        clamped SQL builders, command parsing); used by
#                        tests/k4_guilds_tests.cpp
#   plugify-k4_guilds    the plugin shared library (entry "bin/k4_guilds")
#
# Standalone: no pisexcompat and no helper library (docs/PLUGIN_STANDARD.md §7). plgdb is the only
# shared library, for the asynchronous MySQL/SQLite access. The optional hudmenu plugin is a normal
# plugin dependency used only for the player's own "!menu" choice. The guards of
# handoff/weaponskins/PITFALLS.md live in src/plugin.cpp itself (map gate, C1/C2 client and handle
# checks, C8 schema caching, C9 accumulators instead of timers, C10 native menus, C11 commands,
# C12 events).

set(K4_GUILDS_VERSION "2.0.0")

# The plugin is published as a standalone repository too: the root CMakeLists.txt defines these
# targets, but a build of this directory alone must still work. Every fallback carries a
# plugin-unique target name (a generic one would collide in a combined build).
if(TARGET plugify_sdk)
    set(K4_GUILDS_PLUGIFY_SDK plugify_sdk)
else()
    add_library(k4_guilds_plugify_sdk INTERFACE)
    target_include_directories(k4_guilds_plugify_sdk INTERFACE "${CMAKE_SOURCE_DIR}/sdk/include")
    set(K4_GUILDS_PLUGIFY_SDK k4_guilds_plugify_sdk)
endif()

if(TARGET s2sdk_bindings)
    set(K4_GUILDS_S2SDK_BINDINGS s2sdk_bindings)
else()
    add_library(k4_guilds_s2sdk_bindings_obj OBJECT "${CMAKE_SOURCE_DIR}/sdk/include/s2sdk/s2sdk.cpp")
    target_include_directories(k4_guilds_s2sdk_bindings_obj SYSTEM PUBLIC "${CMAKE_SOURCE_DIR}/sdk/include")
    target_compile_definitions(k4_guilds_s2sdk_bindings_obj PUBLIC _GLIBCXX_USE_CXX11_ABI=1)
    set(K4_GUILDS_S2SDK_BINDINGS k4_guilds_s2sdk_bindings_obj)
endif()

# hudmenu is an *optional* API dependency (an existing Plugify plugin, not a wrapper library): its
# GetClientMenuType() is the only source of the player's own "!menu" choice. Without it the plugin
# falls back to the old HUD_LAYOUT default ("hud" when registered, else s2sdk "button").
if(TARGET hudmenu_bindings)
    set(K4_GUILDS_HUDMENU_BINDINGS hudmenu_bindings)
elseif(EXISTS "${CMAKE_SOURCE_DIR}/sdk/include/hudmenu/hudmenu.cpp")
    add_library(k4_guilds_hudmenu_bindings_obj OBJECT "${CMAKE_SOURCE_DIR}/sdk/include/hudmenu/hudmenu.cpp")
    target_include_directories(k4_guilds_hudmenu_bindings_obj SYSTEM PUBLIC "${CMAKE_SOURCE_DIR}/sdk/include")
    target_compile_definitions(k4_guilds_hudmenu_bindings_obj PUBLIC _GLIBCXX_USE_CXX11_ABI=1)
    set(K4_GUILDS_HUDMENU_BINDINGS k4_guilds_hudmenu_bindings_obj)
else()
    message(FATAL_ERROR "k4_guilds needs the generated hudmenu consumer bindings "
            "(sdk/include/hudmenu/hudmenu.cpp)")
endif()

# plgdb: the root CMakeLists.txt adds libs/plgdb when the monorepo has it. A standalone export ships
# libs/plgdb too (docs/PLUGIN_STANDARD.md §7), so the target must exist either way.
if(NOT TARGET plgdb)
    message(FATAL_ERROR "k4_guilds needs the plgdb library (libs/plgdb)")
endif()

configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/k4_guilds.pplugin.in"
        "${CMAKE_CURRENT_BINARY_DIR}/k4_guilds.pplugin"
)

# --- core ---------------------------------------------------------------------
add_library(k4_guilds_core STATIC
        src/k4_core.cpp
        src/k4_files.cpp
        src/kv1.cpp
)
target_include_directories(k4_guilds_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
# nlohmann/json is bundled under sdk/include.
target_include_directories(k4_guilds_core SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/sdk/include")
target_compile_options(k4_guilds_core PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(k4_guilds_core PUBLIC _GLIBCXX_USE_CXX11_ABI=1)

# --- plugin -------------------------------------------------------------------
add_library(plugify-k4_guilds SHARED
        src/plugin.cpp
        src/k4_store.cpp
        # Binding globals (__s2sdk_* / __hudmenu_*) filled by the plugify module from the
        # plugins declared as dependencies.
        $<TARGET_OBJECTS:${K4_GUILDS_S2SDK_BINDINGS}>
        $<TARGET_OBJECTS:${K4_GUILDS_HUDMENU_BINDINGS}>
)
target_include_directories(plugify-k4_guilds PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated s2sdk/hudmenu/plg headers must not fail -Werror on our sources.
target_include_directories(plugify-k4_guilds SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/sdk/include"
        "${CMAKE_SOURCE_DIR}/libs/plgdb/include")
target_link_libraries(plugify-k4_guilds PRIVATE
        k4_guilds_core
        plgdb
        ${K4_GUILDS_PLUGIFY_SDK}
        -static-libstdc++ -static-libgcc
)
target_compile_options(plugify-k4_guilds PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(plugify-k4_guilds PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="k4_guilds"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
)
set_target_properties(plugify-k4_guilds PROPERTIES OUTPUT_NAME k4_guilds)

if(UNIX AND NOT APPLE)
    target_link_options(plugify-k4_guilds PRIVATE
            "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/sym/version_script.lds")
endif()

# --- tests --------------------------------------------------------------------
# The top-level CMakeLists declares the PLUGIFY_PORT_TESTS option *after* the plugin directories,
# so on a first configure it is not defined here yet: default it to the same ON. An explicit
# -DPLUGIFY_PORT_TESTS=OFF (the exported standalone build) is already in the cache and still wins.
if(NOT DEFINED PLUGIFY_PORT_TESTS)
    set(PLUGIFY_PORT_TESTS ON)
endif()
if(PLUGIFY_PORT_TESTS)
    # Engine-free: config parsing, the KV1 reader and the pisexcompat file/phrase/colour/database
    # semantics, level derivation, kill/death accounting with clamp/warmup/teamkill rules, the SQL
    # builders, command parsing, the menu-type table and the handle rule, plus an SQLite smoke test
    # of the store against a schema-only copy of the live clans tables. ./test.sh also runs the
    # same statements on the throwaway MariaDB (MYSQL_*), which is the production driver.
    add_executable(k4_guilds_tests tests/k4_guilds_tests.cpp src/k4_store.cpp)
    target_include_directories(k4_guilds_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_include_directories(k4_guilds_tests SYSTEM PRIVATE
            "${CMAKE_SOURCE_DIR}/sdk/include"
            "${CMAKE_SOURCE_DIR}/libs/plgdb/include")
    target_link_libraries(k4_guilds_tests PRIVATE k4_guilds_core plgdb -static-libstdc++ -static-libgcc)
    target_compile_options(k4_guilds_tests PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(k4_guilds_tests PRIVATE
            _GLIBCXX_USE_CXX11_ABI=1
            K4_GUILDS_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets"
            K4_GUILDS_TEST_PHRASES="${CMAKE_CURRENT_SOURCE_DIR}/translations/k4_guilds.phrases.txt"
            K4_GUILDS_TEST_CONFIG="${CMAKE_CURRENT_SOURCE_DIR}/assets/config.json")
    set_target_properties(k4_guilds_tests PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")

    # The real src/plugin.cpp compiled into the test; every __s2sdk_* / __hudmenu_* binding slot is
    # a recording fake and the database is a real SQLite file next to the test. Covers the map gate
    # (B1/B2), startup/teardown (A1/A7), the handle rule (C1), client checks (C2), schema caching
    # (C8), the accumulators (C9), the native menu queue and teardown (C10), commands (C11) and the
    # event player-slot reads (C12).
    add_executable(k4_guilds_plugin_tests
            tests/k4_guilds_plugin_tests.cpp
            src/k4_store.cpp
            $<TARGET_OBJECTS:${K4_GUILDS_S2SDK_BINDINGS}>
            $<TARGET_OBJECTS:${K4_GUILDS_HUDMENU_BINDINGS}>
    )
    target_include_directories(k4_guilds_plugin_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_include_directories(k4_guilds_plugin_tests SYSTEM PRIVATE
            "${CMAKE_SOURCE_DIR}/sdk/include"
            "${CMAKE_SOURCE_DIR}/libs/plgdb/include")
    target_link_libraries(k4_guilds_plugin_tests PRIVATE
            k4_guilds_core
            plgdb
            ${K4_GUILDS_PLUGIFY_SDK}
            -static-libstdc++ -static-libgcc)
    target_compile_options(k4_guilds_plugin_tests PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(k4_guilds_plugin_tests PRIVATE
            _GLIBCXX_USE_CXX11_ABI=1
            PLUGIFY_MODULE="k4_guilds"
            PLUGIFY_IS_DEBUG=0
            PLUGIFY_IS_RELEASE=1
            K4_GUILDS_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    set_target_properties(k4_guilds_plugin_tests PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")

    # Runs the real package.sh from an isolated fixture: the layout, the databases.cfg.example
    # (never databases.cfg), the empty credentials and every OUT_DIR guard (outside BUILD_DIR,
    # BUILD_DIR itself, /, a symlink escape, a K4_GUILDS_SERVER_ROOTS tree). The script itself is
    # read from the plugin directory; nothing outside plugins/k4_guilds/ is touched.
    add_executable(k4_guilds_package_tests tests/k4_guilds_package_tests.cpp)
    target_include_directories(k4_guilds_package_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(k4_guilds_package_tests PRIVATE
            k4_guilds_core
            -static-libstdc++ -static-libgcc)
    target_compile_options(k4_guilds_package_tests PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(k4_guilds_package_tests PRIVATE
            _GLIBCXX_USE_CXX11_ABI=1
            K4_GUILDS_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    set_target_properties(k4_guilds_package_tests PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
endif()
