#include "block_system.h"
#include "raylib.h"
#include "camera.h"
#include "world.h"

#define INITIAL_WIDTH 1028
#define INITIAL_HEIGHT 720

#define TARGET_FPS 60

int main(void){
  ChangeDirectory(GetApplicationDirectory());

  InitWindow(INITIAL_WIDTH, INITIAL_HEIGHT, "MineGame Beta1");

  Camera3D camera = CreateGameCamera();

  InitBlockRegisry();
  LoadAllBlockDefinitions("assets/blocks");

  DisableCursor();

  SetTargetFPS(TARGET_FPS);

  Chunk chunk = {0};
  chunk.position = (Vector3){0,0,0};
  GenerateFlatChunk(&chunk);
  

  while (!WindowShouldClose()) {
    UpdateGameCamera(&camera);

    BeginDrawing();
    ClearBackground(SKYBLUE);
    BeginMode3D(camera);

    DrawChunk(&chunk);

    EndMode3D();

    DrawFPS(10, 10);

    EndDrawing();
  }

  CloseWindow();
  return 0;
}
