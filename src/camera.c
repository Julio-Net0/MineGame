#include "camera.h"
#include "raylib.h"
#include "rcamera.h"

#define WORLD_UP_VECTOR (Vector3){0.0F, 1.0F, 0.0F}
#define WORLD_ORIGIN (Vector3){0.0F, 0.0F, 0.0F}

#define CAMERA_POSITION (Vector3){8.0F, 20.0F, 8.0F}
#define INITIAL_LOOK_AT WORLD_ORIGIN

#define INITIAL_FOV 90.0F

Camera3D CreateGameCamera(void){
  Camera3D camera = { 0 };
  camera.position = CAMERA_POSITION;
  camera.target = INITIAL_LOOK_AT;
  camera.up = WORLD_UP_VECTOR;
  camera.fovy = INITIAL_FOV;
  camera.projection = CAMERA_PERSPECTIVE;
  return camera;
}


void UpdateGameCamera(Camera3D *camera){
  UpdateCamera(camera, CAMERA_FREE);
} 
