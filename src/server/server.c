#include "server/server.h"

#include "core/log.h"
#include "core/profiler.h"
#include "core/vecmath.h"
#include "net/loopback.h"
#include "net/protocol.h"
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
                                int BlockZ, unsigned char NewBlockId) {
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
  PlayerInput *Pending = &Server->PendingInput;
  const PlayerInput *Incoming = &Pkt->Input;
  Pending->MoveX = Incoming->MoveX;
  Pending->MoveForward = Incoming->MoveForward;
  Pending->AscendHeld = Incoming->AscendHeld;
  Pending->DescendHeld = Incoming->DescendHeld;
  Pending->Jump = Pending->Jump || Incoming->Jump;
  Pending->Break = Pending->Break || Incoming->Break;
  Pending->Place = Pending->Place || Incoming->Place;
  if (Incoming->HotbarSelect >= 0) {
    Pending->HotbarSelect = Incoming->HotbarSelect;
  }
  Pending->HotbarScroll += Incoming->HotbarScroll;

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

  Server->PendingInput = ClearServerInputEdges(Server->PendingInput);
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
