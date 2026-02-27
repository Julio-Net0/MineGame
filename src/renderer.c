#include "renderer.h"
#include "block_system.h"
#include "raylib.h"
#include "raymath.h"
#include <string.h>

#define MAX_FACES (CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE * 6)

static float tempVertices[MAX_FACES * 4 * 3];
static unsigned char tempColors[MAX_FACES * 4 * 4];
static unsigned short tempIndices[MAX_FACES * 6];

static int vCount = 0;
static int iCount = 0;

static Material chunkMaterial;

void InitRenderer(void){
  chunkMaterial = LoadMaterialDefault();
}

bool IsNeighbourTransparent(World *world, Chunk *chunk, int localX, int localY, int localZ){
  if(localX >= 0 && localX < CHUNK_SIZE && localY >= 0 && localY < CHUNK_SIZE && localZ >= 0 && localZ < CHUNK_SIZE){
      unsigned char id = chunk->data[localX][localY][localZ];
      if(id == 0) { return true; }
      return GetBlockDef(id)->isTransparent;
  }

  int globalX = (chunk->chunkX * CHUNK_SIZE) + localX;
  int globalZ = (chunk->chunkZ * CHUNK_SIZE) + localZ;
  
  int id = GetBlockIDFromWorld(world, (Vector3){(float)globalX, (float)localY, (float)globalZ});
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
      tempVertices[vIdx++] = x - 0.5f; tempVertices[vIdx++] = y + 0.5f; tempVertices[vIdx++] = z - 0.5f;
      tempVertices[vIdx++] = x - 0.5f; tempVertices[vIdx++] = y + 0.5f; tempVertices[vIdx++] = z + 0.5f;
      tempVertices[vIdx++] = x + 0.5f; tempVertices[vIdx++] = y + 0.5f; tempVertices[vIdx++] = z + 0.5f;
      tempVertices[vIdx++] = x + 0.5f; tempVertices[vIdx++] = y + 0.5f; tempVertices[vIdx++] = z - 0.5f;
      break;
    case FACE_BOTTOM:
      tempVertices[vIdx++] = x - 0.5f; tempVertices[vIdx++] = y - 0.5f; tempVertices[vIdx++] = z - 0.5f;
      tempVertices[vIdx++] = x + 0.5f; tempVertices[vIdx++] = y - 0.5f; tempVertices[vIdx++] = z - 0.5f;
      tempVertices[vIdx++] = x + 0.5f; tempVertices[vIdx++] = y - 0.5f; tempVertices[vIdx++] = z + 0.5f;
      tempVertices[vIdx++] = x - 0.5f; tempVertices[vIdx++] = y - 0.5f; tempVertices[vIdx++] = z + 0.5f;
      break;
    case FACE_FRONT: 
      tempVertices[vIdx++] = x - 0.5f; tempVertices[vIdx++] = y - 0.5f; tempVertices[vIdx++] = z + 0.5f;
      tempVertices[vIdx++] = x + 0.5f; tempVertices[vIdx++] = y - 0.5f; tempVertices[vIdx++] = z + 0.5f;
      tempVertices[vIdx++] = x + 0.5f; tempVertices[vIdx++] = y + 0.5f; tempVertices[vIdx++] = z + 0.5f;
      tempVertices[vIdx++] = x - 0.5f; tempVertices[vIdx++] = y + 0.5f; tempVertices[vIdx++] = z + 0.5f;
      break;
    case FACE_BACK:
      tempVertices[vIdx++] = x - 0.5f; tempVertices[vIdx++] = y - 0.5f; tempVertices[vIdx++] = z - 0.5f;
      tempVertices[vIdx++] = x - 0.5f; tempVertices[vIdx++] = y + 0.5f; tempVertices[vIdx++] = z - 0.5f;
      tempVertices[vIdx++] = x + 0.5f; tempVertices[vIdx++] = y + 0.5f; tempVertices[vIdx++] = z - 0.5f;
      tempVertices[vIdx++] = x + 0.5f; tempVertices[vIdx++] = y - 0.5f; tempVertices[vIdx++] = z - 0.5f;
      break;
    case FACE_LEFT:
      tempVertices[vIdx++] = x - 0.5f; tempVertices[vIdx++] = y - 0.5f; tempVertices[vIdx++] = z - 0.5f;
      tempVertices[vIdx++] = x - 0.5f; tempVertices[vIdx++] = y - 0.5f; tempVertices[vIdx++] = z + 0.5f;
      tempVertices[vIdx++] = x - 0.5f; tempVertices[vIdx++] = y + 0.5f; tempVertices[vIdx++] = z + 0.5f;
      tempVertices[vIdx++] = x - 0.5f; tempVertices[vIdx++] = y + 0.5f; tempVertices[vIdx++] = z - 0.5f;
      break;
    case FACE_RIGHT:
      tempVertices[vIdx++] = x + 0.5f; tempVertices[vIdx++] = y - 0.5f; tempVertices[vIdx++] = z - 0.5f;
      tempVertices[vIdx++] = x + 0.5f; tempVertices[vIdx++] = y + 0.5f; tempVertices[vIdx++] = z - 0.5f;
      tempVertices[vIdx++] = x + 0.5f; tempVertices[vIdx++] = y + 0.5f; tempVertices[vIdx++] = z + 0.5f;
      tempVertices[vIdx++] = x + 0.5f; tempVertices[vIdx++] = y - 0.5f; tempVertices[vIdx++] = z + 0.5f;
      break;
  }
  vCount += 4;
}


void AddCubeToMeshBuilder(World *world, Chunk *chunk, int localX, int localY, int localZ, Color color){

  int globalX = (chunk->chunkX * CHUNK_SIZE) + localX;
  int globalZ = (chunk->chunkZ * CHUNK_SIZE) + localZ;
  Vector3 pos = { (float)globalX, (float)localY, (float)globalZ };  

  if (IsNeighbourTransparent(world, chunk, localX, localY + 1, localZ)) { AddFaceToMeshBuilder(pos, color, FACE_TOP); }
  if (IsNeighbourTransparent(world, chunk, localX, localY - 1, localZ)) { AddFaceToMeshBuilder(pos, color, FACE_BOTTOM); }
  if (IsNeighbourTransparent(world, chunk, localX + 1, localY, localZ)) { AddFaceToMeshBuilder(pos, color, FACE_RIGHT); }
  if (IsNeighbourTransparent(world, chunk, localX - 1, localY, localZ)) { AddFaceToMeshBuilder(pos, color, FACE_LEFT); }
  if (IsNeighbourTransparent(world, chunk, localX, localY, localZ + 1)) { AddFaceToMeshBuilder(pos, color, FACE_FRONT); }
  if (IsNeighbourTransparent(world, chunk, localX, localY, localZ - 1)) { AddFaceToMeshBuilder(pos, color, FACE_BACK); }
}

void BuildChunkMesh(World *world, Chunk *chunk){

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

  if(vCount == 0) { return; }

  chunk->mesh = (Mesh){0};
  chunk->mesh.vertexCount = vCount;
  chunk->mesh.triangleCount = iCount / 3;

  chunk->mesh.vertices = (float *)MemAlloc(vCount * 3 * sizeof(float));
  memcpy(chunk->mesh.vertices, tempVertices, vCount * 3 * sizeof(float));

  chunk->mesh.indices = (unsigned short *)MemAlloc(iCount * sizeof(unsigned short));
  memcpy(chunk->mesh.indices, tempIndices, iCount * sizeof(unsigned short));

  chunk->mesh.colors = (unsigned char *)MemAlloc(vCount * 4 * sizeof(unsigned char));
  memcpy(chunk->mesh.colors, tempColors, vCount * 4 * sizeof(unsigned char));

  UploadMesh(&chunk->mesh, false);

  chunk->hasMesh = true;
  chunk->isDirty = false;
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

void DrawBlockHighlight(Vector3 pos){
  DrawCubeWires(pos, 1.01F, 1.01F, 1.01F, BLACK);
}
