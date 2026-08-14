#!/bin/bash

BUILD_DIR="build"
EXECUTABLE="MineGame"

# Configura o CMake
cmake -S . -B "$BUILD_DIR" -G "Ninja" -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

# Imprime a mensagem em Ciano (\e[36m) e reseta a cor (\e[0m)
echo -e "\n\e[36m--- Compiling with Ninja ---\e[0m"
cmake --build "$BUILD_DIR"

# Verifica se o arquivo executável existe
if [ -f "$BUILD_DIR/$EXECUTABLE" ]; then
    # Imprime a mensagem em Verde (\e[32m)
    echo -e "\n\e[32m--- Starting game ---\e[0m"
    "./$BUILD_DIR/$EXECUTABLE"
else
    # Imprime a mensagem de erro em Vermelho (\e[31m)
    echo -e "\n\e[31m[!] Error: Executable not found.\e[0m"
fi
