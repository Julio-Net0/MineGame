#include "block_system.h"
#include "raylib.h"
#include "camera.h"

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

  while (!WindowShouldClose()) {
    UpdateGameCamera(&camera);

    BeginDrawing();
    ClearBackground(SKYBLUE);
    BeginMode3D(camera);

    DrawGrid(20, 1.0F);

    BlockType* GrassDef = GetBlockDef(1);
    BlockType* StoneDef = GetBlockDef(2);
    Vector3 positionGrass = {0.0F, 0.5F, 0.0F};
    DrawCube(positionGrass, 1.0F, 1.0F, 1.0F, GrassDef->color);
    DrawCubeWires(positionGrass, 1.0F, 1.0F, 1.0F, BLACK);
    Vector3 positionStone = {1.0F, 0.5F, 0.0F};
    DrawCube(positionStone, 1.0F, 1.0F, 1.0F, StoneDef->color);
    DrawCubeWires(positionStone, 1.0F, 1.0F, 1.0F, BLACK);

    EndMode3D();

    DrawFPS(10, 10);

    EndDrawing();
  }

  CloseWindow();
  return 0;
}
