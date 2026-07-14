#include "player/player.h"
#include "world/block_system.h"
#include "ui/debug.h"
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
  PlayerObj.PrevPosition = SpawnPos;
  PlayerObj.Velocity = (Vec3){0.0F, 0.0F, 0.0F};
  PlayerObj.Size = PLAYER_SIZE;
  PlayerObj.Radius = PLAYER_RADIUS;
  PlayerObj.Speed = PLAYER_SPEED;
  PlayerObj.SelectedHotbarSlot = 0;
  PlayerObj.ReachDistance = PLAYER_REACH_DISTANCE;
  PlayerObj.HeadOffset = PLAYER_HEAD_OFFSET;
  PlayerObj.IsGrounded = false;
  PlayerObj.Noclip = false;
  PlayerObj.HasSelectionA = false;
  PlayerObj.HasSelectionB = false;
  PlayerObj.HasSelectionOffset = false;

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

// Discontinuous move (teleport, respawn): set position and collapse the
// render-interpolation baseline so the camera does not smear across the jump.
void SetPlayerPosition(Player *PlayerVal, Vec3 Pos) {
  PlayerVal->Position = Pos;
  PlayerVal->PrevPosition = Pos;
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
                        bool JumpRequested) {
  if (PlayerVal->IsGrounded && JumpRequested) {
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

static void NoClipMov(Player *PlayerVal, float Dt, bool AscendHeld, bool DescendHeld) {
  PlayerVal->Velocity.y = 0.0F;
  if (AscendHeld) {
    PlayerVal->Position.y += PlayerVal->Speed * Dt;
  }
  if (DescendHeld) {
    PlayerVal->Position.y -= PlayerVal->Speed * Dt;
  }

  PlayerVal->Position.x += PlayerVal->Velocity.x * Dt;
  PlayerVal->Position.z += PlayerVal->Velocity.z * Dt;
}

// Movement basis from the view forward (flattened to the horizontal plane).
static Vec3 CalculateMoveDir(Vec3 ViewForward, float MoveX, float MoveForward) {
  Vec3 Forward = Vec3Normalize((Vec3){ViewForward.x, 0.0F, ViewForward.z});
  Vec3 Right =
      Vec3Normalize(Vec3Cross(Forward, (Vec3){0.0F, 1.0F, 0.0F}));

  Vec3 MoveDir = {0.0F, 0.0F, 0.0F};

  MoveDir = Vec3Add(MoveDir, Vec3Scale(Forward, MoveForward));
  MoveDir = Vec3Add(MoveDir, Vec3Scale(Right, MoveX));

  if (Vec3Length(MoveDir) > 0.0F) {
    MoveDir = Vec3Normalize(MoveDir);
  }

  return MoveDir;
}

void UpdatePlayer(Player *PlayerVal, World *WorldVal, PlayerView View,
                  PlayerInput Input, float Dt) {
  if (GetDebugState()->Freecam) {
    return;
  }

  Vec3 MoveDir = CalculateMoveDir(View.Forward, Input.MoveX, Input.MoveForward);

  PlayerVal->Velocity.x = MoveDir.x * PlayerVal->Speed;
  PlayerVal->Velocity.z = MoveDir.z * PlayerVal->Speed;

  if (!PlayerVal->Noclip) {
    PhysicalMov(PlayerVal, WorldVal, Dt, Input.Jump);
  } else {
    NoClipMov(PlayerVal, Dt, Input.AscendHeld, Input.DescendHeld);
  }
}

static void RequestBlockBreak(World *WorldVal, Vec3 Pos) {
  SetBlockInWorld(WorldVal, Pos, 0);
}

static void RequestBlockPlace(World *WorldVal, Vec3 Pos,
                              unsigned char BlockId) {
  SetBlockInWorld(WorldVal, Pos, BlockId);
}

void HandlePlayerInteraction(Player *PlayerVal, World *WorldVal, PlayerView View,
                             PlayerInput Input) {
  Vec3 RayDir = Vec3Normalize(View.Forward);
  PlayerVal->TargetBlock =
      RayCastToWorld(WorldVal, View.EyePosition, RayDir, PlayerVal->ReachDistance);

  if (Input.HotbarSelect >= 0) {
    PlayerVal->SelectedHotbarSlot = Input.HotbarSelect;
  }

  if (Input.HotbarScroll != 0) {
    PlayerVal->SelectedHotbarSlot -= Input.HotbarScroll;
    if (PlayerVal->SelectedHotbarSlot < 0) {
      PlayerVal->SelectedHotbarSlot = HOTBAR_SIZE - 1;
    }
    if (PlayerVal->SelectedHotbarSlot > HOTBAR_SIZE - 1) {
      PlayerVal->SelectedHotbarSlot = 0;
    }
  }

  if (PlayerVal->TargetBlock.Hit) {
    if (Input.Break) {
      RequestBlockBreak(WorldVal, PlayerVal->TargetBlock.BlockPos);
    }

    if (Input.Place) {
      unsigned char BlockInHandId = PlayerVal->Hotbar[PlayerVal->SelectedHotbarSlot];

      Vec3 PlacePos =
          Vec3Add(PlayerVal->TargetBlock.BlockPos, PlayerVal->TargetBlock.Normal);
      RequestBlockPlace(WorldVal, PlacePos, BlockInHandId);
    }
  }
}
