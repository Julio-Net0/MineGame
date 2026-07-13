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
void UpdateFreeCamera(GameCamera *Camera, Vec2 LookDelta, PlayerInput Move,
                      float Dt);

#endif
