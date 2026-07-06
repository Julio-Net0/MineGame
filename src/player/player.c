#include "player/player.h"
#include "world/block_system.h"
#include "ui/debug.h"
#include "raylib.h"
#include "raymath.h"
#include "world/world.h"
#include "core/vecmath.h"

#define INIT_SLOT_0 0
#define INIT_SLOT_1 1
#define INIT_SLOT_2 2
#define INIT_SLOT_3 3
#define INIT_SLOT_4 4
#define INIT_SLOT_5 5
#define INIT_SLOT_6 6

#define INIT_BLOCK_1 1
#define INIT_BLOCK_2 2
#define INIT_BLOCK_3 3
#define INIT_BLOCK_4 4
#define INIT_BLOCK_5 5
#define INIT_BLOCK_6 6
#define INIT_BLOCK_7 7

Player InitPlayer(Vec3 SpawnPos) {
  Player PlayerObj = {0};

  PlayerObj.Id = 0;
  PlayerObj.Position = SpawnPos;
  PlayerObj.Velocity = (Vec3){0.0F, 0.0F, 0.0F};
  PlayerObj.Size = PLAYER_SIZE;
  PlayerObj.Radius = PLAYER_RADIUS;
  PlayerObj.Speed = PLAYER_SPEED;
  PlayerObj.SelectedHotbarSlot = 0;
  PlayerObj.ReachDistance = PLAYER_REACH_DISTANCE;
  PlayerObj.HeadOffset = PLAYER_HEAD_OFFSET;
  PlayerObj.IsGrounded = false;
  PlayerObj.Noclip = false;

  SetHotbarSlot(&PlayerObj, INIT_SLOT_0, INIT_BLOCK_1);
  SetHotbarSlot(&PlayerObj, INIT_SLOT_1, INIT_BLOCK_2);
  SetHotbarSlot(&PlayerObj, INIT_SLOT_2, INIT_BLOCK_3);
  SetHotbarSlot(&PlayerObj, INIT_SLOT_3, INIT_BLOCK_4);
  SetHotbarSlot(&PlayerObj, INIT_SLOT_4, INIT_BLOCK_5);
  SetHotbarSlot(&PlayerObj, INIT_SLOT_5, INIT_BLOCK_6);
  SetHotbarSlot(&PlayerObj, INIT_SLOT_6, INIT_BLOCK_7);

  return PlayerObj;
}

void SetHotbarSlot(Player *PlayerVal, int Slot, unsigned char BlockId) {
  if (Slot >= 0 && Slot < HOTBAR_SIZE) {
    PlayerVal->Hotbar[Slot] = BlockId;
  }
}

bool IsPointSolid(World *WorldVal, Vec3 Pos) {
  Vec3 BlockPos = {__builtin_floorf(Pos.x + BLOCK_HALF_SIZE),
                      __builtin_floorf(Pos.y + BLOCK_HALF_SIZE),
                      __builtin_floorf(Pos.z + BLOCK_HALF_SIZE)};

  int BlockId = GetBlockIDFromWorld(WorldVal, BlockPos);

  return (BlockId != 0 && GetBlockDef(BlockId)->IsSolid) != 0;
}

void GetPlayerPoints(Player *PlayerVal, PointConfig Config,
                     Vec3 OutPoints[COLLISION_POINTS]) {
  float CheckY = PlayerVal->Position.y + Config.YOffset + Config.Epsilon;

  OutPoints[0] = (Vec3){PlayerVal->Position.x, CheckY, PlayerVal->Position.z};
  OutPoints[1] = (Vec3){PlayerVal->Position.x - Config.Radius, CheckY,
                           PlayerVal->Position.z - Config.Radius};
  OutPoints[2] = (Vec3){PlayerVal->Position.x + Config.Radius, CheckY,
                           PlayerVal->Position.z - Config.Radius};
  OutPoints[3] = (Vec3){PlayerVal->Position.x - Config.Radius, CheckY,
                           PlayerVal->Position.z + Config.Radius};
  OutPoints[4] = (Vec3){PlayerVal->Position.x + Config.Radius, CheckY,
                           PlayerVal->Position.z + Config.Radius};
}

static bool IsAnyPointSolid(World *WorldVal, Vec3 Points[], int PointsLen) {
  #pragma unroll 4
  for (int IdxI = 0; IdxI < PointsLen; IdxI++) {
    if (IsPointSolid(WorldVal, Points[IdxI])) {
      return true;
    }
  }
  return false;
}

static bool IsPlayerOnGround(World *WorldVal, Player *PlayerVal) {
  Vec3 BottomPoints[COLLISION_POINTS];
  GetPlayerPoints(
      PlayerVal,
      (PointConfig){.Radius = PlayerVal->Radius - VERTICAL_RADIUS_SHRINK,
                    .YOffset = 0.0F,
                    .Epsilon = -COLLISION_EPSILON},
      BottomPoints);

  return IsAnyPointSolid(WorldVal, BottomPoints, COLLISION_POINTS);
}

static bool VerifyBottomCollision(Player *PlayerVal, World *WorldVal, float NextY) {
  float CurrentY = PlayerVal->Position.y;
  PlayerVal->Position.y = NextY;
  bool HitsGround = IsPlayerOnGround(WorldVal, PlayerVal);
  PlayerVal->Position.y = CurrentY;

  if (PlayerVal->Velocity.y <= 0.0F && HitsGround) {
    PlayerVal->Velocity.y = 0.0F;
    PlayerVal->IsGrounded = true;

    float CheckY = NextY - COLLISION_EPSILON;
    float BlockCenterY = __builtin_floorf(CheckY + BLOCK_HALF_SIZE);

    PlayerVal->Position.y = BlockCenterY + BLOCK_HALF_SIZE;

    return true;
  }

  PlayerVal->IsGrounded = false;
  return false;
}

static bool IsPlayerOnTop(World *WorldVal, Player *PlayerVal, float NextY) {
  float CurrentY = PlayerVal->Position.y;
  PlayerVal->Position.y = NextY;

  Vec3 TopPoints[COLLISION_POINTS];
  GetPlayerPoints(
      PlayerVal,
      (PointConfig){.Radius = PlayerVal->Radius - VERTICAL_RADIUS_SHRINK,
                    .YOffset = PlayerVal->Size.y,
                    .Epsilon = COLLISION_EPSILON},
      TopPoints);

  PlayerVal->Position.y = CurrentY;

  return IsAnyPointSolid(WorldVal, TopPoints, COLLISION_POINTS);
}

static bool VerifyTopCollision(Player *PlayerVal, World *WorldVal, float NextY) {
  if (PlayerVal->Velocity.y > 0.0F && IsPlayerOnTop(WorldVal, PlayerVal, NextY)) {
    PlayerVal->Velocity.y = 0.0F;

    float CheckY = NextY + PlayerVal->Size.y + COLLISION_EPSILON;
    float BlockCenterY = __builtin_floorf(CheckY + BLOCK_HALF_SIZE);
    float CeilingBottom = BlockCenterY - BLOCK_HALF_SIZE;

    PlayerVal->Position.y = CeilingBottom - PlayerVal->Size.y - AABB_OFFSET_MARGIN;
    return true;
  }
  return false;
}

static bool IsPlayerCollidingLateral(World *WorldVal, Player *PlayerVal) {
  Vec3 ShinPoints[COLLISION_POINTS];
  Vec3 FacePoints[COLLISION_POINTS];

  GetPlayerPoints(PlayerVal,
                  (PointConfig){.Radius = PlayerVal->Radius,
                                .YOffset = LATERAL_Y_MARGIN,
                                .Epsilon = 0.0F},
                  ShinPoints);
  GetPlayerPoints(PlayerVal,
                  (PointConfig){.Radius = PlayerVal->Radius,
                                .YOffset = PlayerVal->Size.y - LATERAL_Y_MARGIN,
                                .Epsilon = 0.0F},
                  FacePoints);

  if (IsAnyPointSolid(WorldVal, ShinPoints, COLLISION_POINTS)) {
    return true;
  }
  if (IsAnyPointSolid(WorldVal, FacePoints, COLLISION_POINTS)) {
    return true;
  }

  return false;
}

static bool VerifyXCollision(Player *PlayerVal, World *WorldVal, float NextX) {
  if (PlayerVal->Velocity.x == 0.0F) {
    return false;
  }

  float CurrentX = PlayerVal->Position.x;
  PlayerVal->Position.x = NextX;
  bool HitsWall = IsPlayerCollidingLateral(WorldVal, PlayerVal);
  PlayerVal->Position.x = CurrentX;

  if (HitsWall) {
    float Sign = (PlayerVal->Velocity.x > 0.0F) ? 1.0F : -1.0F;

    float HitPointX =
        NextX + (PlayerVal->Radius * Sign) + (COLLISION_EPSILON * Sign);
    float BlockCenterX = __builtin_floorf(HitPointX + BLOCK_HALF_SIZE);

    if (PlayerVal->Velocity.x > 0.0F) {
      PlayerVal->Position.x = (BlockCenterX - BLOCK_HALF_SIZE) - PlayerVal->Radius -
                           AABB_OFFSET_MARGIN;
    } else {
      PlayerVal->Position.x = (BlockCenterX + BLOCK_HALF_SIZE) + PlayerVal->Radius +
                           AABB_OFFSET_MARGIN;
    }

    PlayerVal->Velocity.x = 0.0F;
    return true;
  }
  return false;
}

static bool VerifyZCollision(Player *PlayerVal, World *WorldVal, float NextZ) {
  if (PlayerVal->Velocity.z == 0.0F) {
    return false;
  }

  float CurrentZ = PlayerVal->Position.z;
  PlayerVal->Position.z = NextZ;
  bool HitsWall = IsPlayerCollidingLateral(WorldVal, PlayerVal);
  PlayerVal->Position.z = CurrentZ;

  if (HitsWall) {
    float Sign = (PlayerVal->Velocity.z > 0.0F) ? 1.0F : -1.0F;

    float HitPointZ =
        NextZ + (PlayerVal->Radius * Sign) + (COLLISION_EPSILON * Sign);
    float BlockCenterZ = __builtin_floorf(HitPointZ + BLOCK_HALF_SIZE);

    if (PlayerVal->Velocity.z > 0.0F) {
      PlayerVal->Position.z = (BlockCenterZ - BLOCK_HALF_SIZE) - PlayerVal->Radius -
                           AABB_OFFSET_MARGIN;
    } else {
      PlayerVal->Position.z = (BlockCenterZ + BLOCK_HALF_SIZE) + PlayerVal->Radius +
                           AABB_OFFSET_MARGIN;
    }

    PlayerVal->Velocity.z = 0.0F;
    return true;
  }
  return false;
}

static void PhysicalMov(Player *PlayerVal, World *WorldVal, float Dt,
                        bool HasControl) {
  if (PlayerVal->IsGrounded && IsKeyPressed(KEY_SPACE) && HasControl) {
    PlayerVal->Velocity.y = PLAYER_JUMP_FORCE;
    PlayerVal->IsGrounded = false;
  }

  PlayerVal->Velocity.y -= PLAYER_GRAVITY * Dt;
  if (PlayerVal->Velocity.y < -PLAYER_TERMINAL_VELOCITY) {
    PlayerVal->Velocity.y = -PLAYER_TERMINAL_VELOCITY;
  }

  float NextY = PlayerVal->Position.y + (PlayerVal->Velocity.y * Dt);
  if (PlayerVal->Velocity.y > 0.0F) {
    if (!VerifyTopCollision(PlayerVal, WorldVal, NextY)) {
      PlayerVal->Position.y = NextY;
    }
  } else {
    if (!VerifyBottomCollision(PlayerVal, WorldVal, NextY)) {
      PlayerVal->Position.y = NextY;
    }
  }

  float NextX = PlayerVal->Position.x + (PlayerVal->Velocity.x * Dt);
  if (!VerifyXCollision(PlayerVal, WorldVal, NextX)) {
    PlayerVal->Position.x = NextX;
  }
  float NextZ = PlayerVal->Position.z + (PlayerVal->Velocity.z * Dt);
  if (!VerifyZCollision(PlayerVal, WorldVal, NextZ)) {
    PlayerVal->Position.z = NextZ;
  }
}

static void NoClipMov(Player *PlayerVal, float Dt) {
  PlayerVal->Velocity.y = 0.0F;
  if (IsKeyDown(KEY_SPACE)) {
    PlayerVal->Position.y += PlayerVal->Speed * Dt;
  }
  if (IsKeyDown(KEY_LEFT_CONTROL)) {
    PlayerVal->Position.y -= PlayerVal->Speed * Dt;
  }

  PlayerVal->Position.x += PlayerVal->Velocity.x * Dt;
  PlayerVal->Position.z += PlayerVal->Velocity.z * Dt;
}

static Vec3 HandleInput(bool HasControl) {
  Vec3 Input = {0.0F, 0.0F, 0.0F};

  if (!HasControl) {
    return Input;
  }

  if (IsKeyDown(KEY_W)) {
    Input.z += 1.0F;
  }
  if (IsKeyDown(KEY_S)) {
    Input.z -= 1.0F;
  }
  if (IsKeyDown(KEY_A)) {
    Input.x -= 1.0F;
  }
  if (IsKeyDown(KEY_D)) {
    Input.x += 1.0F;
  }

  return Input;
}

static Vec3 CalculateMoveDir(Camera3D *CameraVal, Vec3 Input) {
  // Camera is still a Raylib type (deferred seam); convert its vectors to
  // engine Vec3 at this boundary and run the movement math in engine ops.
  Vec3 CamTarget = {CameraVal->target.x, CameraVal->target.y, CameraVal->target.z};
  Vec3 CamPos = {CameraVal->position.x, CameraVal->position.y, CameraVal->position.z};

  Vec3 ViewVector = Vec3Sub(CamTarget, CamPos);
  Vec3 Forward = Vec3Normalize((Vec3){ViewVector.x, 0.0F, ViewVector.z});
  Vec3 Right =
      Vec3Normalize(Vec3Cross(Forward, (Vec3){0.0F, 1.0F, 0.0F}));

  Vec3 MoveDir = {0.0F, 0.0F, 0.0F};

  MoveDir = Vec3Add(MoveDir, Vec3Scale(Forward, Input.z));
  MoveDir = Vec3Add(MoveDir, Vec3Scale(Right, Input.x));

  if (Vec3Length(MoveDir) > 0.0F) {
    MoveDir = Vec3Normalize(MoveDir);
  }

  return MoveDir;
}

static void UpdateCameraFollow(Player *PlayerVal, Camera3D *CameraVal) {
  // Camera vectors stay Raylib types (deferred seam). Preserve the view
  // direction while snapping the camera to the player's head position.
  Vector3 ViewVector = Vector3Subtract(CameraVal->target, CameraVal->position);

  CameraVal->position.x = PlayerVal->Position.x;
  CameraVal->position.y = PlayerVal->Position.y + PlayerVal->HeadOffset;
  CameraVal->position.z = PlayerVal->Position.z;

  CameraVal->target = Vector3Add(CameraVal->position, ViewVector);
}

void UpdatePlayer(Player *PlayerVal, Camera3D *CameraVal, World *WorldVal, float Dt,
                  bool HasControl) {
  if (GetDebugState()->Freecam) {
    return;
  }

  Vec3 Input = HandleInput(HasControl);

  Vec3 MoveDir = CalculateMoveDir(CameraVal, Input);

  PlayerVal->Velocity.x = MoveDir.x * PlayerVal->Speed;
  PlayerVal->Velocity.z = MoveDir.z * PlayerVal->Speed;

  if (!PlayerVal->Noclip) {
    PhysicalMov(PlayerVal, WorldVal, Dt, HasControl);
  } else {
    NoClipMov(PlayerVal, Dt);
  }

  UpdateCameraFollow(PlayerVal, CameraVal);
}

static void RequestBlockBreak(World *WorldVal, Vec3 Pos) {
  SetBlockInWorld(WorldVal, Pos, 0);
}

static void RequestBlockPlace(World *WorldVal, Vec3 Pos,
                              unsigned char BlockId) {
  SetBlockInWorld(WorldVal, Pos, BlockId);
}

void HandlePlayerInteraction(Player *PlayerVal, Camera3D *CameraVal, World *WorldVal,
                             bool HasControl) {
  // Camera is a Raylib type (deferred seam); build the ray in engine Vec3
  // before handing it to the simulation raycast.
  Vec3 RayOrigin = {CameraVal->position.x, CameraVal->position.y, CameraVal->position.z};
  Vec3 CamTarget = {CameraVal->target.x, CameraVal->target.y, CameraVal->target.z};
  Vec3 RayDir = Vec3Normalize(Vec3Sub(CamTarget, RayOrigin));
  PlayerVal->TargetBlock =
      RayCastToWorld(WorldVal, RayOrigin, RayDir, PlayerVal->ReachDistance);

  if (!HasControl) {
    return;
  }

  #pragma unroll 4
  for (int IdxI = 0; IdxI < HOTBAR_SIZE; IdxI++) {
    if (IsKeyPressed(KEY_ONE + IdxI)) {
      PlayerVal->SelectedHotbarSlot = IdxI;
    }
  }

  float Wheel = GetMouseWheelMove();
  if (Wheel != 0.0F) {
    PlayerVal->SelectedHotbarSlot -= (int)Wheel;
    if (PlayerVal->SelectedHotbarSlot < 0) {
      PlayerVal->SelectedHotbarSlot = HOTBAR_SIZE - 1;
    }
    if (PlayerVal->SelectedHotbarSlot > HOTBAR_SIZE - 1) {
      PlayerVal->SelectedHotbarSlot = 0;
    }
  }

  if (PlayerVal->TargetBlock.Hit) {
    if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
      RequestBlockBreak(WorldVal, PlayerVal->TargetBlock.BlockPos);
    }

    if (IsMouseButtonPressed(MOUSE_RIGHT_BUTTON)) {
      unsigned char BlockInHandId = PlayerVal->Hotbar[PlayerVal->SelectedHotbarSlot];

      Vec3 PlacePos =
          Vec3Add(PlayerVal->TargetBlock.BlockPos, PlayerVal->TargetBlock.Normal);
      RequestBlockPlace(WorldVal, PlacePos, BlockInHandId);
    }
  }
}
