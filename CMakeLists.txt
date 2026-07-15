cmake_minimum_required(VERSION 3.11)
project(MineGame)

set(CMAKE_C_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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
  src/third_party/stb_perlin.c
  src/third_party/cJSON.c
)

target_link_libraries(${PROJECT_NAME} raylib)

target_include_directories(${PROJECT_NAME} PUBLIC src ${raylib_SOURCE_DIR}/src)

file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/assets DESTINATION ${CMAKE_CURRENT_BINARY_DIR})
