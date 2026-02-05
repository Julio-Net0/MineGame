#include "raylib.h"

int main(void){
  InitWindow(800, 400, "MineGame");
  Camera3D camera = { 0 };
  camera.position = (Vector3){10.0f, 10.0f, 10.0f};
  camera.target = (Vector3){0.0f, 0.0f, 0.0f};
  camera.up = (Vector3){0.0f, 1.0f, 0.0f};
  camera.fovy = 45.0f;
  camera.projection = CAMERA_PERSPECTIVE;

  SetTargetFPS(60);

  while (!WindowShouldClose()) {
    BeginDrawing();
    ClearBackground(SKYBLUE);
    BeginMode3D(camera);

    DrawCube((Vector3){0,0,0}, 2.0f, 2.0f, 2.0f, RED);
    DrawGrid(100, 1.0f);

    EndMode3D();
    DrawText("3D ambient", 10, 40, 20, DARKGRAY);
    EndDrawing();
  }

  CloseWindow();
}
