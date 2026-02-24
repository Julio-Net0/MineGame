#ifndef PLAYER_H
#define PLAYER_H

#include "raylib.h"
#include "world.h"

#define PLAYER_GRAVITY 20.0F
#define PLAYER_TERMINAL_VELOCITY 50.0F
#define PLAYER_JUMP_FORCE 7.0F
#define PLAYER_SIZE (Vector3){0.6F, 1.8F, 0.6F}
#define PLAYER_RADIUS PLAYER_SIZE.x / 2.0F
#define PLAYER_SPEED 5.0F
#define PLAYER_HEAD_OFFSET 1.6F
#define PLAYER_DEBUG_AABB_SQUARES_SIZE 0.05F
#define PLAYER_DEBUG_AABB_WIRES_SIZE PLAYER_DEBUG_AABB_SQUARES_SIZE * 1.5F;

typedef struct {
  int id;

  Vector3 position;
  Vector3 velocity;
  Vector3 size;

  float radius;
  float speed;
  float headOffset;

  bool isGrounded;
  bool noclip;

  bool debug_aabb;

} Player ;

Player InitPlayer(Vector3 spawnPos);

void UpdatePlayer(Player *player, Camera3D *camera, Chunk *chunk, float dt);

void DrawPlayerDebug(Chunk *chunk, Player *player);

#endif
