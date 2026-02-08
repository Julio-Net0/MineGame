$BUILD_DIR = "build"
$EXECUTABLE = "MineGame.exe"

cmake -S . -B $BUILD_DIR -G "Ninja" -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

Write-Host "`n--- Compiling with Ninja ---" -ForegroundColor Cyan
cmake --build $BUILD_DIR

if (Test-Path "$BUILD_DIR\$EXECUTABLE") {
    Write-Host "`n--- Starting game ---" -ForegroundColor Green
    & "$BUILD_DIR\$EXECUTABLE"
} else {
    Write-Host "`n[!] Error: Executable not found." -ForegroundColor Red
}
