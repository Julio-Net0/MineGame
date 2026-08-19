#!/bin/bash

BUILD_DIR="build"
EXECUTABLE="MineGame"

# Configure CMake.
#
# Release is passed explicitly rather than left to the default in CMakeLists.txt,
# so the build type this script produces is visible here. Release means -O3 with
# link-time optimization; without a build type CMake contributes no optimization
# flags at all and GCC falls back to -O0.
#
# For an unoptimized build with debug info, configure by hand with
# -DCMAKE_BUILD_TYPE=Debug. To build without the frame profiler, add
# -DMINEGAME_PROFILE=OFF.
cmake -S . -B "$BUILD_DIR" -G "Ninja" -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

# Print the message in cyan (\e[36m) and reset the colour (\e[0m)
echo -e "\n\e[36m--- Compiling with Ninja ---\e[0m"
cmake --build "$BUILD_DIR"

# Check whether the executable exists
if [ -f "$BUILD_DIR/$EXECUTABLE" ]; then
    # Print the message in green (\e[32m)
    echo -e "\n\e[32m--- Starting game ---\e[0m"
    "./$BUILD_DIR/$EXECUTABLE"
else
    # Print the error message in red (\e[31m)
    echo -e "\n\e[31m[!] Error: Executable not found.\e[0m"
fi
