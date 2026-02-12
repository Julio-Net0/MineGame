#include "block_system.h"
#include "raylib.h"
#include "camera.h"
#include "renderer.h"
#include "world.h"
#include "raymath.h"

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

    Vector3 rayDir = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
    RaycastResult hit = RayCastToBlock(&chunk, camera.position, rayDir, 10.0F);


    BeginDrawing();
    ClearBackground(SKYBLUE);

    BeginMode3D(camera);

    DrawChunk(&chunk);

    if(hit.hit){
      DrawBlockHighlight(hit.blockPos);
    }

    EndMode3D();

    DrawCircle(GetScreenWidth() / 2, GetScreenHeight() / 2, 2, WHITE);

    DrawFPS(10, 10);

    EndDrawing();
  }

  CloseWindow();
  return 0;
}
