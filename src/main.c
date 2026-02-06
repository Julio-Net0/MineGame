#include "raylib.h"
#include "camera.h"

#define INITIAL_WIDTH 1028
#define INITIAL_HEIGHT 720

#define TARGET_FPS 60

int main(void){
  InitWindow(INITIAL_WIDTH, INITIAL_HEIGHT, "MineGame Beta1");

  Camera3D camera = CreateGameCamera();

  DisableCursor();

  SetTargetFPS(TARGET_FPS);

  while (!WindowShouldClose()) {
    UpdateGameCamera(&camera);

    BeginDrawing();
    ClearBackground(SKYBLUE);
    BeginMode3D(camera);

    DrawGrid(20, 1.0F);
    DrawCube((Vector3){ 0.0F, 0.0F, 0.0F }, 10.0F, 10.0F, 10.0F, RED);

    EndMode3D();

    DrawFPS(10, 10);

    EndDrawing();
  }

  CloseWindow();
  return 0;
}
