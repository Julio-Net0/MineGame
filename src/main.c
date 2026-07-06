#include "world/block_system.h"
#include "render/camera.h"
#include "ui/chat.h"
#include "world/chunk.h"
#include "world/chunk_worker.h"
#include "ui/debug.h"
#include "render/hud.h"
#include "player/player.h"
#include "pthread.h"
#include "raylib.h"
#include "render/renderer.h"
#include "rlgl.h"
#include "world/world.h"
#include "persistence/world_save.h"
#include <stdbool.h>

#define INITIAL_WIDTH 1280
#define INITIAL_HEIGHT 720

#define PLAYER_SPAWN_X 0.0F
#define PLAYER_SPAWN_Y 25.0F
#define PLAYER_SPAWN_Z 0.0F

static void InitGame(World **WorldVal, Player *PlayerVal, Camera3D *PlayerCamera,
                     ChatState *ChatVal, Camera3D *FreeCamera) {
  SetTraceLogLevel(LOG_WARNING);
  ChangeDirectory(GetApplicationDirectory());

  InitWindow(INITIAL_WIDTH, INITIAL_HEIGHT, "MineGame Beta 3");
  ToggleBorderlessWindowed();
  DisableCursor();

  InitRenderer();

  InitBlockRegistry();
  LoadAllBlockDefinitions("assets/blocks");

  *WorldVal = (World *)MemAlloc(sizeof(World));
  InitWorldSave();
  InitWorld(*WorldVal);
  TraceLog(LOG_INFO, "MAIN THREAD ID: %llu",
           (unsigned long long)pthread_self());
  InitChunkWorker();

  *PlayerVal =
      InitPlayer((Vector3){PLAYER_SPAWN_X, PLAYER_SPAWN_Y, PLAYER_SPAWN_Z});
  *PlayerCamera = CreateGameCamera();
  *FreeCamera = CreateGameCamera();
  InitChat(ChatVal);
}

static void CleanupGame(World *WorldVal) {
  CloseChunkWorker();

  // Sweep and save all modified chunks
  #pragma unroll 4
  for (int IdxI = 0; IdxI < WorldVal->ChunkCount; IdxI++) {
    if (WorldVal->Chunks[IdxI].IsModified) {
      SaveChunkToDisk(&WorldVal->Chunks[IdxI]);
    }
  }

  CloseWorldSave();
  MemFree(WorldVal);
  CloseRenderer();
  CloseWindow();
}

static void UpdateSystemInputs(bool *ShowDebug) {
  if (IsKeyPressed(KEY_F11)) {
    ToggleBorderlessWindowed();
  }

  if (IsKeyPressed(KEY_F3)) {
    *ShowDebug = ((!*ShowDebug) != 0);
  }
}

static void UpdateCameras(Camera3D *PlayerCamera, Camera3D *FreeCamera,
                           bool *WasFreecam, bool HasControl,
                           Camera3D **ActiveCamera) {
  *ActiveCamera = PlayerCamera;

  if (GetDebugState()->Freecam) {
    if (!*WasFreecam) {
      *FreeCamera = *PlayerCamera;
    }
    UpdateCamera(FreeCamera, CAMERA_FREE);
    *ActiveCamera = FreeCamera;
  } else if (HasControl) {
    UpdateGameCamera(PlayerCamera, GetMouseDelta());
  }

  *WasFreecam = GetDebugState()->Freecam;
}

static void UpdateWorldChunks(World *WorldVal) {
  #pragma unroll 4
  for (int IdxI = 0; IdxI < WorldVal->ChunkCount; IdxI++) {
    Chunk *ChunkVal = &WorldVal->Chunks[IdxI];
    if (ChunkVal->IsGenerated && ChunkVal->TerrainJustGenerated) {
      UpdateNeighborsDirtyFlag(WorldVal, ChunkVal->ChunkX, ChunkVal->ChunkY, ChunkVal->ChunkZ);
      ChunkVal->TerrainJustGenerated = false;
    }
  }
}

static void BuildMeshes(World *WorldVal) {
  int MeshesBuiltThisFrame = 0;
  int MaxMeshesPerFrame = 2;

  #pragma unroll 4
  for (int IdxI = 0; IdxI < WorldVal->ChunkCount; IdxI++) {
    Chunk *ChunkVal = &WorldVal->Chunks[IdxI];

    if (ChunkVal->IsGenerated && ChunkVal->IsDirty && !ChunkVal->IsGenerating &&
        AreNeighborsGenerated(WorldVal, ChunkVal)) {
      BuildChunkMesh(WorldVal, ChunkVal);
      MeshesBuiltThisFrame++;

      if (MeshesBuiltThisFrame >= MaxMeshesPerFrame) {
        break;
      }
    }
  }
}

static void RenderGame(World *WorldVal, Player *PlayerVal, Camera3D *ActiveCamera,
                       ChatState *ChatVal, bool ShowDebug) {
  BeginDrawing();
  {
    ClearBackground(SKYBLUE);

    BeginMode3D(*ActiveCamera);
    {
      DrawWorld(WorldVal, *ActiveCamera);

      if (PlayerVal->TargetBlock.Hit) {
        DrawBlockHighlight(PlayerVal->TargetBlock.BlockPos);
      }

      DrawAABBDebug(WorldVal, PlayerVal);
    }
    EndMode3D();

    DrawHUD(PlayerVal, WorldVal, *ActiveCamera, ShowDebug);
    DrawChat(ChatVal);
  }
  EndDrawing();
}

int main(void) {
  World *WorldVal = (World *)0;
  Player PlayerVal;
  Camera3D PlayerCamera;
  Camera3D FreeCamera;
  bool WasFreecam = false;
  ChatState ChatVal;
  bool ShowDebug = false;

  InitGame(&WorldVal, &PlayerVal, &PlayerCamera, &ChatVal, &FreeCamera);

  while (!WindowShouldClose()) {
    float Dt = GetFrameTime();

    UpdateSystemInputs(&ShowDebug);

    Vector3 LoadCenter =
        (GetDebugState()->Freecam && WasFreecam) ? FreeCamera.position : PlayerVal.Position;
    UpdateWorld(WorldVal, LoadCenter, MAX_RENDER_DISTANCE);

    bool HasControl = (!ChatVal.IsActive) != 0;
    UpdatePlayer(&PlayerVal, &PlayerCamera, WorldVal, Dt, HasControl);

    Camera3D *ActiveCamera = (Camera3D *)0;
    UpdateCameras(&PlayerCamera, &FreeCamera, &WasFreecam, HasControl,
                  &ActiveCamera);

    UpdateChat(&ChatVal, ActiveCamera, &PlayerVal, WorldVal);
    HandlePlayerInteraction(&PlayerVal, ActiveCamera, WorldVal, HasControl);

    UpdateWorldChunks(WorldVal);
    BuildMeshes(WorldVal);

    RenderGame(WorldVal, &PlayerVal, ActiveCamera, &ChatVal, ShowDebug);
  }

  CleanupGame(WorldVal);
  return 0;
}
