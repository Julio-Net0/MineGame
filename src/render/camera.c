#include "render/camera.h"
#include "core/camera.h"
#include "core/vecmath.h"

#define WORLD_UP_VECTOR (Vec3){0.0F, 1.0F, 0.0F}
#define WORLD_ORIGIN (Vec3){0.0F, 0.0F, 0.0F}

#define CAMERA_POSITION (Vec3){8.0F, 20.0F, 8.0F}
#define INITIAL_LOOK_AT WORLD_ORIGIN

#define INITIAL_FOV 90.0F
#define CAMERA_PITCH_LIMIT 89.0F
#define MOUSE_SENSITIVITY 0.05F
#define DEG_TO_RAD (3.14159265358979323846F / 180.0F)
#define FREECAM_SPEED 20.0F

enum {
  ALIGNMENT_EIGHT = 8
};

static float CustomCosf(float XVal) {
  return __builtin_cosf(XVal);
}

static float CustomSinf(float XVal) {
  return __builtin_sinf(XVal);
}

typedef struct {
  float Pitch;
  float Yaw;
} __attribute__((aligned(ALIGNMENT_EIGHT))) CameraState;

static CameraState *GetCameraState(void) {
  static CameraState SCameraState = {0.0F, 0.0F};
  return &SCameraState;
}

// Freecam keeps its own yaw/pitch so switching modes does not fight the player
// camera's look state.
static CameraState *GetFreeCameraState(void) {
  static CameraState SFreeCameraState = {0.0F, 0.0F};
  return &SFreeCameraState;
}

// Yaw/pitch view vector from the shared look formula.
static Vec3 ViewVectorFromState(const CameraState *State) {
  float PitchRad = State->Pitch * DEG_TO_RAD;
  float YawRad = State->Yaw * DEG_TO_RAD;

  Vec3 ViewVector;
  ViewVector.x = CustomCosf(PitchRad) * CustomSinf(YawRad);
  ViewVector.y = CustomSinf(PitchRad);
  ViewVector.z = CustomCosf(PitchRad) * CustomCosf(YawRad);
  return ViewVector;
}

static void ApplyLook(CameraState *State, Vec2 LookDelta) {
  State->Yaw -= (LookDelta.x * MOUSE_SENSITIVITY);
  State->Pitch -= (LookDelta.y * MOUSE_SENSITIVITY);

  if (State->Pitch > CAMERA_PITCH_LIMIT) {
    State->Pitch = CAMERA_PITCH_LIMIT;
  }
  if (State->Pitch < -CAMERA_PITCH_LIMIT) {
    State->Pitch = -CAMERA_PITCH_LIMIT;
  }
}

GameCamera CreateGameCamera(void) {
  GameCamera Camera = { 0 };
  Camera.Position = CAMERA_POSITION;
  Camera.Target = INITIAL_LOOK_AT;
  Camera.Up = WORLD_UP_VECTOR;
  Camera.FovY = INITIAL_FOV;
  return Camera;
}

void UpdateGameCamera(GameCamera *Camera, Vec2 LookDelta) {
  CameraState *State = GetCameraState();
  ApplyLook(State, LookDelta);

  Vec3 ViewVector = ViewVectorFromState(State);
  Camera->Target = Vec3Add(Camera->Position, ViewVector);
}

void CameraFollowTarget(GameCamera *Camera, Vec3 PlayerPos, float HeadOffset) {
  Vec3 ViewVector = Vec3Sub(Camera->Target, Camera->Position);

  Camera->Position.x = PlayerPos.x;
  Camera->Position.y = PlayerPos.y + HeadOffset;
  Camera->Position.z = PlayerPos.z;

  Camera->Target = Vec3Add(Camera->Position, ViewVector);
}

void UpdateFreeCamera(GameCamera *Camera, Vec2 LookDelta, PlayerInput Move,
                      float Dt) {
  CameraState *State = GetFreeCameraState();
  ApplyLook(State, LookDelta);

  Vec3 Forward = ViewVectorFromState(State);
  Vec3 Right = Vec3Normalize(Vec3Cross(Forward, WORLD_UP_VECTOR));

  float Vertical = (Move.AscendHeld ? 1.0F : 0.0F) -
                   (Move.DescendHeld ? 1.0F : 0.0F);

  Vec3 Motion = Vec3Scale(Forward, Move.MoveForward);
  Motion = Vec3Add(Motion, Vec3Scale(Right, Move.MoveX));
  Motion = Vec3Add(Motion, Vec3Scale(WORLD_UP_VECTOR, Vertical));

  float Step = FREECAM_SPEED * Dt;
  Camera->Position = Vec3Add(Camera->Position, Vec3Scale(Motion, Step));
  Camera->Target = Vec3Add(Camera->Position, Forward);
}
