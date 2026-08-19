#include "world/block_system.h"
#include "world/biome.h"
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
#include "core/profiler.h"
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
// Wall-clock budget for mesh building, spent once per rendered frame.
//
// Measured on the low-end target: at 152 FPS with a 20 Hz tick, running this
// inside the tick loop meant mesh building happened on 13% of frames, capping
// loading at ~30 chunks/s and taking 44 s to fill the render distance. The
// budget is expressed per frame, so it is spent per frame; that alone took the
// same hardware to ~160 chunks/s and ~8 s, at 80 FPS while loading instead of
// 170 idle. Loading throughput therefore scales with framerate now, which is
// the intended reading of a per-frame budget: it caps the share of each frame
// spent meshing, and a machine with more frames has more capacity to spend.
//
// A chunk count is not a budget: it bounds work only in units of "chunks", and
// a chunk costs whatever the hardware makes it cost. Measured, building one
// chunk mesh costs ~660 us on the development machine and ~2730 us on the
// low-end target, so the count of 96 that fit one frame on the first took
// 217 ms on the second — the sustained 5 FPS this constant replaces. Time is
// the unit the constraint is actually expressed in, so the same value is right
// on every machine; it changes how fast the world loads, not whether the frame
// survives.
#define MESH_BUDGET_MS 4.0
#define MESH_BUDGET_SECONDS (MESH_BUDGET_MS / 1000.0)

// Hard ceiling on chunk meshes built (and GPU-uploaded) in a single frame, kept
// alongside the time budget rather than replaced by it. The two bound different
// things: the time budget bounds how long the frame takes, which is what the
// player feels, while this bounds how many synchronous GPU uploads a frame can
// issue — a burst large enough to hang the GPU triggers a Windows TDR driver
// reset. On fast hardware many uploads fit inside a few milliseconds, so the
// time budget alone would leave that hazard open. Mesh building stops at
// whichever limit is reached first.
#define MAX_MESHES_PER_FRAME 96

// Returns false when startup could not complete; nothing is initialised in that
// case, so the caller has nothing to tear down.
static bool InitGame(World **WorldVal, Player *PlayerVal, GameCamera *PlayerCamera,
                     ChatState *ChatVal, GameCamera *FreeCamera) {
  // First, before the window and the registries: a failure here then costs no
  // teardown of a live GL context and no unloading of already-parsed assets.
  *WorldVal = (World *)malloc(sizeof(World));
  if (*WorldVal == (World *)0) {
    LogError("MAIN: out of memory allocating the world (%zu bytes)", sizeof(World));
    return false;
  }

  PlatformInit(INITIAL_WIDTH, INITIAL_HEIGHT, "MineGame Beta 4");
  PlatformSetCursorDisabled(true);
  PlatformToggleFullscreen();

  InitRenderer();

  InitBlockRegistry();
  LoadAllBlockDefinitions("assets/blocks");

  InitPrefabRegistry();
  LoadAllPrefabs("assets/prefabs");

  // After the blocks (biome palettes resolve block names against that registry)
  // and before InitChunkWorker (the workers sample the registry concurrently and
  // rely on it never being written again).
  InitBiomeRegistry();
  LoadBiomeParams("assets/biome_params.json");
  LoadAllBiomeDefinitions("assets/biomes");

  InitTerrainGeneration();

  InitWorldSave();
  InitWorld(*WorldVal);
  LogInfo("MAIN THREAD ID: %llu", (unsigned long long)pthread_self());
  InitChunkWorker();

  *PlayerVal =
      InitPlayer((Vec3){PLAYER_SPAWN_X, PLAYER_SPAWN_Y, PLAYER_SPAWN_Z});
  *PlayerCamera = CreateGameCamera();
  *FreeCamera = CreateGameCamera();
  InitChat(ChatVal);

  return true;
}

static void CleanupGame(World *WorldVal) {
  CloseChunkWorker();

  // Sweep and save all modified chunks
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
  for (int IdxI = 0; IdxI < WorldVal->ChunkCount; IdxI++) {
    Chunk *ChunkVal = &WorldVal->Chunks[IdxI];
    if (ChunkVal->IsGenerated && ChunkVal->TerrainJustGenerated) {
      UpdateNeighborsDirtyFlag(WorldVal, ChunkVal->ChunkX, ChunkVal->ChunkY, ChunkVal->ChunkZ);
      ChunkVal->TerrainJustGenerated = false;
    }
  }
}

// Build chunk meshes until the time budget is spent or the count allowance is
// used, whichever comes first. Called exactly once per rendered frame, which is
// what makes the minimum-one-build guarantee frame-scoped without needing to be
// tracked: this call is the frame's only one.
static void BuildMeshes(World *WorldVal, int CountAllowance, double Deadline) {
  int MeshesBuilt = 0;
  if (CountAllowance <= 0) {
    return;
  }

  for (int IdxI = 0; IdxI < WorldVal->ChunkCount; IdxI++) {
    Chunk *ChunkVal = &WorldVal->Chunks[IdxI];

    if (ChunkVal->IsGenerated && ChunkVal->IsDirty && !ChunkVal->IsGenerating &&
        AreNeighborsGenerated(WorldVal, ChunkVal)) {
      BuildChunkMesh(WorldVal, ChunkVal);
      MeshesBuilt++;

      if (MeshesBuilt >= CountAllowance) {
        break;
      }

      // Checked only after a chunk is built, never before the first one. A
      // chunk is not interruptible, so the budget can overrun by one chunk's
      // cost; that is deliberate. On hardware where a single chunk costs more
      // than the whole budget, testing first would build nothing at all and the
      // world would never finish loading. Overrunning by one chunk degrades
      // gracefully, building nothing does not.
      if (PlatformGetTime() >= Deadline) {
        break;
      }
    }
  }
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

    DrawPrefabSelection(PlayerVal);
    DrawAABBDebug(WorldVal, PlayerVal);
  }
  RenderEnd3D();

  // 2D HUD layer (deferred; still drawn directly by Raylib helpers).
  PROFILE_BEGIN(HudStart);
  DrawHUD(PlayerVal, WorldVal, *ActiveCamera, ShowDebug);
  DrawChat(ChatVal);
  PROFILE_END(HudStart, PROFILE_HUD);

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

  if (!InitGame(&WorldVal, &PlayerVal, &PlayerCamera, &ChatVal, &FreeCamera)) {
    return EXIT_FAILURE;
  }

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
    while (Accumulator >= TICK_DT && Ticks < MAX_TICKS_PER_FRAME) {
      Vec3 LoadCenter = (GetDebugState()->Freecam && WasFreecam)
                            ? FreeCamera.Position
                            : PlayerVal.Position;
      UpdateWorld(WorldVal, LoadCenter, MAX_RENDER_DISTANCE);

      PROFILE_BEGIN(PlayerStart);
      PlayerVal.PrevPosition = PlayerVal.Position;
      PlayerView MoveView = PlayerViewFromCamera(PlayerCamera);
      UpdatePlayer(&PlayerVal, WorldVal, MoveView, PendingInput, TICK_DT);

      PlayerView InteractView = PlayerViewFromCamera(*ActiveCamera);
      HandlePlayerInteraction(&PlayerVal, WorldVal, InteractView, PendingInput);
      PROFILE_END(PlayerStart, PROFILE_PLAYER);

      PendingInput = ClearInputEdges(PendingInput);

      // Neighbour-dirty bookkeeping stays tick-paced: it reacts to terrain the
      // workers finished, which is simulation state, and running it per frame
      // would repeat a full chunk scan for nothing. Mesh building runs after the
      // tick loop instead — see below.
      UpdateWorldChunks(WorldVal);

      Accumulator -= TICK_DT;
      Ticks++;
    }
    if (Ticks >= MAX_TICKS_PER_FRAME) {
      Accumulator = 0.0F;
    }

    // Once per frame, after the ticks so it sees this frame's bookkeeping. The
    // deadline is taken here rather than before the tick loop so the budget
    // measures meshing alone and is not eaten by the simulation ahead of it.
    BuildMeshes(WorldVal, MAX_MESHES_PER_FRAME,
                PlatformGetTime() + MESH_BUDGET_SECONDS);

    UpdateChat(&ChatVal, ActiveCamera, &PlayerVal, WorldVal);

    // Render interpolation: the camera follows the player position interpolated
    // between the previous and current tick, for smooth motion above tick rate.
    float Alpha = Accumulator / TICK_DT;
    Vec3 RenderPos = Vec3Lerp(PlayerVal.PrevPosition, PlayerVal.Position, Alpha);
    if (!GetDebugState()->Freecam) {
      CameraFollowTarget(&PlayerCamera, RenderPos, PlayerVal.HeadOffset);
    }

    RenderGame(WorldVal, &PlayerVal, ActiveCamera, &ChatVal, ShowDebug);

    // After rendering, so the frame's own work is fully accounted for before
    // its samples are folded into the profiler's window.
    PROFILE_FRAME_END();
  }

  CleanupGame(WorldVal);
  return 0;
}
