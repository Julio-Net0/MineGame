#include "block_system.h"
#include "raylib.h"
#include "camera.h"
#include "renderer.h"
#include "world.h"
#include "raymath.h"
#include "chat.h"
#include "player.h"

#define INITIAL_WIDTH GetScreenWidth()
#define INITIAL_HEIGHT GetScreenHeight()

int main(void){
  ChangeDirectory(GetApplicationDirectory());

  InitWindow(INITIAL_WIDTH, INITIAL_HEIGHT, "MineGame Beta1");

  Camera3D camera = CreateGameCamera();

  InitBlockRegisry();
  LoadAllBlockDefinitions("assets/blocks");

  DisableCursor();

  World world = {0};
  InitWorld(&world);

  ChatState chat;
  InitChat(&chat);

  Player player = InitPlayer((Vector3){0, 17, 0});

  ToggleBorderlessWindowed();

  while (!WindowShouldClose()) {

    float dt = GetFrameTime();

    if(IsKeyPressed(KEY_F11)){
      ToggleBorderlessWindowed();
    }

    UpdateWorld(&world, player.position);
    UpdateChat(&chat, &camera, &player, &world);
    UpdatePlayer(&player, &camera, &world, dt, !chat.isActive);

    Vector3 rayDir = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
    RaycastResult hit = RayCastToWorld(&world, camera.position, rayDir, 10.0F);

    if(!chat.isActive){

      UpdateGameCamera(&camera);

      if(hit.hit){
        if(IsMouseButtonPressed(MOUSE_LEFT_BUTTON)){
          SetBlockInWorld(&world, hit.blockPos, 0);
        }

        if(IsMouseButtonPressed(MOUSE_RIGHT_BUTTON)){
          Vector3 placePos = Vector3Add(hit.blockPos, hit.normal);
          SetBlockInWorld(&world, placePos, 1);
        }
      }
    }

    BeginDrawing();
    ClearBackground(SKYBLUE);
    BeginMode3D(camera);

    DrawWorld(&world);

    if(hit.hit){
      DrawBlockHighlight(hit.blockPos);
    }

    DrawPlayerDebug(&world, &player);

    EndMode3D();

    DrawChat(&chat);

    DrawCircle(GetScreenWidth() / 2, GetScreenHeight() / 2, 2, WHITE);

    DrawFPS(10, 10);

    EndDrawing();
  }

  CloseWindow();
  return 0;
}
