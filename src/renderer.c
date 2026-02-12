#include "renderer.h"
#include "block_system.h"
#include "rlgl.h"
#include "world.h"

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

bool IsNeighbourTransparent(Chunk *chunk, int x, int y, int z){
  if (x < 0 || x >= CHUNK_SIZE || y < 0 || y >= CHUNK_SIZE || z < 0 || z >= CHUNK_SIZE) {
    return true; 
  }

  int blockID = chunk->data[x][y][z];
  return GetBlockDef(blockID)->isTransparent;
}

void DrawCubeCulled(Chunk *chunk, int x, int y, int z, Color color){
  Vector3 pos = {
    chunk->position.x + x,
    chunk->position.y + y,
    chunk->position.z + z
  };  

  if (IsNeighbourTransparent(chunk, x, y + 1, z)){ DrawBlockFace(pos, color, FACE_TOP);}
  if (IsNeighbourTransparent(chunk, x, y - 1, z)){ DrawBlockFace(pos, color, FACE_BOTTOM);}
  if (IsNeighbourTransparent(chunk, x + 1, y, z)){ DrawBlockFace(pos, color, FACE_RIGHT);}
  if (IsNeighbourTransparent(chunk, x - 1, y, z)){ DrawBlockFace(pos, color, FACE_LEFT);}
  if (IsNeighbourTransparent(chunk, x, y, z + 1)){ DrawBlockFace(pos, color, FACE_FRONT);}
  if (IsNeighbourTransparent(chunk, x, y, z - 1)){ DrawBlockFace(pos, color, FACE_BACK);}
}


