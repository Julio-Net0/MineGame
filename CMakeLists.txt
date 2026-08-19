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

include(FetchContent)
FetchContent_Declare(
  raylib
  GIT_REPOSITORY https://github.com/raysan5/raylib.git
  GIT_TAG        5.5
)

set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(raylib)

add_executable(${PROJECT_NAME}
  src/main.c
  src/world/world.c
  src/world/chunk.c
  src/world/chunk_worker.c
  src/world/block_system.c
  src/world/biome.c
  src/world/prefab.c
  src/world/prefab_capture.c
  src/world/feature.c
  src/persistence/world_save.c
  src/persistence/chunk_serial.c
  src/player/player.c
  src/input/input.c
  src/input/raylib_input.c
  src/platform/raylib_platform.c
  src/render/renderer.c
  src/render/raylib_backend.c
  src/render/camera.c
  src/render/hud.c
  src/ui/chat.c
  src/ui/commands.c
  src/ui/debug.c
  src/core/utils.c
  src/core/fileio.c
  src/core/log.c
  src/core/noise.c
  src/core/profiler.c
  src/third_party/cJSON.c
)

target_link_libraries(${PROJECT_NAME} raylib Threads::Threads)

# Applied to the game target only, not to raylib. The cross-TU inlining worth
# having is within this project's own code; raylib is called once per draw,
# where inlining buys nothing, and leaving it out avoids its build interacting
# with LTO under MinGW. Debug keeps full inlining off so it stays debuggable.
if(MINEGAME_IPO_SUPPORTED AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
  set_property(TARGET ${PROJECT_NAME}
               PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

target_include_directories(${PROJECT_NAME} PUBLIC src ${raylib_SOURCE_DIR}/src)

if(MINEGAME_PROFILE)
  target_compile_definitions(${PROJECT_NAME} PRIVATE MINEGAME_PROFILE)
endif()

# Mirror assets into the build tree on every build. file(COPY) at configure time
# never prunes, so a renamed or deleted source asset would linger in the build
# copy and shadow the real one; wiping the destination first keeps them in sync
# and removes orphans.
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_CURRENT_BINARY_DIR}/assets
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_CURRENT_SOURCE_DIR}/assets ${CMAKE_CURRENT_BINARY_DIR}/assets
  COMMENT "Syncing assets to build tree (pruning orphans)"
  VERBATIM)
