#include "block_system.h"
#include "hud.h"
#include "raylib.h"
#include "camera.h"
#include "renderer.h"
#include "world.h"
#include "chat.h"
#include "player.h"

#define INITIAL_WIDTH 1280
#define INITIAL_HEIGHT 720

int main(void){

  SetTraceLogLevel(LOG_WARNING);
  ChangeDirectory(GetApplicationDirectory());
  //SetTargetFPS(60); 
  //SetConfigFlags(FLAG_VSYNC_HINT);

  InitWindow(INITIAL_WIDTH, INITIAL_HEIGHT, "MineGame Beta 2");
  ToggleBorderlessWindowed();
  DisableCursor();

  InitRenderer();

  InitBlockRegistry();
  LoadAllBlockDefinitions("assets/blocks");

  World *world = (World*)MemAlloc(sizeof(World));
  InitWorld(world);

  Player player = InitPlayer((Vector3){0, 17, 0});
  Camera3D camera = CreateGameCamera();

  ChatState chat;
  InitChat(&chat);

  while (!WindowShouldClose()) {

    float dt = GetFrameTime();

    if(IsKeyPressed(KEY_F11)){
      ToggleBorderlessWindowed();
    }

    UpdateWorld(world, player.position, 2);
    UpdateChat(&chat, &camera, &player, world);

    bool hasControl = !chat.isActive;
    UpdatePlayer(&player, &camera, world, dt, hasControl);
    HandlePlayerInteraction(&player, &camera, world, hasControl);

    if(hasControl){
      UpdateGameCamera(&camera, GetMouseDelta());
    }


    BeginDrawing();{

      ClearBackground(SKYBLUE);

      BeginMode3D(camera);{

        DrawWorld(world);

        if(player.targetBlock.hit){
          DrawBlockHighlight(player.targetBlock.blockPos);
        }

        DrawPlayerDebug(world, &player);

      }EndMode3D();

      DrawHUD(&player, world, true);
      DrawChat(&chat);

    }EndDrawing();
  }

  MemFree(world);

  CloseWindow();
  return 0;
}
