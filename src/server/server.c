#include "server/server.h"

#include "core/log.h"
#include "core/profiler.h"
#include "persistence/world_save.h"
#include "platform/os_time.h"
#include "world/biome.h"
#include "world/block_system.h"
#include "world/chunk.h"
#include "world/chunk_worker.h"
#include "world/prefab.h"
#include "world/world.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

enum {
  SETTLE_POLL_SLEEP_MS = 2U
};

#define DEFAULT_SETTLE_TIMEOUT_SECONDS 60.0

static PlayerInput ClearServerInputEdges(PlayerInput Pending) {
  Pending.Jump = false;
  Pending.Break = false;
  Pending.Place = false;
  Pending.HotbarSelect = -1;
  Pending.HotbarScroll = 0;
  return Pending;
}

void ServerUpdateWorldChunks(World *WorldVal) {
  for (int IdxI = 0; IdxI < WorldVal->ChunkCount; IdxI++) {
    Chunk *ChunkVal = &WorldVal->Chunks[IdxI];
    if (ChunkVal->IsGenerated && ChunkVal->TerrainJustGenerated) {
      UpdateNeighborsDirtyFlag(WorldVal, ChunkVal->ChunkX, ChunkVal->ChunkY,
                               ChunkVal->ChunkZ);
      ChunkVal->TerrainJustGenerated = false;
    }
  }
}

static bool WaitForInitialWorldSettle(ServerState *Server,
                                      double TimeoutSeconds) {
  double Timeout = (TimeoutSeconds > 0.0) ? TimeoutSeconds
                                          : DEFAULT_SETTLE_TIMEOUT_SECONDS;
  double Deadline = OsGetTimeSeconds() + Timeout;

  UpdateWorld(Server->WorldVal, Server->PlayerVal.Position,
              Server->RenderDistance);

  for (;;) {
    ServerUpdateWorldChunks(Server->WorldVal);
    if (WorldIsGenerationSettled(Server->WorldVal, Server->PlayerVal.Position,
                                 Server->RenderDistance)) {
      return true;
    }
    if (OsGetTimeSeconds() >= Deadline) {
      return false;
    }
    OsSleepMillis(SETTLE_POLL_SLEEP_MS);
  }
}

bool ServerInit(ServerState *Server, const ServerConfig *Config) {
  if (Server == (ServerState *)0 || Config == (const ServerConfig *)0) {
    return false;
  }

  *Server = (ServerState){0};
  Server->PendingInput.HotbarSelect = -1;
  Server->RenderDistance = (Config->RenderDistance > 0 &&
                            Config->RenderDistance <= MAX_RENDER_DISTANCE)
                               ? Config->RenderDistance
                               : MAX_RENDER_DISTANCE;

  Server->WorldVal = (World *)malloc(sizeof(World));
  if (Server->WorldVal == (World *)0) {
    LogError("SERVER: out of memory allocating world (%zu bytes)",
             sizeof(World));
    return false;
  }

  (void)OsAnchorWorkingDirectoryToExecutable();

  InitBlockRegistry();
  LoadAllBlockDefinitions("assets/blocks");

  InitPrefabRegistry();
  LoadAllPrefabs("assets/prefabs");

  InitBiomeRegistry();
  LoadBiomeParams("assets/biome_params.json");
  LoadAllBiomeDefinitions("assets/biomes");

  InitTerrainGeneration();

  if (Config->HasSeed) {
    WorldSaveSetSeedOverride(Config->Seed);
  }
  WorldSaveSetReadOnly(Config->ReadOnly);
  if (Config->ReadOnly) {
    LogInfo("SERVER: read-only mode enabled for this run");
  }
  InitWorldSave();
  InitWorld(Server->WorldVal);

  int WorkerCount = (Config->WorkerCount > 0) ? Config->WorkerCount
                                              : CHUNK_WORKER_MAX_THREADS;
  SetChunkWorkerMeshRunner((MeshJobRunner)0);
  InitChunkWorker(WorkerCount);

  Server->PlayerVal = InitPlayer(
      (Vec3){SERVER_SPAWN_X, SERVER_SPAWN_Y, SERVER_SPAWN_Z});
  Server->View = (PlayerView){
      .EyePosition = {SERVER_SPAWN_X,
                      SERVER_SPAWN_Y + Server->PlayerVal.HeadOffset,
                      SERVER_SPAWN_Z},
      .Forward = {0.0F, -1.0F, 0.0F},
  };

  if (!WaitForInitialWorldSettle(Server, Config->SettleTimeoutSeconds)) {
    LogError("SERVER: timed out waiting for initial spawn chunks to generate");
    ServerShutdown(Server);
    return false;
  }

  return true;
}

void ServerStepTick(ServerState *Server) {
  UpdateWorld(Server->WorldVal, Server->PlayerVal.Position,
              Server->RenderDistance);

  PROFILE_BEGIN(PlayerStart);
  Server->PlayerVal.PrevPosition = Server->PlayerVal.Position;
  Server->View.EyePosition = (Vec3){
      Server->PlayerVal.Position.x,
      Server->PlayerVal.Position.y + Server->PlayerVal.HeadOffset,
      Server->PlayerVal.Position.z,
  };
  UpdatePlayer(&Server->PlayerVal, Server->WorldVal, Server->View,
               Server->PendingInput, SERVER_TICK_DT);
  HandlePlayerInteraction(&Server->PlayerVal, Server->WorldVal, Server->View,
                          Server->PendingInput);
  PROFILE_END(PlayerStart, PROFILE_PLAYER);

  Server->PendingInput = ClearServerInputEdges(Server->PendingInput);
  ServerUpdateWorldChunks(Server->WorldVal);
  Server->TickCount++;
}

void ServerShutdown(ServerState *Server) {
  if (Server == (ServerState *)0 || Server->WorldVal == (World *)0) {
    return;
  }

  CloseChunkWorker();

  for (int IdxI = 0; IdxI < Server->WorldVal->ChunkCount; IdxI++) {
    if (Server->WorldVal->Chunks[IdxI].IsModified) {
      SaveChunkToDisk(&Server->WorldVal->Chunks[IdxI]);
    }
  }

  CloseWorldSave();
  free(Server->WorldVal);
  Server->WorldVal = (World *)0;
}
