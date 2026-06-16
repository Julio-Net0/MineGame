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
#include "world_save.h"

#define INITIAL_WIDTH 1280
#define INITIAL_HEIGHT 720

#define PLAYER_SPAWN_X 0.0f
#define PLAYER_SPAWN_Y 25.0f
#define PLAYER_SPAWN_Z 0.0f

static void InitGame(World **world, Player *player, Camera3D *playerCamera,
                     ChatState *chat, Camera3D *freeCamera) {
  SetTraceLogLevel(LOG_WARNING);
  ChangeDirectory(GetApplicationDirectory());

  InitWindow(INITIAL_WIDTH, INITIAL_HEIGHT, "MineGame Beta 3");
  ToggleBorderlessWindowed();
  DisableCursor();

  InitRenderer();

  InitBlockRegistry();
  LoadAllBlockDefinitions("assets/blocks");

  *world = (World *)MemAlloc(sizeof(World));
  InitWorldSave();
  InitWorld(*world);
  TraceLog(LOG_INFO, "MAIN THREAD ID: %p", (void *)pthread_self());
  InitChunkWorker();

  *player = InitPlayer(
      (Vector3){PLAYER_SPAWN_X, PLAYER_SPAWN_Y, PLAYER_SPAWN_Z});
  *playerCamera = CreateGameCamera();
  *freeCamera = CreateGameCamera();
  InitChat(chat);
}

static void CleanupGame(World *world) {
  CloseChunkWorker();

  // Sweep and save all modified chunks
  for (int i = 0; i < world->chunkCount; i++) {
    if (world->chunks[i].isModified) {
      SaveChunkToDisk(&world->chunks[i]);
    }
  }

  CloseWorldSave();
  MemFree(world);
  CloseRenderer();
  CloseWindow();
}

static void UpdateSystemInputs(bool *showDebug) {
  if (IsKeyPressed(KEY_F11)) {
    ToggleBorderlessWindowed();
  }

  if (IsKeyPressed(KEY_F3)) {
    *showDebug = !*showDebug;
  }
}

static void UpdateCameras(Camera3D *playerCamera, Camera3D *freeCamera,
                          bool *wasFreecam, bool hasControl,
                          Camera3D **activeCamera) {
  *activeCamera = playerCamera;

  if (g_debug.freecam) {
    if (!*wasFreecam) {
      *freeCamera = *playerCamera;
    }
    UpdateCamera(freeCamera, CAMERA_FREE);
    *activeCamera = freeCamera;
  } else if (hasControl) {
    UpdateGameCamera(playerCamera, GetMouseDelta());
  }

  *wasFreecam = g_debug.freecam;
}

static void UpdateWorldChunks(World *world) {
  for (int i = 0; i < world->chunkCount; i++) {
    Chunk *c = &world->chunks[i];
    if (c->isGenerated && c->terrainJustGenerated) {
      UpdateNeighborsDirtyFlag(world, c->chunkX, c->chunkY, c->chunkZ);
      c->terrainJustGenerated = false;
    }
  }
}

static void BuildMeshes(World *world) {
  int meshesBuiltThisFrame = 0;
  int maxMeshesPerFrame = 2;

  for (int i = 0; i < world->chunkCount; i++) {
    Chunk *c = &world->chunks[i];

    if (c->isGenerated && c->isDirty && !c->isGenerating &&
        AreNeighborsGenerated(world, c)) {
      BuildChunkMesh(world, c);
      meshesBuiltThisFrame++;

      if (meshesBuiltThisFrame >= maxMeshesPerFrame) {
        break;
      }
    }
  }
}

static void RenderGame(World *world, Player *player, Camera3D *activeCamera,
                       ChatState *chat, bool showDebug) {
  BeginDrawing();
  {
    ClearBackground(SKYBLUE);

    BeginMode3D(*activeCamera);
    {
      DrawWorld(world, *activeCamera);

      if (player->targetBlock.hit) {
        DrawBlockHighlight(player->targetBlock.blockPos);
      }

      DrawAABBDebug(world, player);
    }
    EndMode3D();

    DrawHUD(player, world, *activeCamera, showDebug);
    DrawChat(chat);
  }
  EndDrawing();
}

int main(void) {
  World *world = NULL;
  Player player;
  Camera3D playerCamera;
  Camera3D freeCamera;
  bool wasFreecam = false;
  ChatState chat;
  bool showDebug = false;

  InitGame(&world, &player, &playerCamera, &chat, &freeCamera);

  while (!WindowShouldClose()) {
    float dt = GetFrameTime();

    UpdateSystemInputs(&showDebug);

    Vector3 loadCenter =
        (g_debug.freecam && wasFreecam) ? freeCamera.position : player.position;
    UpdateWorld(world, loadCenter, MAX_RENDER_DISTANCE);

    bool hasControl = !chat.isActive;
    UpdatePlayer(&player, &playerCamera, world, dt, hasControl);

    Camera3D *activeCamera = NULL;
    UpdateCameras(&playerCamera, &freeCamera, &wasFreecam, hasControl,
                  &activeCamera);

    UpdateChat(&chat, activeCamera, &player, world);
    HandlePlayerInteraction(&player, activeCamera, world, hasControl);

    UpdateWorldChunks(world);
    BuildMeshes(world);

    RenderGame(world, &player, activeCamera, &chat, showDebug);
  }

  CleanupGame(world);
  return 0;
}
