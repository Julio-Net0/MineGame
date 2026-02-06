$BUILD_DIR = "build"
$EXECUTABLE = "MineGame.exe"

cmake -S . -B $BUILD_DIR -G "Ninja" -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

Write-Host "`n--- Compilando com Ninja ---" -ForegroundColor Cyan
cmake --build $BUILD_DIR

if (Test-Path "$BUILD_DIR\$EXECUTABLE") {
    Write-Host "`n--- Iniciando Jogo ---" -ForegroundColor Green
    & "$BUILD_DIR\$EXECUTABLE"
} else {
    Write-Host "`n[!] Erro: Executável não encontrado." -ForegroundColor Red
}
