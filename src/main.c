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


  Chunk chunk = {0};
  chunk.position = (Vector3){0,0,0};
  GenerateFlatChunk(&chunk);

  ChatState chat;
  InitChat(&chat);

  Player player = InitPlayer((Vector3){0, 17, 0});

  ToggleBorderlessWindowed();

  while (!WindowShouldClose()) {

    float dt = GetFrameTime();

    if(IsKeyPressed(KEY_F11)){
      ToggleBorderlessWindowed();
    }

    UpdateChat(&chat, &camera, &player);

    Vector3 rayDir = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
    RaycastResult hit = RayCastToBlock(&chunk, camera.position, rayDir, 10.0F);

    if(!chat.isActive){

      UpdateGameCamera(&camera);
      UpdatePlayer(&player, &camera, &chunk,dt);

      if(hit.hit){
        if(IsMouseButtonPressed(MOUSE_LEFT_BUTTON)){
          SetBlock(&chunk, hit.blockPos, 0);
        }

        if(IsMouseButtonPressed(MOUSE_RIGHT_BUTTON)){
          Vector3 placePos = Vector3Add(hit.blockPos, hit.normal);

          SetBlock(&chunk, placePos, 1);
        }
      }
    }

    BeginDrawing();
    ClearBackground(SKYBLUE);

    BeginMode3D(camera);

    DrawChunk(&chunk);

    if(hit.hit){
      DrawBlockHighlight(hit.blockPos);
    }

    DrawPlayerDebug(&chunk, &player);

    EndMode3D();

    DrawChat(&chat);

    DrawCircle(GetScreenWidth() / 2, GetScreenHeight() / 2, 2, WHITE);

    DrawFPS(10, 10);

    EndDrawing();
  }

  CloseWindow();
  return 0;
}
