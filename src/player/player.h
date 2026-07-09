#ifndef PLAYER_H
#define PLAYER_H

#include "world/world.h"
#include "core/vecmath.h"

#define PLAYER_GRAVITY 20.0F
#define PLAYER_TERMINAL_VELOCITY 50.0F
#define PLAYER_JUMP_FORCE 7.0F
#define PLAYER_SIZE (Vec3){0.6F, 1.8F, 0.6F}
#define PLAYER_RADIUS (PLAYER_SIZE.x / 2.0F)
#define PLAYER_SPEED 5.0F
#define HOTBAR_SIZE 9
#define PLAYER_HEAD_OFFSET 1.6F
#define PLAYER_DEBUG_AABB_SQUARES_SIZE 0.05F
#define PLAYER_DEBUG_AABB_WIRES_SIZE (PLAYER_DEBUG_AABB_SQUARES_SIZE * 1.5F)
#define PLAYER_REACH_DISTANCE 10.0F

#define COLLISION_POINTS 5
#define COLLISION_EPSILON 0.01F
#define AABB_OFFSET_MARGIN 0.001F
#define VERTICAL_RADIUS_SHRINK 0.05F
#define LATERAL_Y_MARGIN 0.1F

typedef struct {
  RaycastResult TargetBlock;

  Vec3 Position;
  Vec3 PrevPosition; // tick position before the current tick, for render interpolation
  Vec3 Velocity;
  Vec3 Size;

  float ReachDistance;
  float Radius;
  float Speed;
  float HeadOffset;
  int Id;
  int SelectedHotbarSlot;

  unsigned char Hotbar[HOTBAR_SIZE];
  bool IsGrounded;
  bool Noclip;
} Player;

typedef struct {
  float Radius;
  float YOffset;
  float Epsilon;
} PointConfig;

// Abstract player input intent. The client produces it; the simulation
// consumes it. This is the client->server command contract for multiplayer.
typedef struct {
  float MoveX;       // strafe axis: -1 (left/A) .. +1 (right/D)
  float MoveForward; // forward axis: -1 (back/S) .. +1 (forward/W)
  bool Jump;         // edge: jump requested this tick
  bool AscendHeld;   // held: noclip ascend
  bool DescendHeld;  // held: noclip descend
  int HotbarSelect;  // -1 = none, else 0..HOTBAR_SIZE-1 (edge)
  int HotbarScroll;  // hotbar scroll delta this tick
  bool Break;        // edge: break targeted block
  bool Place;        // edge: place block
} PlayerInput;

// Abstract view the simulation uses for movement basis and the interaction
// ray, replacing a renderer Camera3D.
typedef struct {
  Vec3 EyePosition;
  Vec3 Forward;
} PlayerView;

Player InitPlayer(Vec3 SpawnPos);
void UpdatePlayer(Player *PlayerVal, World *WorldVal, PlayerView View, PlayerInput Input, float Dt);
void HandlePlayerInteraction(Player *PlayerVal, World *WorldVal, PlayerView View, PlayerInput Input);
bool IsPointSolid(World *WorldVal, Vec3 Pos);
void GetPlayerPoints(Player *PlayerVal, PointConfig Config, Vec3 OutPoints[COLLISION_POINTS]);
void SetHotbarSlot(Player *PlayerVal, int Slot, unsigned char BlockId);
void SetPlayerPosition(Player *PlayerVal, Vec3 Pos);

#endif
