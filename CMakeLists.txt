cmake_minimum_required(VERSION 3.11)
project(MineGame)

set(CMAKE_C_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# CMake contributes no optimization flags when CMAKE_BUILD_TYPE is empty, so a
# tree configured without one compiles at the compiler's default -O0. Default it
# here so unoptimized is a deliberate choice (-DCMAKE_BUILD_TYPE=Debug) rather
# than the result of not making one. Multi-config generators are left alone:
# CMAKE_BUILD_TYPE means nothing to them and forcing it would mislead.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
               Debug Release MinSizeRel RelWithDebInfo)
endif()

# No -march or -mtune. The lowest-end target is an AMD Phenom II (K10), which
# has SSE3 and SSE4a but NOT SSSE3, SSE4.1 or SSE4.2. The x86-64-v2 level
# requires SSE4.2 and POPCNT, so a binary built for it faults with an illegal
# instruction on that machine; -march=native would be worse still in CI, baking
# the runner's instruction set into a binary shipped to unknown hardware.
# Targeting a newer baseline later needs runtime dispatch or per-target builds,
# not a global flag.

# Link-time optimization, so the compiler can inline across translation units.
# That matters specifically here: GetBlockDef lives in world/block_system.c and
# GetChunkFromWorld in world/world.c, and both are called from the mesher's
# innermost loops in render/renderer.c. Without it each stays a real call in the
# hottest loop in the engine. Checked rather than assumed, so a toolchain that
# cannot do it still produces a working build.
include(CheckIPOSupported)
check_ipo_supported(RESULT MINEGAME_IPO_SUPPORTED
                    OUTPUT MINEGAME_IPO_ERROR
                    LANGUAGES C)
if(NOT MINEGAME_IPO_SUPPORTED)
  message(STATUS "MineGame: link-time optimization unavailable: ${MINEGAME_IPO_ERROR}")
endif()

# The engine uses pthreads directly (chunk workers, region I/O locks). Linking
# them implicitly happens to work on glibc 2.34+ and on MinGW, but that is a
# toolchain accident rather than a guarantee; request the flag explicitly so any
# supported toolchain links them the way it expects.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# On by default: the point of the profiler is to measure the OPTIMIZED build,
# the overlay it feeds is already behind F3 and hidden by default, and at the
# coarse granularity the scopes are placed at its cost is not measurable.
# Turning it off becomes interesting when the game ships for real.
option(MINEGAME_PROFILE "Build with frame profiling instrumentation" ON)

# Off by default: this cross-checks every cell of every chunk snapshot against
# the live world lookup it replaced, which costs far more than the meshing it
# guards. It exists to prove the invariant the threaded mesher rests on -- that
# a snapshot returns exactly what reading the world returned -- and is worth
# turning on after any change to what the mesher reads.
option(MINEGAME_SNAPSHOT_VERIFY "Cross-check chunk snapshots against live world reads" OFF)

include(FetchContent)
FetchContent_Declare(
  raylib
  GIT_REPOSITORY https://github.com/raysan5/raylib.git
  GIT_TAG        5.5
)

set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(raylib)

set(MINEGAME_SIM_SOURCES
  src/server/server.c
  src/net/protocol.c
  src/net/loopback.c
  src/world/world.c
  src/world/chunk.c
  src/world/chunk_worker.c
  src/world/chunk_snapshot.c
  src/world/block_system.c
  src/world/biome.c
  src/world/prefab.c
  src/world/prefab_capture.c
  src/world/feature.c
  src/world/surface.c
  src/world/terrain_shape.c
  src/world/terrain_density.c
  src/world/cave_fields.c
  src/world/stone_filler.c
  src/world/worldgen_strata.c
  src/world/worldgen_column_cache.c
  src/persistence/world_save.c
  src/persistence/chunk_serial.c
  src/player/player.c
  src/platform/os_limits.c
  src/platform/os_time.c
  src/core/utils.c
  src/core/fileio.c
  src/core/log.c
  src/core/noise.c
  src/core/profiler.c
  src/third_party/cJSON.c
)

add_executable(${PROJECT_NAME}
  src/main.c
  src/automation/script.c
  src/automation/worldgen_preview.c
  ${MINEGAME_SIM_SOURCES}
  src/input/input.c
  src/input/raylib_input.c
  src/platform/raylib_platform.c
  src/render/renderer.c
  src/render/raylib_backend.c
  src/render/camera.c
  src/ui/hud.c
  src/ui/ui.c
  src/ui/draw_list.c
  src/ui/gui_atlas.c
  src/ui/block_icon.c
  src/ui/font.c
  src/ui/pause_menu.c
  src/ui/creative_inventory.c
  src/ui/chat.c
  src/ui/commands.c
  src/ui/debug.c
  src/core/image.c
  src/core/resource_pack.c
  src/core/launch_options.c
  src/third_party/png_write.c
)

add_executable(MineGameServer
  src/server/server_main.c
  src/server/protocol_selftest.c
  src/server/worldgen_selftest.c
  ${MINEGAME_SIM_SOURCES}
)

target_link_libraries(${PROJECT_NAME} raylib Threads::Threads)
target_link_libraries(MineGameServer Threads::Threads)
if(WIN32)
  target_link_libraries(MineGameServer psapi)
else()
  # The client gets libm transitively through raylib; the server must link it.
  target_link_libraries(MineGameServer m)
endif()

# Applied to the game targets only, not to raylib. The cross-TU inlining worth
# having is within this project's own code; raylib is called once per draw,
# where inlining buys nothing, and leaving it out avoids its build interacting
# with LTO under MinGW. Debug keeps full inlining off so it stays debuggable.
if(MINEGAME_IPO_SUPPORTED AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
  set_property(TARGET ${PROJECT_NAME}
               PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
  set_property(TARGET MineGameServer
               PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

target_include_directories(${PROJECT_NAME} PUBLIC src ${raylib_SOURCE_DIR}/src)
target_include_directories(MineGameServer PUBLIC src)

# Terrain generation and its pure density probe must compute bit-identical
# floats in different translation units, and client and server must agree on
# generated terrain. Fusing a*b+c into one FMA is allowed by default in GNU C and
# would round differently depending on where the compiler chose to fuse, so it
# is disabled. The x86-64 baseline has no FMA either way; this keeps other
# targets honest.
target_compile_options(${PROJECT_NAME} PRIVATE -ffp-contract=off)
target_compile_options(MineGameServer PRIVATE -ffp-contract=off)

if(MINEGAME_PROFILE)
  target_compile_definitions(${PROJECT_NAME} PRIVATE MINEGAME_PROFILE)
  target_compile_definitions(MineGameServer PRIVATE MINEGAME_PROFILE)
endif()

if(MINEGAME_SNAPSHOT_VERIFY)
  target_compile_definitions(${PROJECT_NAME} PRIVATE MINEGAME_SNAPSHOT_VERIFY)
  target_compile_definitions(MineGameServer PRIVATE MINEGAME_SNAPSHOT_VERIFY)
endif()

set_source_files_properties(src/platform/os_limits.c PROPERTIES
  COMPILE_DEFINITIONS _GNU_SOURCE)

# FastNoiseLite hashes lattice coordinates with signed int multiplies that are
# meant to wrap. Signed overflow is undefined in C, and -O3 with LTO is free to
# exploit that, so the one file instantiating it gets defined wrapping.
set_source_files_properties(src/core/noise.c PROPERTIES
  COMPILE_OPTIONS -fwrapv)

# clock_gettime, nanosleep and readlink need POSIX.1-2008 under -std=c17.
if(NOT WIN32)
  set_source_files_properties(src/platform/os_time.c PROPERTIES
    COMPILE_DEFINITIONS _POSIX_C_SOURCE=200809L)
endif()

# Mirror assets into the build tree on every build. file(COPY) at configure time
# never prunes, so a renamed or deleted source asset would linger in the build
# copy and shadow the real one; wiping the source-backed subdirectories first
# keeps them in sync and removes orphans while leaving synced textures intact.
if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.26")
  set(MINEGAME_COPY_DIR_CMD copy_directory_if_different)
else()
  set(MINEGAME_COPY_DIR_CMD copy_directory)
endif()

# A custom target with no outputs is always out of date, so it runs on every
# build. A POST_BUILD step only ran when the executable relinked, which left an
# asset-only change (new block JSON) out of the build tree.
add_custom_target(minegame_sync_assets ALL
  COMMAND ${CMAKE_COMMAND} -E rm -rf
          ${CMAKE_CURRENT_BINARY_DIR}/assets/atlas
          ${CMAKE_CURRENT_BINARY_DIR}/assets/biomes
          ${CMAKE_CURRENT_BINARY_DIR}/assets/blocks
          ${CMAKE_CURRENT_BINARY_DIR}/assets/prefabs
          ${CMAKE_CURRENT_BINARY_DIR}/assets/shaders
          ${CMAKE_CURRENT_BINARY_DIR}/assets/worldgen
          ${CMAKE_CURRENT_BINARY_DIR}/assets/biome_params.json
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_CURRENT_SOURCE_DIR}/assets ${CMAKE_CURRENT_BINARY_DIR}/assets
  COMMAND ${CMAKE_COMMAND} -E ${MINEGAME_COPY_DIR_CMD}
          ${CMAKE_CURRENT_SOURCE_DIR}/textures/block
          ${CMAKE_CURRENT_BINARY_DIR}/assets/textures/block
  COMMAND ${CMAKE_COMMAND} -E ${MINEGAME_COPY_DIR_CMD}
          ${CMAKE_CURRENT_SOURCE_DIR}/textures/gui
          ${CMAKE_CURRENT_BINARY_DIR}/assets/textures/gui
  COMMAND ${CMAKE_COMMAND} -E ${MINEGAME_COPY_DIR_CMD}
          ${CMAKE_CURRENT_SOURCE_DIR}/textures/font
          ${CMAKE_CURRENT_BINARY_DIR}/assets/textures/font
  COMMAND ${CMAKE_COMMAND} -E ${MINEGAME_COPY_DIR_CMD}
          ${CMAKE_CURRENT_SOURCE_DIR}/textures/external
          ${CMAKE_CURRENT_BINARY_DIR}/assets/textures/external
  COMMENT "Syncing assets and textures to build tree"
  VERBATIM)
add_dependencies(${PROJECT_NAME} minegame_sync_assets)
