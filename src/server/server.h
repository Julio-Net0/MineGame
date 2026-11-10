#ifndef SERVER_SERVER_H
#define SERVER_SERVER_H

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

typedef struct {
  World *WorldVal;
  Player PlayerVal;
  PlayerView View;
  PlayerInput PendingInput;
  int RenderDistance;
  uint64_t TickCount;
} ServerState;

// Initialise registries, persistence, chunk workers, world, and player, then
// wait until initial chunk generation around the spawn position settles.
// Returns false on allocation failure or if initial generation times out.
bool ServerInit(ServerState *Server, const ServerConfig *Config);

// Clear TerrainJustGenerated flags and mark neighbour chunks dirty on any
// chunks the worker threads finished since the last scan.
void ServerUpdateWorldChunks(World *WorldVal);

// Advance the authoritative world and player simulation by one 20 TPS tick
// (SERVER_TICK_DT = 0.05 s) and consume edge/scroll fields of PendingInput.
void ServerStepTick(ServerState *Server);

// Stop workers, sweep and save modified chunks (when not read-only), and free
// the world allocation.
void ServerShutdown(ServerState *Server);

#endif
