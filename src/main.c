#include "block_system.h"
#include "chunk.h"
#include "chunk_worker.h"
#include "debug.h"
#include "hud.h"
#include "pthread.h"
#include "raylib.h"
#include "camera.h"
#include "renderer.h"
#include "world.h"
#include "chat.h"
#include "player.h"
#include "rlgl.h"

#define INITIAL_WIDTH 1280
#define INITIAL_HEIGHT 720

int main(void){

  SetTraceLogLevel(LOG_WARNING);
  ChangeDirectory(GetApplicationDirectory());

  InitWindow(INITIAL_WIDTH, INITIAL_HEIGHT, "MineGame Beta 2");
  ToggleBorderlessWindowed();
  DisableCursor();

  InitRenderer();

  InitBlockRegistry();
  LoadAllBlockDefinitions("assets/blocks");

  World *world = (World*)MemAlloc(sizeof(World));
  InitWorld(world);
  TraceLog(LOG_INFO, "MAIN THREAD ID: %p", (void*)pthread_self());
  InitChunkWorker();

  Player player = InitPlayer((Vector3){0, 25, 0});
  
  Camera3D playerCamera = CreateGameCamera();
  Camera3D freeCamera = CreateGameCamera();
  bool wasFreecam = false;

  ChatState chat;
  InitChat(&chat);

  while (!WindowShouldClose()) {

    float dt = GetFrameTime();

    if(IsKeyPressed(KEY_F11)){
      ToggleBorderlessWindowed();
    }

    UpdateWorld(world, player.position, MAX_RENDER_DISTANCE);
    
    bool hasControl = !chat.isActive;
    UpdatePlayer(&player, &playerCamera, world, dt, hasControl);
    
    Camera3D *activeCamera = &playerCamera;

    if(g_debug.freecam){
      if (!wasFreecam) {
          freeCamera = playerCamera;
      }
      UpdateCamera(&freeCamera, CAMERA_FREE);
      activeCamera = &freeCamera;
    } else if(hasControl){
      UpdateGameCamera(&playerCamera, GetMouseDelta());
    }
    
    wasFreecam = g_debug.freecam;

    UpdateChat(&chat, activeCamera, &player, world);
    HandlePlayerInteraction(&player, activeCamera, world, hasControl);

    for(int i = 0; i < world->chunkCount; i++){
      Chunk *c = &world->chunks[i];
      if(c->isGenerated && c->terrainJustGenerated){
        UpdateNeighborsDirtyFlag(world, c->chunkX, c->chunkY, c->chunkZ);
        c->terrainJustGenerated = false; // Desliga para não dar o loop infinito!
      }
    }

    int meshesBuiltThisFrame = 0;
    int maxMeshesPerFrame = 2;

    for(int i = 0; i < world->chunkCount; i++){
      Chunk *c = &world->chunks[i];

      if(c->isGenerated && c->isDirty && !c->isGenerating){
        BuildChunkMesh(world, c);
        meshesBuiltThisFrame++;

        if(meshesBuiltThisFrame >= maxMeshesPerFrame) {
          break;
        }
      }
    }

    BeginDrawing();{

      ClearBackground(SKYBLUE);

      BeginMode3D(*activeCamera);{

        DrawWorld(world, playerCamera);

        if(player.targetBlock.hit){
          DrawBlockHighlight(player.targetBlock.blockPos);
        }

        DrawAABBDebug(world, &player);
      }EndMode3D();

      DrawHUD(&player, world, true);
      DrawChat(&chat);
    }EndDrawing();
  }

  CloseChunkWorker();
  MemFree(world);
  CloseWindow();
  return 0;
}
