#ifndef PLAYER_H
#define PLAYER_H

#include "raylib.h"
#include "world.h"

#define PLAYER_GRAVITY 20.0F
#define PLAYER_TERMINAL_VELOCITY 50.0F
#define PLAYER_JUMP_FORCE 7.0F
#define PLAYER_SIZE (Vector3){0.6F, 1.8F, 0.6F}
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
  RaycastResult targetBlock;

  Vector3 position;
  Vector3 velocity;
  Vector3 size;

  float reachDistance;
  float radius;
  float speed;
  float headOffset;
  int id;
  int selectedHotbarSlot;

  unsigned char hotbar[HOTBAR_SIZE];
  bool isGrounded;
  bool noclip;
  bool debug_aabb;
} Player ;

typedef struct {
  float radius;
  float yOffset;
  float epsilon;
} PointConfig;

Player InitPlayer(Vector3 spawnPos);
void UpdatePlayer(Player *player, Camera3D *camera, World *world, float dt, bool hasControl);
void HandlePlayerInteraction(Player *player, Camera3D *camera, World *world, bool hasControl);
bool IsPointSolid(World *world, Vector3 pos);
void GetPlayerPoints(Player *player, PointConfig config, Vector3 outPoints[COLLISION_POINTS]);
void SetHotbarSlot(Player *player, int slot, unsigned char blockID);

#endif
