#ifndef SERVER_SERVER_H
#define SERVER_SERVER_H

#include "net/loopback.h"
#include "player/player.h"
#include "world/world.h"
#include <stdbool.h>
#include <stdint.h>

#define SERVER_TICK_RATE 20
#define SERVER_TICK_DT (1.0F / (float)SERVER_TICK_RATE)
#define SERVER_TICK_MS 50U

#define SERVER_SPAWN_X 0.0F
#define SERVER_SPAWN_Y 25.0F
#define SERVER_SPAWN_Z 0.0F

typedef struct {
  int WorkerCount;
  int RenderDistance;
  bool HasSeed;
  uint64_t Seed;
  bool ReadOnly;
  double SettleTimeoutSeconds;
} ServerConfig;

// Block changes buffered per tick before they become PKT_SERVER_BLOCK_DELTA
// packets; beyond this the affected chunk is resent whole instead.
#define SERVER_MAX_DELTAS_PER_TICK 256
#define SERVER_MAX_CHUNK_RESENDS 64

typedef struct {
  int BlockX;
  int BlockY;
  int BlockZ;
  unsigned char BlockId;
} ServerBlockDelta;

typedef struct {
  int ChunkX;
  int ChunkY;
  int ChunkZ;
} ServerChunkCoord;

typedef struct {
  World *WorldVal;
  Player PlayerVal;
  PlayerView View;
  PlayerInput PendingInput;
  int RenderDistance;
  uint64_t TickCount;

  NetLoopback *Channel;
  uint64_t LastProcessedClientTick;
  uint64_t DroppedClientPackets;

  ServerBlockDelta PendingDeltas[SERVER_MAX_DELTAS_PER_TICK];
  int PendingDeltaCount;
  ServerChunkCoord PendingResends[SERVER_MAX_CHUNK_RESENDS];
  int PendingResendCount;
} ServerState;

// Initialise registries, persistence, chunk workers, world, and player, then
// wait until initial chunk generation around the spawn position settles.
// Returns false on allocation failure or if initial generation times out.
bool ServerInit(ServerState *Server, const ServerConfig *Config);

// Clear TerrainJustGenerated flags and mark neighbour chunks dirty on any
// chunks the worker threads finished since the last scan.
void ServerUpdateWorldChunks(World *WorldVal);

// Attach a transport (or detach with NULL). While attached, ServerStepTick
// consumes PKT_CLIENT_INPUT packets and emits block deltas, chunk resends and
// player state, and world block changes are buffered for delivery.
void ServerBindLoopback(ServerState *Server, NetLoopback *Channel);

// Advance the authoritative world and player simulation by one 20 TPS tick
// (SERVER_TICK_DT = 0.05 s) and consume edge/scroll fields of PendingInput.
void ServerStepTick(ServerState *Server);

// Encode the loaded, generated chunk at the given chunk coordinates as
// PKT_SERVER_CHUNK_DATA and queue it to the client. Returns false when no
// channel is bound, the chunk is unavailable, or the queue is full.
bool ServerSendChunkToClient(ServerState *Server, int ChunkX, int ChunkY,
                             int ChunkZ);

// Stop workers, sweep and save modified chunks (when not read-only), and free
// the world allocation.
void ServerShutdown(ServerState *Server);

#endif
