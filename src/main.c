#include "world/block_system.h"
#include "world/prefab.h"
#include "render/camera.h"
#include "ui/chat.h"
#include "world/chunk.h"
#include "world/chunk_worker.h"
#include "ui/debug.h"
#include "render/hud.h"
#include "input/input.h"
#include "platform/platform.h"
#include "player/player.h"
#include "pthread.h"
#include "render/renderer.h"
#include "core/log.h"
#include "world/world.h"
#include "persistence/world_save.h"
#include <stdbool.h>
#include <stdlib.h>

#define INITIAL_WIDTH 1280
#define INITIAL_HEIGHT 720

#define PLAYER_SPAWN_X 0.0F
#define PLAYER_SPAWN_Y 25.0F
#define PLAYER_SPAWN_Z 0.0F

#define TICK_RATE 20
#define TICK_DT (1.0F / (float)TICK_RATE)
#define MAX_TICKS_PER_FRAME 5
// Mesh-build budget per simulation tick. Throughput = MESHES_PER_TICK * TICK_RATE
// (~960/s at 20 TPS), constant regardless of render framerate.
#define MESHES_PER_TICK 48
// Hard ceiling on chunk meshes built (and GPU-uploaded) in a single frame.
// A slow frame runs several ticks, each with its own MESHES_PER_TICK budget; on
// a teleport that burst can be hundreds of synchronous uploads, hanging the GPU
// long enough to trigger a Windows TDR driver reset (crash). This cap bounds the
// per-frame upload burst without changing the steady tick-paced throughput.
#define MAX_MESHES_PER_FRAME 96

static void InitGame(World **WorldVal, Player *PlayerVal, GameCamera *PlayerCamera,
                     ChatState *ChatVal, GameCamera *FreeCamera) {
  PlatformInit(INITIAL_WIDTH, INITIAL_HEIGHT, "MineGame Beta 4");
  PlatformSetCursorDisabled(true);
  PlatformToggleFullscreen();

  InitRenderer();

  InitBlockRegistry();
  LoadAllBlockDefinitions("assets/blocks");

  InitPrefabRegistry();
  LoadAllPrefabs("assets/prefabs");

  *WorldVal = (World *)malloc(sizeof(World));
  InitWorldSave();
  InitWorld(*WorldVal);
  LogInfo("MAIN THREAD ID: %llu", (unsigned long long)pthread_self());
  InitChunkWorker();

  *PlayerVal =
      InitPlayer((Vec3){PLAYER_SPAWN_X, PLAYER_SPAWN_Y, PLAYER_SPAWN_Z});
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
  free(WorldVal);
  CloseRenderer();
  PlatformShutdown();
}

static void UpdateSystemInputs(bool *ShowDebug) {
  SystemInput System = PollSystemInput();

  if (System.FullscreenToggle) {
    PlatformToggleFullscreen();
  }

  if (System.DebugToggle) {
    *ShowDebug = ((!*ShowDebug) != 0);
  }
}

static void UpdateCameras(GameCamera *PlayerCamera, GameCamera *FreeCamera,
                           bool *WasFreecam, bool HasControl,
                           GameCamera **ActiveCamera, PlayerInput Move) {
  *ActiveCamera = PlayerCamera;

  if (GetDebugState()->Freecam) {
    if (!*WasFreecam) {
      *FreeCamera = *PlayerCamera;
    }
    UpdateFreeCamera(FreeCamera, InputGetLookDelta(), Move,
                     PlatformGetFrameTime());
    *ActiveCamera = FreeCamera;
  } else if (HasControl) {
    UpdateGameCamera(PlayerCamera, InputGetLookDelta());
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

// Build up to Budget chunk meshes; returns how many were built.
static int BuildMeshes(World *WorldVal, int Budget) {
  int MeshesBuilt = 0;
  if (Budget <= 0) {
    return 0;
  }

  #pragma unroll 4
  for (int IdxI = 0; IdxI < WorldVal->ChunkCount; IdxI++) {
    Chunk *ChunkVal = &WorldVal->Chunks[IdxI];

    if (ChunkVal->IsGenerated && ChunkVal->IsDirty && !ChunkVal->IsGenerating &&
        AreNeighborsGenerated(WorldVal, ChunkVal)) {
      BuildChunkMesh(WorldVal, ChunkVal);
      MeshesBuilt++;

      if (MeshesBuilt >= Budget) {
        break;
      }
    }
  }

  return MeshesBuilt;
}

static void RenderGame(World *WorldVal, Player *PlayerVal, GameCamera *ActiveCamera,
                       ChatState *ChatVal, bool ShowDebug) {
  RenderCamera Cam = {
      .Position = ActiveCamera->Position,
      .Target = ActiveCamera->Target,
      .Up = ActiveCamera->Up,
      .FovY = ActiveCamera->FovY,
  };

  RenderBeginFrame(Cam);
  {
    DrawWorld(WorldVal, Cam);

    if (PlayerVal->TargetBlock.Hit) {
      RenderDrawBlockHighlight(PlayerVal->TargetBlock.BlockPos);
    }

    DrawAABBDebug(WorldVal, PlayerVal);
  }
  RenderEnd3D();

  // 2D HUD layer (deferred; still drawn directly by Raylib helpers).
  DrawHUD(PlayerVal, WorldVal, *ActiveCamera, ShowDebug);
  DrawChat(ChatVal);

  RenderEndFrame();
}

// Fold a per-frame poll into the pending intent: held axes take the latest
// value, edge actions latch until consumed, scroll accumulates.
static PlayerInput AccumulateInput(PlayerInput Pending, PlayerInput Frame) {
  Pending.MoveX = Frame.MoveX;
  Pending.MoveForward = Frame.MoveForward;
  Pending.AscendHeld = Frame.AscendHeld;
  Pending.DescendHeld = Frame.DescendHeld;
  Pending.Jump = Pending.Jump || Frame.Jump;
  Pending.Break = Pending.Break || Frame.Break;
  Pending.Place = Pending.Place || Frame.Place;
  if (Frame.HotbarSelect >= 0) {
    Pending.HotbarSelect = Frame.HotbarSelect;
  }
  Pending.HotbarScroll += Frame.HotbarScroll;
  return Pending;
}

// Clear edge/scroll fields after a tick consumes the intent (keep held axes).
static PlayerInput ClearInputEdges(PlayerInput Pending) {
  Pending.Jump = false;
  Pending.Break = false;
  Pending.Place = false;
  Pending.HotbarSelect = -1;
  Pending.HotbarScroll = 0;
  return Pending;
}

int main(void) {
  World *WorldVal = (World *)0;
  Player PlayerVal;
  GameCamera PlayerCamera;
  GameCamera FreeCamera;
  bool WasFreecam = false;
  ChatState ChatVal;
  bool ShowDebug = false;

  InitGame(&WorldVal, &PlayerVal, &PlayerCamera, &ChatVal, &FreeCamera);

  PlayerInput PendingInput = {0};
  PendingInput.HotbarSelect = -1;
  float Accumulator = 0.0F;

  while (!PlatformShouldClose()) {
    UpdateSystemInputs(&ShowDebug);

    bool HasControl = (!ChatVal.IsActive) != 0;
    PendingInput = AccumulateInput(PendingInput, PollPlayerInput(HasControl));

    // Mouse-look and active-camera selection run per frame for responsiveness.
    GameCamera *ActiveCamera = (GameCamera *)0;
    UpdateCameras(&PlayerCamera, &FreeCamera, &WasFreecam, HasControl,
                  &ActiveCamera, PendingInput);

    // Fixed-timestep simulation: step the world at a constant rate regardless
    // of framerate, catching up via an accumulator (capped to avoid spiral).
    Accumulator += PlatformGetFrameTime();
    int Ticks = 0;
    int MeshesThisFrame = 0;
    while (Accumulator >= TICK_DT && Ticks < MAX_TICKS_PER_FRAME) {
      Vec3 LoadCenter = (GetDebugState()->Freecam && WasFreecam)
                            ? FreeCamera.Position
                            : PlayerVal.Position;
      UpdateWorld(WorldVal, LoadCenter, MAX_RENDER_DISTANCE);

      PlayerVal.PrevPosition = PlayerVal.Position;
      PlayerView MoveView = PlayerViewFromCamera(PlayerCamera);
      UpdatePlayer(&PlayerVal, WorldVal, MoveView, PendingInput, TICK_DT);

      PlayerView InteractView = PlayerViewFromCamera(*ActiveCamera);
      HandlePlayerInteraction(&PlayerVal, WorldVal, InteractView, PendingInput);

      PendingInput = ClearInputEdges(PendingInput);

      // Chunk bookkeeping and mesh building are tick-paced (bookkeeping first)
      // so loading throughput is independent of render framerate. The per-frame
      // ceiling clamps the per-tick budget during catch-up bursts (e.g. after a
      // teleport) so a slow frame never issues a GPU-hanging upload storm.
      UpdateWorldChunks(WorldVal);
      int MeshBudget = MAX_MESHES_PER_FRAME - MeshesThisFrame;
      if (MeshBudget > MESHES_PER_TICK) {
        MeshBudget = MESHES_PER_TICK;
      }
      MeshesThisFrame += BuildMeshes(WorldVal, MeshBudget);

      Accumulator -= TICK_DT;
      Ticks++;
    }
    if (Ticks >= MAX_TICKS_PER_FRAME) {
      Accumulator = 0.0F;
    }

    UpdateChat(&ChatVal, ActiveCamera, &PlayerVal, WorldVal);

    // Render interpolation: the camera follows the player position interpolated
    // between the previous and current tick, for smooth motion above tick rate.
    float Alpha = Accumulator / TICK_DT;
    Vec3 RenderPos = Vec3Lerp(PlayerVal.PrevPosition, PlayerVal.Position, Alpha);
    if (!GetDebugState()->Freecam) {
      CameraFollowTarget(&PlayerCamera, RenderPos, PlayerVal.HeadOffset);
    }

    RenderGame(WorldVal, &PlayerVal, ActiveCamera, &ChatVal, ShowDebug);
  }

  CleanupGame(WorldVal);
  return 0;
}
