#include "block_system.h"
#include "hud.h"
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

    if(!chat.isActive){
      UpdateGameCamera(&camera);
    }

    HandlePlayerInteraction(&player, &camera, &world, !chat.isActive);

    BeginDrawing();
    ClearBackground(SKYBLUE);
    BeginMode3D(camera);

    DrawWorld(&world);

    if(player.targetBlock.hit){
      DrawBlockHighlight(player.targetBlock.blockPos);
    }

    DrawPlayerDebug(&world, &player);

    EndMode3D();

    DrawHUD(&player, &world, true);

    DrawChat(&chat);

    DrawFPS(10, 10);

    EndDrawing();
  }

  CloseWindow();
  return 0;
}
