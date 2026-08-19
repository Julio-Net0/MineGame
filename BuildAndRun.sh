#!/bin/bash

BUILD_DIR="build"
EXECUTABLE="MineGame"

# Configure CMake
cmake -S . -B "$BUILD_DIR" -G "Ninja" -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

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
