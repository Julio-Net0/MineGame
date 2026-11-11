#include "automation/script.h"
#include "world/block_system.h"
#include "world/biome.h"
#include "world/prefab.h"
#include "world/surface.h"
#include "world/terrain_shape.h"
#include "world/worldgen_strata.h"
#include "render/camera.h"
#include "ui/chat.h"
#include "ui/commands.h"
#include "world/chunk.h"
#include "world/chunk_worker.h"
#include "ui/debug.h"
#include "ui/hud.h"
#include "ui/ui.h"
#include "ui/font.h"
#include "ui/pause_menu.h"
#include "ui/creative_inventory.h"
#include "input/input.h"
#include "platform/os_limits.h"
#include "platform/platform.h"
#include "player/player.h"
#include "pthread.h"
#include "render/backend.h"
#include "render/renderer.h"
#include "server/server.h"
#include "core/launch_options.h"
#include "core/log.h"
#include "core/profiler.h"
#include "core/resource_pack.h"
#include "world/world.h"
#include "persistence/world_save.h"
#include <stdbool.h>
#include <stdlib.h>


#define BYTES_PER_MEGABYTE (1024ULL * 1024ULL)

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

static void FreeEvictedChunkMeshes(Chunk *Evicted) {
  if (Evicted->HasMesh) {
    RenderFreeMesh(Evicted->ChunkMesh);
    Evicted->ChunkMesh = MESH_HANDLE_INVALID;
    Evicted->HasMesh = false;
  }
  if (Evicted->HasCutoutMesh) {
    RenderFreeMesh(Evicted->CutoutMesh);
    Evicted->CutoutMesh = MESH_HANDLE_INVALID;
    Evicted->HasCutoutMesh = false;
  }
  if (Evicted->HasTranslucentMesh) {
    RenderFreeMesh(Evicted->TranslucentMesh);
    Evicted->TranslucentMesh = MESH_HANDLE_INVALID;
    Evicted->HasTranslucentMesh = false;
  }
}

// Returns false when startup could not complete; nothing is initialised in that
// case, so the caller has nothing to tear down.
static bool InitGame(World **WorldVal, Player *PlayerVal, GameCamera *PlayerCamera,
                     ChatState *ChatVal, GameCamera *FreeCamera,
                     const LaunchOptions *Options) {
  // First, before the window and the registries: a failure here then costs no
  // teardown of a live GL context and no unloading of already-parsed assets.
  *WorldVal = (World *)malloc(sizeof(World));
  if (*WorldVal == (World *)0) {
    LogError("MAIN: out of memory allocating the world (%zu bytes)", sizeof(World));
    return false;
  }

  PlatformInit(Options->Window.Width, Options->Window.Height, "MineGame Beta 4");
  // A scripted run ignores the mouse, so capturing the cursor would only take it
  // away from whoever is watching the run.
  PlatformSetCursorDisabled(!ScriptIsActive());
  if (Options->Window.Fullscreen) { PlatformToggleFullscreen(); }

  InitBlockRegistry();
  LoadAllBlockDefinitions("assets/blocks");

  // After PlatformInit, which anchors the working directory the pack and base
  // paths are relative to; before InitRenderer, which loads block textures.
  ResourcePacksInit();
  InitRenderer();

  // Every module registers its GUI sprites before the atlas is packed.
  UiInit();
  InitHud();
  CreativeInventoryInit();
  FontInit();
  UiBuildAtlas();

  InitPrefabRegistry();
  LoadAllPrefabs("assets/prefabs");

  // After the blocks (biome palettes resolve block names against that registry)
  // and before InitChunkWorker (the workers sample the registry concurrently and
  // rely on it never being written again).
  InitBiomeRegistry();
  LoadBiomeParams("assets/biome_params.json");
  LoadAllBiomeDefinitions("assets/biomes");

  InitTerrainShape();
  LoadTerrainNoise("assets/worldgen/noise.json");
  LoadTerrainShape("assets/worldgen/terrain_shape.json");
  InitStrata();
  LoadStrata("assets/worldgen/strata.json");

  InitTerrainGeneration();

  // Before InitWorldSave, which creates the level metadata of a world that does
  // not exist yet: a scripted run must not write even that.
  if (Options->HasSeed) {
    WorldSaveSetSeedOverride(Options->Seed);
  }
  if (ScriptIsActive()) {
    WorldSaveSetReadOnly(true);
    LogInfo("MAIN: script mode; the world is read-only for this run");
  }
  InitWorldSave();
  InitWorld(*WorldVal);
  WorldSetChunkEvictCallback(*WorldVal, FreeEvictedChunkMeshes);
  LogInfo("MAIN THREAD ID: %llu", (unsigned long long)pthread_self());

  int WorkerCount = (Options->WorkerCount > 0) ? Options->WorkerCount
                                               : CHUNK_WORKER_MAX_THREADS;
  if (Options->WorkerCount > 0) {
    LogInfo("MAIN: simulation --workers %d", WorkerCount);
  }
#ifdef MINEGAME_PROFILE
  if (Options->CpuSlowdown > 1.0) {
    ProfilerSetSlowdown(Options->CpuSlowdown);
    LogInfo("MAIN: simulation --cpu-slowdown %.2f", Options->CpuSlowdown);
  }
#endif
  SetChunkWorkerMeshRunner(RendererBuildMeshJob);
  InitChunkWorker(WorkerCount);

  SpawnPoint Spawn = FindSpawnColumn(GetWorldSeed());
  LogInfo("MAIN: spawn column (%d, %d) surface %d", Spawn.X, Spawn.Z,
          Spawn.Surface);
  *PlayerVal = InitPlayer(Spawn.FeetPosition);
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
  UiShutdown();
  CloseRenderer();
  ResourcePacksShutdown();
  PlatformShutdown();

  size_t PeakBytes = OsGetPeakProcessMemory();
  if (PeakBytes > 0) {
    double PeakMb = (double)PeakBytes / (double)BYTES_PER_MEGABYTE;
    LogInfo("MAIN: peak process memory (%s): %.1f MB",
            OsGetPeakProcessMemoryKind(), PeakMb);
  }
}

static SystemInput UpdateSystemInputs(bool *ShowDebug, const ChatState *Chat,
                                      const Player *PlayerVal) {
  SystemInput System = PollSystemInput();

  // With chat open, ESC belongs to the chat, which closes on it this frame. An
  // open inventory takes ESC next, so it never stacks the pause menu over it.
  if (System.PauseToggle && !Chat->IsActive) {
    if (CreativeInventoryIsOpen()) {
      CreativeInventoryClose();
    } else {
      PauseMenuToggle();
    }
  } else if (System.InventoryToggle && !Chat->IsActive && !PauseMenuIsOpen()) {
    if (CreativeInventoryIsOpen()) {
      CreativeInventoryClose();
    } else {
      CreativeInventoryOpen(PlayerVal);
    }
  }

  if (System.FullscreenToggle) {
    PlatformToggleFullscreen();
  }

  if (System.DebugToggle) {
    *ShowDebug = ((!*ShowDebug) != 0);
  }
  return System;
}

static void UpdateCameras(GameCamera *PlayerCamera, GameCamera *FreeCamera,
                           bool *WasFreecam, bool HasControl,
                           GameCamera **ActiveCamera, PlayerInput Move,
                           Vec2 LookDelta) {
  *ActiveCamera = PlayerCamera;

  if (GetDebugState()->Freecam) {
    if (!*WasFreecam) {
      *FreeCamera = *PlayerCamera;
    }
    UpdateFreeCamera(FreeCamera, LookDelta, Move, PlatformGetFrameTime());
    *ActiveCamera = FreeCamera;
  } else if (HasControl) {
    UpdateGameCamera(PlayerCamera, LookDelta);
  }

  *WasFreecam = GetDebugState()->Freecam;
}

// Hand dirty chunks to the workers. Capturing the snapshot is the only cost the
// frame pays here; the mask scanning it feeds happens off-thread.
static void QueueMeshWork(World *WorldVal) {
  for (int IdxI = 0; IdxI < WorldVal->ChunkCount; IdxI++) {
    Chunk *ChunkVal = &WorldVal->Chunks[IdxI];

    // Already in flight: queueing it again would build the same chunk twice and
    // race two results onto one slot.
    if (ChunkVal->IsMeshing) {
      continue;
    }

    if (ChunkVal->IsGenerated && ChunkVal->IsDirty && !ChunkVal->IsGenerating &&
        AreNeighborsGenerated(WorldVal, ChunkVal)) {
      // The pool being full is the back-pressure that keeps the workers from
      // running arbitrarily far ahead of what the frame can upload.
      if (!RendererHasMeshJobCapacity()) {
        return;
      }

      int JobId = RendererQueueMeshJob(WorldVal, ChunkVal);
      if (JobId < 0) {
        continue;
      }
      if (!EnqueueChunkMeshJob(JobId)) {
        // The work queue refused it, so the slot must go back. Leaving it
        // claimed would strand it and leave the chunk's in-flight flag set,
        // which would keep it from ever being queued again.
        RendererAbandonMeshJob(JobId);
        return;
      }
    }
  }
}

// Upload finished meshes until the time budget is spent or the count allowance
// is used, whichever comes first. Called exactly once per rendered frame, which
// is what makes the minimum-one guarantee frame-scoped without tracking it: this
// call is the frame's only one.
static void UploadFinishedMeshes(int CountAllowance, double Deadline) {
  int Uploaded = 0;
  if (CountAllowance <= 0) {
    return;
  }

  while (RendererUploadFinishedMesh()) {
    Uploaded++;

    if (Uploaded >= CountAllowance) {
      break;
    }

    // Checked only after an upload, never before the first one. An upload is not
    // interruptible, so the budget can overrun by one; that is deliberate. On
    // hardware where a single upload costs more than the whole budget, testing
    // first would upload nothing and the world would never finish loading.
    if (PlatformGetTime() >= Deadline) {
      break;
    }
  }
}

static void RenderGame(World *WorldVal, Player *PlayerVal, GameCamera *ActiveCamera,
                       GameCamera *PlayerCamera, ChatState *ChatVal,
                       bool ShowDebug, const InventoryInput *InvInput) {
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

  UiContext *Ui = GetUiContext();
  PROFILE_BEGIN(UiBuildStart);
  PointerInput Pointer = PollPointerInput();
  UiPointer UiPointerPixels = {Pointer.Cursor.x, Pointer.Cursor.y,
                               Pointer.LeftPressed, Pointer.LeftDown,
                               Pointer.LeftReleased};
  // A scripted run ignores the real mouse here too, so the viewer cannot click
  // anything on an open screen.
  if (ScriptIsActive()) {
    UiPointerPixels = (UiPointer){.X = -1.0F, .Y = -1.0F};
  }
  UiBeginFrame(Ui, PlatformGetScreenWidth(), PlatformGetScreenHeight(),
               &UiPointerPixels);
  DrawHUD(Ui, PlayerVal, WorldVal, *ActiveCamera, ShowDebug);
  DrawCreativeInventory(Ui, InvInput);
  DrawChat(Ui, ChatVal);
  DrawPauseMenu(Ui, PlayerCamera);
  UiEndFrame(Ui);
  PROFILE_END(UiBuildStart, PROFILE_UI_BUILD);

  PROFILE_BEGIN(UiSubmitStart);
  RenderSubmitUI(&Ui->DrawList);
  PROFILE_END(UiSubmitStart, PROFILE_UI_SUBMIT);

  RenderEndFrame();
}

int main(int Argc, char **Argv) {
  World *WorldVal = (World *)0;
  Player PlayerVal;
  GameCamera PlayerCamera;
  GameCamera FreeCamera;
  bool WasFreecam = false;
  ChatState ChatVal;
  bool ShowDebug = false;

  ParseLaunchOptions(Argc, Argv);
  const LaunchOptions *Options = GetLaunchOptions();
  if (Options->CoreCount > 0) {
    int AppliedCores = 0;
    if (OsLimitPhysicalCores(Options->CoreCount, &AppliedCores)) {
      SetAppliedCoreCount(AppliedCores);
      LogInfo("MAIN: simulation --cores %d", AppliedCores);
    } else {
      SetAppliedCoreCount(0);
    }
  }
  if (Options->RamLimitMb > 0) {
    // Log before applying the cap so stdio and console buffers are committed
    // while the process is still unconstrained; otherwise a cap smaller than
    // the static .bss image prevents the subsequent out-of-memory log from
    // allocating its first console write buffer.
    LogInfo("MAIN: simulation --ram-limit %zu MB", Options->RamLimitMb);
    if (!OsLimitProcessMemory(Options->RamLimitMb * BYTES_PER_MEGABYTE)) {
      SetAppliedRamLimitMb(0);
    }
  }

  // Parsed before the window exists: a script that cannot be read or has a
  // syntax error then costs no startup at all, and the paths it was given still
  // resolve against the directory the run was launched from.
  if (Options->ScriptPath != (const char *)0 &&
      !ScriptLoad(Options->ScriptPath, Options->ScriptOutDir,
                  Options->ScriptTimeoutSeconds)) {
    int ExitCode = ScriptExitCode();
    ScriptShutdown();
    return ExitCode;
  }

  if (!InitGame(&WorldVal, &PlayerVal, &PlayerCamera, &ChatVal, &FreeCamera,
                Options)) {
    ScriptShutdown();
    return EXIT_FAILURE;
  }

  PlayerInput PendingInput = {0};
  PendingInput.HotbarSelect = -1;
  float Accumulator = 0.0F;
  GameCamera *ActiveCamera = &PlayerCamera;

  while (!PlatformShouldClose() && !PauseMenuQuitRequested() &&
         !ScriptRunEnded()) {
    // Before input is polled, so a statement takes effect on the frame it is
    // reached rather than the next one.
    ScriptStep(&ChatVal, ActiveCamera, &PlayerVal, WorldVal);

    // Real input is ignored for the duration of a scripted run, so a key or a
    // mouse movement from whoever is watching cannot change its result. The
    // window's close button still ends the process.
    bool ScriptDriven = ScriptIsActive();
    InventoryInput InvInput = {.WheelDelta = 0, .HotbarKey = -1};
    if (!ScriptDriven) {
      SystemInput System = UpdateSystemInputs(&ShowDebug, &ChatVal, &PlayerVal);
      if (CreativeInventoryIsOpen()) {
        InvInput.WheelDelta = InputGetScrollDelta();
        InvInput.HotbarKey = System.HotbarKey;
      }
    }

    // An open menu or chat owns the keyboard and mouse, so nothing typed or
    // clicked there also moves the player or breaks a block.
    bool HasControl = !ChatVal.IsActive && !PauseMenuIsOpen() &&
                      !CreativeInventoryIsOpen() && !ScriptDriven;
    PendingInput = FoldPlayerInput(PendingInput, PollPlayerInput(HasControl));

    // A script's intent is folded in the same way the polled intent is, so it
    // reaches the simulation through the one path and exercises the same
    // movement, collision and interaction code a player would.
    if (ScriptDriven) {
      PendingInput = FoldPlayerInput(PendingInput, ScriptPollInput());
    }

    // The inventory edits a desired hotbar; the edit reaches the player only as
    // intent, so the simulation validates it like any other request.
    BlockID RequestedHotbar[HOTBAR_SIZE];
    if (CreativeInventoryTakeHotbarRequest(RequestedHotbar)) {
      PendingInput.HotbarSetRequested = true;
      for (int Slot = 0; Slot < HOTBAR_SIZE; Slot++) {
        PendingInput.HotbarSet[Slot] = RequestedHotbar[Slot];
      }
    }

    // Mouse-look and active-camera selection run per frame for responsiveness.
    Vec2 LookDelta = ScriptDriven ? (Vec2){0.0F, 0.0F} : InputGetLookDelta();
    UpdateCameras(&PlayerCamera, &FreeCamera, &WasFreecam, HasControl,
                  &ActiveCamera, PendingInput, LookDelta);

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
      if (!GetDebugState()->Freecam) {
        PlayerView MoveView = PlayerViewFromCamera(PlayerCamera);
        UpdatePlayer(&PlayerVal, WorldVal, MoveView, PendingInput, TICK_DT);
      }

      PlayerView InteractView = PlayerViewFromCamera(*ActiveCamera);
      HandlePlayerInteraction(&PlayerVal, WorldVal, InteractView, PendingInput);
      PROFILE_END(PlayerStart, PROFILE_PLAYER);

      PendingInput = ClearPlayerInputEdges(PendingInput);

      // Neighbour-dirty bookkeeping stays tick-paced: it reacts to terrain the
      // workers finished, which is simulation state, and running it per frame
      // would repeat a full chunk scan for nothing. Mesh building runs after the
      // tick loop instead — see below.
      ServerUpdateWorldChunks(WorldVal);

      Accumulator -= TICK_DT;
      Ticks++;
    }
    if (Ticks >= MAX_TICKS_PER_FRAME) {
      Accumulator = 0.0F;
    }
    ScriptRecordTicks(Ticks);

    // Once per frame, after the ticks so it sees this frame's bookkeeping.
    // Queueing costs the frame only the snapshot captures; the mask scanning
    // runs on the workers. The budget now bounds the GPU uploads, which are the
    // only part that must stay on this thread.
    QueueMeshWork(WorldVal);
    UploadFinishedMeshes(MAX_MESHES_PER_FRAME,
                         PlatformGetTime() + MESH_BUDGET_SECONDS);

    if (!PauseMenuIsOpen()) {
      UpdateChat(&ChatVal, ActiveCamera, &PlayerVal, WorldVal);
    }
    PollCommandEvents(&ChatVal);

    // Render interpolation: the camera follows the player position interpolated
    // between the previous and current tick, for smooth motion above tick rate.
    float Alpha = Accumulator / TICK_DT;
    Vec3 RenderPos = Vec3Lerp(PlayerVal.PrevPosition, PlayerVal.Position, Alpha);
    if (!GetDebugState()->Freecam) {
      CameraFollowTarget(&PlayerCamera, RenderPos, PlayerVal.HeadOffset);
    }

    RenderGame(WorldVal, &PlayerVal, ActiveCamera, &PlayerCamera, &ChatVal,
               ShowDebug, &InvInput);
    // The pause menu's FOV slider edits the player camera; the free camera
    // follows so switching to it keeps the chosen field of view.
    FreeCamera.FovY = PlayerCamera.FovY;

    // After rendering, so the frame's own work is fully accounted for before
    // its samples are folded into the profiler's window.
    PROFILE_FRAME_END();
    ScriptEndFrame();
  }

  // Shut the runner down first: it decides the exit code of a run the loop left
  // for a reason of its own, such as the window being closed.
  ScriptShutdown();
  int ExitCode = ScriptExitCode();
  CleanupGame(WorldVal);
  return ExitCode;
}
