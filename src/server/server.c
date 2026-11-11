#include "server/server.h"

#include "core/log.h"
#include "core/noise.h"
#include "core/profiler.h"
#include "core/vecmath.h"
#include "net/loopback.h"
#include "net/protocol.h"
#include "persistence/world_save.h"
#include "platform/os_time.h"
#include "world/biome.h"
#include "world/surface.h"
#include "world/cave_fields.h"
#include "world/terrain_shape.h"
#include "world/worldgen_strata.h"
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

static int BlockToChunkCoord(int BlockCoord) {
  return (BlockCoord >= 0) ? (BlockCoord / CHUNK_SIZE)
                           : -((CHUNK_SIZE - 1 - BlockCoord) / CHUNK_SIZE);
}

static bool IsChunkQueuedForResend(const ServerState *Server, int ChunkX,
                                   int ChunkY, int ChunkZ) {
  for (int Idx = 0; Idx < Server->PendingResendCount; Idx++) {
    const ServerChunkCoord *Coord = &Server->PendingResends[Idx];
    if (Coord->ChunkX == ChunkX && Coord->ChunkY == ChunkY &&
        Coord->ChunkZ == ChunkZ) {
      return true;
    }
  }
  return false;
}

static void QueueChunkResend(ServerState *Server, int ChunkX, int ChunkY,
                             int ChunkZ) {
  if (IsChunkQueuedForResend(Server, ChunkX, ChunkY, ChunkZ)) {
    return;
  }
  if (Server->PendingResendCount >= SERVER_MAX_CHUNK_RESENDS) {
    LogWarn("SERVER: chunk resend queue full; dropping chunk (%d, %d, %d)",
            ChunkX, ChunkY, ChunkZ);
    return;
  }
  Server->PendingResends[Server->PendingResendCount++] =
      (ServerChunkCoord){ChunkX, ChunkY, ChunkZ};
}

static void OnServerBlockChange(void *UserData, int BlockX, int BlockY,
                                int BlockZ, BlockID NewBlockId) {
  ServerState *Server = (ServerState *)UserData;
  if (Server->PendingDeltaCount < SERVER_MAX_DELTAS_PER_TICK) {
    Server->PendingDeltas[Server->PendingDeltaCount++] =
        (ServerBlockDelta){BlockX, BlockY, BlockZ, NewBlockId};
    return;
  }
  QueueChunkResend(Server, BlockToChunkCoord(BlockX),
                   BlockToChunkCoord(BlockY), BlockToChunkCoord(BlockZ));
}

void ServerBindLoopback(ServerState *Server, NetLoopback *Channel) {
  if (Server == (ServerState *)0) {
    return;
  }
  Server->Channel = Channel;
  Server->PendingDeltaCount = 0;
  Server->PendingResendCount = 0;
  if (Server->WorldVal != (World *)0) {
    bool Bound = (Channel != (NetLoopback *)0);
    WorldSetBlockChangeCallback(
        Server->WorldVal, Bound ? OnServerBlockChange : (BlockChangeCallback)0,
        Bound ? (void *)Server : NULL);
  }
}

// Movement axes and held buttons take the latest packet; edge actions latch
// until the tick consumes them, so a press is never lost between ticks.
static void FoldClientInput(ServerState *Server, const PktClientInput *Pkt) {
  Server->PendingInput = FoldPlayerInput(Server->PendingInput, Pkt->Input);

  if (Vec3Length(Pkt->Forward) > 0.0F) {
    Server->View.Forward = Vec3Normalize(Pkt->Forward);
  }
  if (Pkt->ClientTick > Server->LastProcessedClientTick) {
    Server->LastProcessedClientTick = Pkt->ClientTick;
  }
}

static void DrainClientPackets(ServerState *Server) {
  uint8_t Buf[NET_MAX_PACKET_BYTES];
  for (;;) {
    int Size = NetRecvFromClient(Server->Channel, Buf, (int)sizeof(Buf));
    if (Size == 0) {
      return;
    }
    if (Size < 0) {
      Server->DroppedClientPackets++;
      return;
    }
    PktClientInput Pkt = {0};
    if (NetDecodeClientInput(&Pkt, Buf, Size)) {
      FoldClientInput(Server, &Pkt);
    } else {
      Server->DroppedClientPackets++;
    }
  }
}

static void FlushBlockDeltas(ServerState *Server) {
  uint8_t Buf[NET_BLOCK_DELTA_BYTES];
  for (int Idx = 0; Idx < Server->PendingDeltaCount; Idx++) {
    const ServerBlockDelta *Delta = &Server->PendingDeltas[Idx];
    int ChunkX = BlockToChunkCoord(Delta->BlockX);
    int ChunkY = BlockToChunkCoord(Delta->BlockY);
    int ChunkZ = BlockToChunkCoord(Delta->BlockZ);
    if (IsChunkQueuedForResend(Server, ChunkX, ChunkY, ChunkZ)) {
      continue;
    }
    PktServerBlockDelta Pkt = {
        .BlockX = Delta->BlockX,
        .BlockY = Delta->BlockY,
        .BlockZ = Delta->BlockZ,
        .BlockId = Delta->BlockId,
    };
    int Size = NetEncodeBlockDelta(&Pkt, Buf, (int)sizeof(Buf));
    if (Size == 0 || !NetSendToClient(Server->Channel, Buf, Size)) {
      QueueChunkResend(Server, ChunkX, ChunkY, ChunkZ);
    }
  }
  Server->PendingDeltaCount = 0;
}

// Resends that cannot be queued stay pending for the next tick; chunks that
// have since unloaded are dropped.
static void FlushChunkResends(ServerState *Server) {
  int Kept = 0;
  for (int Idx = 0; Idx < Server->PendingResendCount; Idx++) {
    ServerChunkCoord Coord = Server->PendingResends[Idx];
    Chunk *ChunkVal = GetChunkFromWorld(Server->WorldVal, Coord.ChunkX,
                                        Coord.ChunkY, Coord.ChunkZ);
    if (ChunkVal == (Chunk *)0 || !ChunkVal->IsGenerated) {
      continue;
    }
    if (!ServerSendChunkToClient(Server, Coord.ChunkX, Coord.ChunkY,
                                 Coord.ChunkZ)) {
      Server->PendingResends[Kept++] = Coord;
    }
  }
  Server->PendingResendCount = Kept;
}

static void SendPlayerState(ServerState *Server) {
  const Player *PlayerVal = &Server->PlayerVal;
  PktServerPlayerState Pkt = {
      .ServerTick = Server->TickCount,
      .LastProcessedClientTick = Server->LastProcessedClientTick,
      .Position = PlayerVal->Position,
      .Velocity = PlayerVal->Velocity,
      .IsGrounded = PlayerVal->IsGrounded,
      .Noclip = PlayerVal->Noclip,
      .SelectedHotbarSlot = PlayerVal->SelectedHotbarSlot,
  };
  uint8_t Buf[NET_SERVER_PLAYER_STATE_BYTES];
  int Size = NetEncodeServerPlayerState(&Pkt, Buf, (int)sizeof(Buf));
  if (Size > 0) {
    (void)NetSendToClient(Server->Channel, Buf, Size);
  }
}

bool ServerSendChunkToClient(ServerState *Server, int ChunkX, int ChunkY,
                             int ChunkZ) {
  if (Server == (ServerState *)0 || Server->Channel == (NetLoopback *)0 ||
      Server->WorldVal == (World *)0) {
    return false;
  }
  Chunk *ChunkVal = GetChunkFromWorld(Server->WorldVal, ChunkX, ChunkY, ChunkZ);
  if (ChunkVal == (Chunk *)0 || !ChunkVal->IsGenerated ||
      ChunkVal->IsGenerating) {
    return false;
  }
  uint8_t Buf[NET_MAX_PACKET_BYTES];
  int Size = NetEncodeChunkData(ChunkVal, Buf, (int)sizeof(Buf));
  return Size > 0 && NetSendToClient(Server->Channel, Buf, Size);
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

bool ServerWaitForWorldSettle(ServerState *Server, double TimeoutSeconds) {
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

  InitTerrainShape();
  LoadTerrainNoise("assets/worldgen/noise.json");
  LoadTerrainShape("assets/worldgen/terrain_shape.json");
  InitStrata();
  LoadStrata("assets/worldgen/strata.json");
  InitCaveFields();
  LoadCaveNoise("assets/worldgen/noise.json");
  LoadCaveParams("assets/worldgen/caves.json");

  InitTerrainGeneration();

  if (Config->HasSeed) {
    WorldSaveSetSeedOverride(Config->Seed);
  }
  WorldSaveSetReadOnly(Config->ReadOnly);
  if (Config->ReadOnly) {
    LogInfo("SERVER: read-only mode enabled for this run");
  }
  InitWorldSave();
  // Every field is defined and the seed is final: prepare the fields for it
  // before the chunk workers start sampling.
  NoisePrepareSeed(GetWorldSeed());
  InitWorld(Server->WorldVal);

  int WorkerCount = (Config->WorkerCount > 0) ? Config->WorkerCount
                                              : CHUNK_WORKER_MAX_THREADS;
  SetChunkWorkerMeshRunner((MeshJobRunner)0);
  InitChunkWorker(WorkerCount);

  SpawnPoint Spawn = FindSpawnColumn(GetWorldSeed());
  LogInfo("SERVER: spawn column (%d, %d) surface %d", Spawn.X, Spawn.Z,
          Spawn.Surface);
  Server->PlayerVal = InitPlayer(Spawn.FeetPosition);
  Server->View = (PlayerView){
      .EyePosition = {Spawn.FeetPosition.x,
                      Spawn.FeetPosition.y + Server->PlayerVal.HeadOffset,
                      Spawn.FeetPosition.z},
      .Forward = {0.0F, -1.0F, 0.0F},
  };

  if (!ServerWaitForWorldSettle(Server, Config->SettleTimeoutSeconds)) {
    LogError("SERVER: timed out waiting for initial spawn chunks to generate");
    ServerShutdown(Server);
    return false;
  }

  return true;
}

void ServerStepTick(ServerState *Server) {
  if (Server->Channel != (NetLoopback *)0) {
    DrainClientPackets(Server);
  }

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

  Server->PendingInput = ClearPlayerInputEdges(Server->PendingInput);
  ServerUpdateWorldChunks(Server->WorldVal);
  Server->TickCount++;

  if (Server->Channel != (NetLoopback *)0) {
    FlushBlockDeltas(Server);
    FlushChunkResends(Server);
    SendPlayerState(Server);
  }
}

void ServerShutdown(ServerState *Server) {
  if (Server == (ServerState *)0 || Server->WorldVal == (World *)0) {
    return;
  }

  ServerBindLoopback(Server, (NetLoopback *)0);
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
