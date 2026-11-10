#ifndef CAMERA_H
#define CAMERA_H

#include "core/camera.h"
#include "core/vecmath.h"
#include "player/player.h"

GameCamera CreateGameCamera(void);

void UpdateGameCamera(GameCamera *Camera, Vec2 LookDelta);

// Snap the camera to the player's head position, preserving its view direction.
// Client-side presentation concern (not simulation).
void CameraFollowTarget(GameCamera *Camera, Vec3 PlayerPos, float HeadOffset);

// Debug free-flight camera: applies mouse-look and translates from the player
// movement intent. Reimplements Raylib's CAMERA_FREE with engine math + input.
// Point a camera at an absolute yaw/pitch in degrees, as if the mouse had been
// moved there: yaw 0 looks along +Z and grows towards +X, positive pitch looks
// up, and pitch is clamped to the same limit mouse look obeys. The free camera
// keeps its own look state, so it has its own setter.
void CameraSetAim(GameCamera *Camera, float YawDegrees, float PitchDegrees);
void FreeCameraSetAim(GameCamera *Camera, float YawDegrees, float PitchDegrees);

void UpdateFreeCamera(GameCamera *Camera, Vec2 LookDelta, PlayerInput Move,
                      float Dt);

#endif
