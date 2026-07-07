#include "render/camera.h"
#include "raylib.h"
#include "raymath.h"
#include "core/vecmath.h"

#define WORLD_UP_VECTOR (Vector3){0.0F, 1.0F, 0.0F}
#define WORLD_ORIGIN (Vector3){0.0F, 0.0F, 0.0F}

#define CAMERA_POSITION (Vector3){8.0F, 20.0F, 8.0F}
#define INITIAL_LOOK_AT WORLD_ORIGIN

#define INITIAL_FOV 90.0F
#define CAMERA_PITCH_LIMIT 89.0F
#define MOUSE_SENSITIVITY 0.05F

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

Camera3D CreateGameCamera(void) {
  Camera3D Camera = { 0 };
  Camera.position = CAMERA_POSITION;
  Camera.target = INITIAL_LOOK_AT;
  Camera.up = WORLD_UP_VECTOR;
  Camera.fovy = INITIAL_FOV;
  Camera.projection = CAMERA_PERSPECTIVE;
  return Camera;
}

void UpdateGameCamera(Camera3D *Camera, Vector2 LookDelta) {
  float Sensitivity = MOUSE_SENSITIVITY;
  CameraState *State = GetCameraState();

  State->Yaw -= (LookDelta.x * Sensitivity);
  State->Pitch -= (LookDelta.y * Sensitivity);

  if (State->Pitch > CAMERA_PITCH_LIMIT) {
    State->Pitch = CAMERA_PITCH_LIMIT;
  }
  if (State->Pitch < -CAMERA_PITCH_LIMIT) {
    State->Pitch = -CAMERA_PITCH_LIMIT;
  }

  float PitchRad = State->Pitch * DEG2RAD;
  float YawRad = State->Yaw * DEG2RAD;

  Vector3 ViewVector;
  ViewVector.x = CustomCosf(PitchRad) * CustomSinf(YawRad);
  ViewVector.y = CustomSinf(PitchRad);
  ViewVector.z = CustomCosf(PitchRad) * CustomCosf(YawRad);

  Camera->target = Vector3Add(Camera->position, ViewVector);
}

void CameraFollowTarget(Camera3D *Camera, Vec3 PlayerPos, float HeadOffset) {
  Vector3 ViewVector = Vector3Subtract(Camera->target, Camera->position);

  Camera->position.x = PlayerPos.x;
  Camera->position.y = PlayerPos.y + HeadOffset;
  Camera->position.z = PlayerPos.z;

  Camera->target = Vector3Add(Camera->position, ViewVector);
}
