$BUILD_DIR = "build"
$EXECUTABLE = "MineGame.exe"

# Release is passed explicitly rather than left to the default in CMakeLists.txt,
# so the build type this script produces is visible here. Release means -O3 with
# link-time optimization; without a build type CMake contributes no optimization
# flags at all and GCC falls back to -O0.
#
# For an unoptimized build with debug info, configure by hand with
# -DCMAKE_BUILD_TYPE=Debug. To build without the frame profiler, add
# -DMINEGAME_PROFILE=OFF.
cmake -S . -B $BUILD_DIR -G "Ninja" -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

Write-Host "`n--- Compiling with Ninja ---" -ForegroundColor Cyan
cmake --build $BUILD_DIR

if (Test-Path "$BUILD_DIR\$EXECUTABLE") {
    Write-Host "`n--- Starting game ---" -ForegroundColor Green
    & "$BUILD_DIR\$EXECUTABLE"
} else {
    Write-Host "`n[!] Error: Executable not found." -ForegroundColor Red
}
