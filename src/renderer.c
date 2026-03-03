#include "renderer.h"
#include "block_system.h"
#include "raylib.h"
#include "raymath.h"
#include "player.h"
#include <stddef.h>
#include <string.h>

#define VERTICES_PER_FACE 4
#define FLOATS_PER_VERTEX 3
#define COLOR_CHANNELS 4
#define INDICES_PER_FACES 6
#define MAX_FACES (CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE * INDICES_PER_FACES)

#define BLOCK_HIGHLIGHT_SCALE (BLOCK_SIZE + 0.01F)

static float tempVertices[MAX_FACES * VERTICES_PER_FACE * FLOATS_PER_VERTEX];
static unsigned char tempColors[MAX_FACES * VERTICES_PER_FACE * COLOR_CHANNELS];
static unsigned short tempIndices[MAX_FACES * INDICES_PER_FACES];

static int vCount = 0;
static int iCount = 0;

static Material chunkMaterial;

void InitRenderer(void){
  chunkMaterial = LoadMaterialDefault();
}

static bool IsNeighbourTransparent(World *world, Chunk *chunk, int localX, int localY, int localZ){
  if(localX >= 0 && localX < CHUNK_SIZE && localY >= 0 && localY < CHUNK_SIZE && localZ >= 0 && localZ < CHUNK_SIZE){
      unsigned char id = chunk->data[localX][localY][localZ];
      if(id == 0) { return true; }
      return GetBlockDef(id)->isTransparent;
  }

  int globalX = (chunk->chunkX * CHUNK_SIZE) + localX;
  int globalY = (chunk->chunkY * CHUNK_SIZE) + localY;
  int globalZ = (chunk->chunkZ * CHUNK_SIZE) + localZ;

  int id = GetBlockIDFromWorld(world, (Vector3){(float)globalX, (float)globalY, (float)globalZ});
  if(id == 0) { return true; }
  return GetBlockDef(id)->isTransparent;
}

static void AddFaceToMeshBuilder(Vector3 pos, Color color, BlockFace face){
  float x = pos.x;
  float y = pos.y;
  float z = pos.z;

  int vIdx = vCount * 3;
  int cIdx = vCount * 4;

  for(int i = 0; i < 4; i++){
    tempColors[cIdx++] = color.r;
    tempColors[cIdx++] = color.g;
    tempColors[cIdx++] = color.b;
    tempColors[cIdx++] = color.a;
  }

  tempIndices[iCount++] = vCount + 0;
  tempIndices[iCount++] = vCount + 1;
  tempIndices[iCount++] = vCount + 2;
  tempIndices[iCount++] = vCount + 0;
  tempIndices[iCount++] = vCount + 2;
  tempIndices[iCount++] = vCount + 3;

  switch (face) {
    case FACE_TOP:
      tempVertices[vIdx++] = x - BLOCK_HALF_SIZE; tempVertices[vIdx++] = y + BLOCK_HALF_SIZE; tempVertices[vIdx++] = z - BLOCK_HALF_SIZE;
      tempVertices[vIdx++] = x - BLOCK_HALF_SIZE; tempVertices[vIdx++] = y + BLOCK_HALF_SIZE; tempVertices[vIdx++] = z + BLOCK_HALF_SIZE;
      tempVertices[vIdx++] = x + BLOCK_HALF_SIZE; tempVertices[vIdx++] = y + BLOCK_HALF_SIZE; tempVertices[vIdx++] = z + BLOCK_HALF_SIZE;
      tempVertices[vIdx++] = x + BLOCK_HALF_SIZE; tempVertices[vIdx++] = y + BLOCK_HALF_SIZE; tempVertices[vIdx++] = z - BLOCK_HALF_SIZE;
      break;
    case FACE_BOTTOM:
      tempVertices[vIdx++] = x - BLOCK_HALF_SIZE; tempVertices[vIdx++] = y - BLOCK_HALF_SIZE; tempVertices[vIdx++] = z - BLOCK_HALF_SIZE;
      tempVertices[vIdx++] = x + BLOCK_HALF_SIZE; tempVertices[vIdx++] = y - BLOCK_HALF_SIZE; tempVertices[vIdx++] = z - BLOCK_HALF_SIZE;
      tempVertices[vIdx++] = x + BLOCK_HALF_SIZE; tempVertices[vIdx++] = y - BLOCK_HALF_SIZE; tempVertices[vIdx++] = z + BLOCK_HALF_SIZE;
      tempVertices[vIdx++] = x - BLOCK_HALF_SIZE; tempVertices[vIdx++] = y - BLOCK_HALF_SIZE; tempVertices[vIdx++] = z + BLOCK_HALF_SIZE;
      break;
    case FACE_FRONT: 
      tempVertices[vIdx++] = x - BLOCK_HALF_SIZE; tempVertices[vIdx++] = y - BLOCK_HALF_SIZE; tempVertices[vIdx++] = z + BLOCK_HALF_SIZE;
      tempVertices[vIdx++] = x + BLOCK_HALF_SIZE; tempVertices[vIdx++] = y - BLOCK_HALF_SIZE; tempVertices[vIdx++] = z + BLOCK_HALF_SIZE;
      tempVertices[vIdx++] = x + BLOCK_HALF_SIZE; tempVertices[vIdx++] = y + BLOCK_HALF_SIZE; tempVertices[vIdx++] = z + BLOCK_HALF_SIZE;
      tempVertices[vIdx++] = x - BLOCK_HALF_SIZE; tempVertices[vIdx++] = y + BLOCK_HALF_SIZE; tempVertices[vIdx++] = z + BLOCK_HALF_SIZE;
      break;
    case FACE_BACK:
      tempVertices[vIdx++] = x - BLOCK_HALF_SIZE; tempVertices[vIdx++] = y - BLOCK_HALF_SIZE; tempVertices[vIdx++] = z - BLOCK_HALF_SIZE;
      tempVertices[vIdx++] = x - BLOCK_HALF_SIZE; tempVertices[vIdx++] = y + BLOCK_HALF_SIZE; tempVertices[vIdx++] = z - BLOCK_HALF_SIZE;
      tempVertices[vIdx++] = x + BLOCK_HALF_SIZE; tempVertices[vIdx++] = y + BLOCK_HALF_SIZE; tempVertices[vIdx++] = z - BLOCK_HALF_SIZE;
      tempVertices[vIdx++] = x + BLOCK_HALF_SIZE; tempVertices[vIdx++] = y - BLOCK_HALF_SIZE; tempVertices[vIdx++] = z - BLOCK_HALF_SIZE;
      break;
    case FACE_LEFT:
      tempVertices[vIdx++] = x - BLOCK_HALF_SIZE; tempVertices[vIdx++] = y - BLOCK_HALF_SIZE; tempVertices[vIdx++] = z - BLOCK_HALF_SIZE;
      tempVertices[vIdx++] = x - BLOCK_HALF_SIZE; tempVertices[vIdx++] = y - BLOCK_HALF_SIZE; tempVertices[vIdx++] = z + BLOCK_HALF_SIZE;
      tempVertices[vIdx++] = x - BLOCK_HALF_SIZE; tempVertices[vIdx++] = y + BLOCK_HALF_SIZE; tempVertices[vIdx++] = z + BLOCK_HALF_SIZE;
      tempVertices[vIdx++] = x - BLOCK_HALF_SIZE; tempVertices[vIdx++] = y + BLOCK_HALF_SIZE; tempVertices[vIdx++] = z - BLOCK_HALF_SIZE;
      break;
    case FACE_RIGHT:
      tempVertices[vIdx++] = x + BLOCK_HALF_SIZE; tempVertices[vIdx++] = y - BLOCK_HALF_SIZE; tempVertices[vIdx++] = z - BLOCK_HALF_SIZE;
      tempVertices[vIdx++] = x + BLOCK_HALF_SIZE; tempVertices[vIdx++] = y + BLOCK_HALF_SIZE; tempVertices[vIdx++] = z - BLOCK_HALF_SIZE;
      tempVertices[vIdx++] = x + BLOCK_HALF_SIZE; tempVertices[vIdx++] = y + BLOCK_HALF_SIZE; tempVertices[vIdx++] = z + BLOCK_HALF_SIZE;
      tempVertices[vIdx++] = x + BLOCK_HALF_SIZE; tempVertices[vIdx++] = y - BLOCK_HALF_SIZE; tempVertices[vIdx++] = z + BLOCK_HALF_SIZE;
      break;
  }
  vCount += 4;
}


static void AddCubeToMeshBuilder(World *world, Chunk *chunk, int localX, int localY, int localZ, Color color){

  int globalX = (chunk->chunkX * CHUNK_SIZE) + localX;
  int globalY = (chunk->chunkY * CHUNK_SIZE) + localY;
  int globalZ = (chunk->chunkZ * CHUNK_SIZE) + localZ;
  Vector3 pos = { (float)globalX, (float)globalY, (float)globalZ };  

  if (IsNeighbourTransparent(world, chunk, localX, localY + 1, localZ)) { AddFaceToMeshBuilder(pos, color, FACE_TOP); }
  if (IsNeighbourTransparent(world, chunk, localX, localY - 1, localZ)) { AddFaceToMeshBuilder(pos, color, FACE_BOTTOM); }
  if (IsNeighbourTransparent(world, chunk, localX + 1, localY, localZ)) { AddFaceToMeshBuilder(pos, color, FACE_RIGHT); }
  if (IsNeighbourTransparent(world, chunk, localX - 1, localY, localZ)) { AddFaceToMeshBuilder(pos, color, FACE_LEFT); }
  if (IsNeighbourTransparent(world, chunk, localX, localY, localZ + 1)) { AddFaceToMeshBuilder(pos, color, FACE_FRONT); }
  if (IsNeighbourTransparent(world, chunk, localX, localY, localZ - 1)) { AddFaceToMeshBuilder(pos, color, FACE_BACK); }
}

void BuildChunkMesh(World *world, Chunk *chunk){

  if(chunk->solidBlockCount == 0){
    UnloadChunkMesh(chunk);
    chunk->isDirty = false;
    chunk->hasMesh = false;
    return;
  }

  vCount = 0;
  iCount = 0;

  for (int x = 0; x < CHUNK_SIZE; x++) {
    for (int y = 0; y < CHUNK_SIZE; y++) {
      for (int z = 0; z < CHUNK_SIZE; z++) {
        unsigned char id = chunk->data[x][y][z];
        if (id == 0) { continue; }
        AddCubeToMeshBuilder(world, chunk, x, y, z, GetBlockDef(id)->color);
      }
    }
  }

  UnloadChunkMesh(chunk);

  chunk->isDirty = false;

  if(vCount == 0){ 
    chunk->hasMesh = false;
    return;
  }

  chunk->mesh = (Mesh){0};
  chunk->mesh.vaoId = 0;
  chunk->mesh.vertexCount = vCount;
  chunk->mesh.triangleCount = iCount / 3;

  chunk->mesh.vertices = (float *)MemAlloc((size_t)vCount * FLOATS_PER_VERTEX * sizeof(float));
  memcpy(chunk->mesh.vertices, tempVertices, (size_t)vCount * FLOATS_PER_VERTEX * sizeof(float));

  chunk->mesh.indices = (unsigned short *)MemAlloc((size_t)iCount * sizeof(unsigned short));
  memcpy(chunk->mesh.indices, tempIndices, (size_t)iCount * sizeof(unsigned short));

  chunk->mesh.colors = (unsigned char *)MemAlloc((size_t)vCount * COLOR_CHANNELS * sizeof(unsigned char));
  memcpy(chunk->mesh.colors, tempColors, (size_t)vCount * COLOR_CHANNELS * sizeof(unsigned char));

  UploadMesh(&chunk->mesh, false);

  chunk->hasMesh = true;
}

void RenderChunkMesh(Chunk *chunk){
  if(chunk->hasMesh){
    DrawMesh(chunk->mesh, chunkMaterial, MatrixIdentity());
  }
}

void UnloadChunkMesh(Chunk *chunk){
  if(chunk->hasMesh){
    UnloadMesh(chunk->mesh);
    chunk->hasMesh = false;
  }
}

void DrawWorld(World *world){
  for(int i = 0; i < world->chunkCount; i++){
    DrawChunk(world, &world->chunks[i]);
  }
}

void DrawBlockHighlight(Vector3 pos){
  DrawCubeWires(pos, BLOCK_HIGHLIGHT_SCALE, BLOCK_HIGHLIGHT_SCALE, BLOCK_HIGHLIGHT_SCALE, BLACK);
}

void DrawPlayerDebug(World *world, Player *player){
  if(!player->debug_aabb) { return; }

  Vector3 bottomPoints[COLLISION_POINTS];
  Vector3 topPoints[COLLISION_POINTS];
  Vector3 shinPoints[COLLISION_POINTS];
  Vector3 facePoints[COLLISION_POINTS];

  GetPlayerPoints(player, (PointConfig){ .radius = player->radius - VERTICAL_RADIUS_SHRINK, .yOffset = 0.0F, .epsilon = -COLLISION_EPSILON }, bottomPoints);
  GetPlayerPoints(player, (PointConfig){ .radius = player->radius - VERTICAL_RADIUS_SHRINK, .yOffset = player->size.y, .epsilon = COLLISION_EPSILON }, topPoints);

  const float LATERALDEBUGRADIUS = player->radius + 0.01F;
  GetPlayerPoints(player, (PointConfig){ .radius = LATERALDEBUGRADIUS, .yOffset = LATERAL_Y_MARGIN, .epsilon = 0.0F }, shinPoints);
  GetPlayerPoints(player, (PointConfig){ .radius = LATERALDEBUGRADIUS, .yOffset = player->size.y, .epsilon = 0.0F }, facePoints);

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

