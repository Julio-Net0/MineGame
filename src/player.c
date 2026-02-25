#include "raylib.h"
#include "player.h"
#include "raymath.h"
#include "world.h"
#include <math.h>

#define COLLISION_POINTS 5

static const float COLLISION_EPSILON = 0.01F;
static const float AABB_OFFSET_MARGIN = 0.001F;

static const float VERTICAL_RADIUS_SHRINK = 0.05F;
static const float LATERAL_Y_MARGIN = 0.1F;

Player InitPlayer(Vector3 spawnPos){
  
  Player p = {0};

  p.id = 0;
  p.position = spawnPos;
  p.velocity = (Vector3){0,0,0};
  p.size = PLAYER_SIZE;
  p.radius = PLAYER_RADIUS;
  p.speed = PLAYER_SPEED;
  p.headOffset = PLAYER_HEAD_OFFSET;
  p.isGrounded = false;
  p.noclip = false;
  p.debug_aabb = false;
  return p;
}

bool IsPointSolid(World *world, Vector3 pos){
  Vector3 blockPos = { floorf(pos.x + BLOCK_HALF_SIZE), floorf(pos.y + BLOCK_HALF_SIZE), floorf(pos.z + BLOCK_HALF_SIZE) };

  int blockID = GetBlockIDFromWorld(world, blockPos);

  return (blockID != 0);
}

void GetPlayerPoints(Player *player, float r, float yOffset, float epsilon, Vector3 outPoints[COLLISION_POINTS]){
  float checkY = player->position.y + yOffset + epsilon;

  outPoints[0] = (Vector3){ player->position.x,     checkY, player->position.z };
  outPoints[1] = (Vector3){ player->position.x - r, checkY, player->position.z - r };
  outPoints[2] = (Vector3){ player->position.x + r, checkY, player->position.z - r };
  outPoints[3] = (Vector3){ player->position.x - r, checkY, player->position.z + r };
  outPoints[4] = (Vector3){ player->position.x + r, checkY, player->position.z + r };
}

bool IsAnyPointSolid(World *world, Vector3 points[], int pointsLen){
  for(int i = 0; i < pointsLen; i++){
    if(IsPointSolid(world, points[i])) { return true; }
  }
  return false;
}

bool IsPlayerOnGround(World *world, Player *player){
  Vector3 bottomPoints[COLLISION_POINTS];
  GetPlayerPoints(player, player->radius - VERTICAL_RADIUS_SHRINK, 0.0F, -COLLISION_EPSILON, bottomPoints);

  return IsAnyPointSolid(world, bottomPoints, COLLISION_POINTS);
}

bool VerifyBottomCollision(Player *player, World *world, float nextY){

  float currentY = player->position.y;
  player->position.y = nextY;
  bool hitsGround = IsPlayerOnGround(world, player);
  player->position.y = currentY;

  if(player->velocity.y <= 0 && hitsGround){
    player->velocity.y = 0;
    player->isGrounded = true;

    float checkY = nextY - COLLISION_EPSILON;
    float blockCenterY = floorf(checkY + BLOCK_HALF_SIZE);

    player->position.y = blockCenterY + BLOCK_HALF_SIZE;

    return true;
  }

  player->isGrounded = false;
  return false;
}

bool IsPlayerOnTop(World *world, Player *player, float nextY){

  float currentY = player->position.y;

  player->position.y = nextY;

  Vector3 topPoints[COLLISION_POINTS];
  GetPlayerPoints(player, player->radius - VERTICAL_RADIUS_SHRINK, player->size.y, COLLISION_EPSILON, topPoints);

  player->position.y = currentY;

  return IsAnyPointSolid(world, topPoints, COLLISION_POINTS);
}

bool VerifyTopCollision(Player *player, World *world, float nextY){

  if(player->velocity.y > 0 && IsPlayerOnTop(world, player, nextY)){
    player->velocity.y = 0;

    float checkY = nextY + player->size.y + COLLISION_EPSILON;
    float blockCenterY = floorf(checkY + BLOCK_HALF_SIZE);
    float ceilingBottom = blockCenterY - BLOCK_HALF_SIZE;

    player->position.y = ceilingBottom - player->size.y - AABB_OFFSET_MARGIN;
    return true;
  }
  return false;
}

bool IsPlayerCollidingLateral(World *world, Player *player){
  Vector3 shinPoints[COLLISION_POINTS];
  Vector3 facePoints[COLLISION_POINTS];

  GetPlayerPoints(player, player->radius, LATERAL_Y_MARGIN, 0.0F, shinPoints);
  GetPlayerPoints(player, player->radius, player->size.y - LATERAL_Y_MARGIN, 0.0F, facePoints);

  if(IsAnyPointSolid(world, shinPoints, COLLISION_POINTS)) { return true; }
  if(IsAnyPointSolid(world, facePoints, COLLISION_POINTS)) {return true; }

  return false;
}

bool VerifyXCollision(Player *player, World *world, float nextX){

  if(player->velocity.x == 0) { return false; }

  float currentX = player->position.x;
  player->position.x = nextX;
  bool hitsWall = IsPlayerCollidingLateral(world, player);
  player->position.x = currentX;

  if(hitsWall){
    float sign = (player->velocity.x > 0) ? 1.0F : -1.0F;

    float hitPointX = nextX + (player->radius * sign) + (COLLISION_EPSILON * sign);
    float blockCenterX = floorf(hitPointX + BLOCK_HALF_SIZE);

    if(player->velocity.x > 0){
      player->position.x = (blockCenterX - BLOCK_HALF_SIZE) - player->radius - AABB_OFFSET_MARGIN;
    } else {
      player->position.x = (blockCenterX + BLOCK_HALF_SIZE) + player->radius + AABB_OFFSET_MARGIN;
    }

    player->velocity.x = 0;
    return true;
  }
  return false;
}

bool VerifyZCollision(Player *player, World *world, float nextZ){
  if(player->velocity.z == 0) { return false; }

  float currentZ = player->position.z;
  player->position.z = nextZ;
  bool hitsWall = IsPlayerCollidingLateral(world, player);
  player->position.z = currentZ;

  if(hitsWall){
    float sign = (player->velocity.z > 0) ? 1.0F : -1.0F;
    
    float hitPointZ = nextZ + (player->radius * sign) + (COLLISION_EPSILON * sign);
    float blockCenterZ = floorf(hitPointZ + BLOCK_HALF_SIZE);

    if(player->velocity.z > 0){
      player->position.z = (blockCenterZ - BLOCK_HALF_SIZE) - player->radius - AABB_OFFSET_MARGIN;
    } else {
      player->position.z = (blockCenterZ + BLOCK_HALF_SIZE) + player->radius + AABB_OFFSET_MARGIN;
    }
    
    player->velocity.z = 0;
    return true;
  }
  return false;
}

void PhisicalMov(Player *player, World *world,float dt, bool hasControl){

  if(player->isGrounded && IsKeyPressed(KEY_SPACE) && hasControl){
    player->velocity.y = PLAYER_JUMP_FORCE;
    player->isGrounded = false;
  }

  player->velocity.y -= PLAYER_GRAVITY * dt;
  if(player->velocity.y < -PLAYER_TERMINAL_VELOCITY) { player->velocity.y = -PLAYER_TERMINAL_VELOCITY; }

  float nextY = player->position.y + (player->velocity.y * dt);
  if(player->velocity.y > 0){
    if(!VerifyTopCollision(player, world, nextY)){ player->position.y = nextY; }
  }else{
    if(!VerifyBottomCollision(player, world, nextY)){ player->position.y = nextY; }
  }

  float nextX = player->position.x + (player->velocity.x * dt);
  if(!VerifyXCollision(player, world, nextX)) { player->position.x = nextX; }
  float nextZ = player->position.z + (player->velocity.z * dt);
  if(!VerifyZCollision(player, world, nextZ)) { player->position.z = nextZ; }
}

void NoClipMov(Player *player, float dt){
  player->velocity.y = 0;
  if(IsKeyDown(KEY_SPACE)) { player->position.y += player->speed *dt; }
  if(IsKeyDown(KEY_LEFT_CONTROL)) { player->position.y -= player->speed *dt; }

  player->position.x += player->velocity.x * dt;
  player->position.z += player->velocity.z * dt;
}

Vector3 HandleInput(bool hasControl){
  Vector3 input = { 0 };

  if(!hasControl) { return input; }

  if(IsKeyDown(KEY_W)) {input.z += 1.0F;} 
  if(IsKeyDown(KEY_S)) {input.z -= 1.0F;}
  if(IsKeyDown(KEY_A)) {input.x -= 1.0F;}
  if(IsKeyDown(KEY_D)) {input.x += 1.0F;}

  return input;
}

Vector3 CalculateMoveDir(Camera3D *camera, Vector3 input){
  Vector3 viewVector = Vector3Subtract(camera->target, camera->position);
  Vector3 foward = Vector3Normalize((Vector3){ viewVector.x, 0, viewVector.z});
  Vector3 right = Vector3Normalize(Vector3CrossProduct(foward, (Vector3){ 0, 1, 0}));

  Vector3 moveDir = { 0 };

  moveDir = Vector3Add(moveDir, Vector3Scale(foward, input.z));
  moveDir = Vector3Add(moveDir, Vector3Scale(right, input.x));

  if(Vector3Length(moveDir) > 0){
    moveDir = Vector3Normalize(moveDir);
  }

  return moveDir;
}

void UpdateCameraFollow(Player *player, Camera3D *camera){
  Vector3 viewVector = Vector3Subtract(camera->target, camera->position);

  camera->position.x = player->position.x;
  camera->position.y = player->position.y + player->headOffset;
  camera->position.z = player->position.z;

  camera->target = Vector3Add(camera->position, viewVector);
}

void UpdatePlayer(Player *player, Camera3D *camera, World *world, float dt, bool hasControl){

  Vector3 input = HandleInput(hasControl);

  Vector3 moveDir = CalculateMoveDir(camera, input);

  player->velocity.x = moveDir.x * player->speed;
  player->velocity.z = moveDir.z * player->speed;

  if(!player->noclip){
    PhisicalMov(player, world, dt, hasControl);
  }else{
    NoClipMov(player, dt);
  }

  UpdateCameraFollow(player, camera);
}
void DrawPlayerDebug(World *world, Player *player){
  if(!player->debug_aabb) { return; }

  Vector3 bottomPoints[COLLISION_POINTS];
  Vector3 topPoints[COLLISION_POINTS];
  Vector3 shinPoints[COLLISION_POINTS];
  Vector3 facePoints[COLLISION_POINTS];

  GetPlayerPoints(player, player->radius - VERTICAL_RADIUS_SHRINK, 0.0F, -COLLISION_EPSILON, bottomPoints);
  GetPlayerPoints(player, player->radius - VERTICAL_RADIUS_SHRINK, player->size.y, COLLISION_EPSILON, topPoints);

  const float LATERALDEBUGRADIUS = player->radius + 0.01F;
  GetPlayerPoints(player, LATERALDEBUGRADIUS, LATERAL_Y_MARGIN, 0.0F, shinPoints);
  GetPlayerPoints(player, LATERALDEBUGRADIUS, player->size.y - LATERAL_Y_MARGIN, 0.0F, facePoints);

  float sz = PLAYER_DEBUG_AABB_SQUARES_SIZE;
  float wz = PLAYER_DEBUG_AABB_WIRES_SIZE;

  for(int i = 0; i < COLLISION_POINTS; i++){
    if(IsPointSolid(world, bottomPoints[i])){
      DrawCube(bottomPoints[i], sz, sz, sz, BLUE);
    }else{
      DrawCube(bottomPoints[i], sz, sz, sz, RED);
    }
    
    if(IsPointSolid(world, topPoints[i])){
      DrawCube(topPoints[i], sz, sz, sz, BLUE);
    }else{
      DrawCube(topPoints[i], sz, sz, sz, RED);
    }

    if(IsPointSolid(world, shinPoints[i])){
      DrawCubeWires(shinPoints[i], wz, wz, wz, PURPLE);
    }else{
      DrawCubeWires(shinPoints[i], wz, wz, wz, ORANGE);
    }

    if(IsPointSolid(world, facePoints[i])){
      DrawCubeWires(facePoints[i], wz, wz, wz, PURPLE);
    }else{
      DrawCubeWires(facePoints[i], wz, wz, wz, ORANGE);
    }
  }
}
