#include "renderer.h"
#include "block_system.h"
#include "chunk.h"
#include "raylib.h"
#include "rlgl.h"

void DrawBlockFace(Vector3 pos, Color color, BlockFace face){
  float x = pos.x; float y = pos.y; float z = pos.z;

  rlBegin(RL_QUADS);
  rlColor4ub(color.r, color.g, color.b, color.a);

  switch (face) {
    case FACE_TOP:
      rlVertex3f(x - 0.5F, y + 0.5F, z - 0.5F);
      rlVertex3f(x - 0.5F, y + 0.5F, z + 0.5F);
      rlVertex3f(x + 0.5F, y + 0.5F, z + 0.5F);
      rlVertex3f(x + 0.5F, y + 0.5F, z - 0.5F);
      break;
    case FACE_BOTTOM:
      rlVertex3f(x - 0.5F, y - 0.5F, z - 0.5F);
      rlVertex3f(x + 0.5F, y - 0.5F, z - 0.5F);
      rlVertex3f(x + 0.5F, y - 0.5F, z + 0.5F);
      rlVertex3f(x - 0.5F, y - 0.5F, z + 0.5F);
      break;
    case FACE_FRONT:
      rlVertex3f(x - 0.5F, y - 0.5F, z + 0.5F);
      rlVertex3f(x + 0.5F, y - 0.5F, z + 0.5F);
      rlVertex3f(x + 0.5F, y + 0.5F, z + 0.5F);
      rlVertex3f(x - 0.5F, y + 0.5F, z + 0.5F);
      break;
    case FACE_BACK:
      rlVertex3f(x - 0.5F, y - 0.5F, z - 0.5F);
      rlVertex3f(x - 0.5F, y + 0.5F, z - 0.5F);
      rlVertex3f(x + 0.5F, y + 0.5F, z - 0.5F);
      rlVertex3f(x + 0.5F, y - 0.5F, z - 0.5F);
      break;
    case FACE_LEFT:
      rlVertex3f(x - 0.5F, y - 0.5F, z - 0.5F);
      rlVertex3f(x - 0.5F, y - 0.5F, z + 0.5F);
      rlVertex3f(x - 0.5F, y + 0.5F, z + 0.5F);
      rlVertex3f(x - 0.5F, y + 0.5F, z - 0.5F);
      break;
    case FACE_RIGHT:
      rlVertex3f(x + 0.5F, y - 0.5F, z - 0.5F);
      rlVertex3f(x + 0.5F, y + 0.5F, z - 0.5F);
      rlVertex3f(x + 0.5F, y + 0.5F, z + 0.5F);
      rlVertex3f(x + 0.5F, y - 0.5F, z + 0.5F);
      break;
  }
  rlEnd();
}

bool IsNeighbourTransparent(World *world, Chunk *chunk, int localX, int localY, int localZ){
  if(localX >= 0 && localX < CHUNK_SIZE && localY >= 0 && localY < CHUNK_SIZE && localZ >= 0 && localZ < CHUNK_SIZE){
      unsigned char id = chunk->data[localX][localY][localZ];
      if(id == 0) return true;
      return GetBlockDef(id)->isTransparent;
  }

  int globalX = (chunk->chunkX * CHUNK_SIZE) + localX;
  int globalZ = (chunk->chunkZ * CHUNK_SIZE) + localZ;
  
  int id = GetBlockIDFromWorld(world, (Vector3){(float)globalX, (float)localY, (float)globalZ});
  if(id == 0) return true;
  return GetBlockDef(id)->isTransparent;
}

void DrawCubeCulled(World *world, Chunk *chunk, int localX, int localY, int localZ, Color color){
  
  int globalX = (chunk->chunkX * CHUNK_SIZE) + localX;
  int globalY = localY;
  int globalZ = (chunk->chunkZ * CHUNK_SIZE) + localZ;

  Vector3 pos = { (float)globalX, (float)globalY, (float)globalZ };  

  if (IsNeighbourTransparent(world, chunk, localX, localY + 1, localZ)) DrawBlockFace(pos, color, FACE_TOP);
  if (IsNeighbourTransparent(world, chunk, localX, localY - 1, localZ)) DrawBlockFace(pos, color, FACE_BOTTOM);
  if (IsNeighbourTransparent(world, chunk, localX + 1, localY, localZ)) DrawBlockFace(pos, color, FACE_RIGHT);
  if (IsNeighbourTransparent(world, chunk, localX - 1, localY, localZ)) DrawBlockFace(pos, color, FACE_LEFT);
  if (IsNeighbourTransparent(world, chunk, localX, localY, localZ + 1)) DrawBlockFace(pos, color, FACE_FRONT);
  if (IsNeighbourTransparent(world, chunk, localX, localY, localZ - 1)) DrawBlockFace(pos, color, FACE_BACK);
}

void DrawBlockHighlight(Vector3 pos){
  DrawCubeWires(pos, 1.01F, 1.01F, 1.01F, BLACK);
}
