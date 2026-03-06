#include "renderer.h"
#include "block_system.h"
#include "chunk.h"
#include "raylib.h"
#include "raymath.h"
#include "player.h"
#include <stddef.h>
#include <string.h>
#include <rlgl.h>

#define ATLAS_COLS 16.0F
#define ATLAS_ROWS 16.0F

#define VERTICES_PER_FACE 4
#define FLOATS_PER_VERTEX 3
#define COLOR_CHANNELS 4
#define INDICES_PER_FACES 6
#define MAX_FACES (CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE * INDICES_PER_FACES)

#define BLOCK_HIGHLIGHT_SCALE (BLOCK_SIZE + 0.01F)

#define CHUNK_SPHERE_RADIUS 14.0F

static Texture2D blockAtlas;

static float tempTexCoords[MAX_FACES * VERTICES_PER_FACE * 2];

static float tempVertices[MAX_FACES * VERTICES_PER_FACE * FLOATS_PER_VERTEX];
static unsigned char tempColors[MAX_FACES * VERTICES_PER_FACE * COLOR_CHANNELS];
static unsigned short tempIndices[MAX_FACES * INDICES_PER_FACES];

static int vCount = 0;
static int iCount = 0;

static Material chunkMaterial;

bool debugWireFrame = false;

void InitRenderer(void){
  chunkMaterial = LoadMaterialDefault();

  blockAtlas = LoadTexture("../assets/atlas/terrain.png");
  SetTextureFilter(blockAtlas, TEXTURE_FILTER_POINT);
  chunkMaterial.maps[MATERIAL_MAP_DIFFUSE].texture = blockAtlas;
}

static void GetTextureUV(int textureIndex, Vector2 *uvMin, Vector2 *uvMax){
  float u = (float)(textureIndex % (int)ATLAS_COLS) / ATLAS_COLS;
  float v = (float)(textureIndex / (int)ATLAS_COLS) / ATLAS_ROWS;

  float w = 1.0F / ATLAS_COLS;
  float h = 1.0F / ATLAS_ROWS;

  uvMin->x = u;
  uvMin->y = v;
  uvMax->x = u + w;
  uvMax->y = v + h;
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

static void AddFaceIndices(void){
  tempIndices[iCount++] = vCount + 0;
  tempIndices[iCount++] = vCount + 1;
  tempIndices[iCount++] = vCount + 2;
  tempIndices[iCount++] = vCount + 0;
  tempIndices[iCount++] = vCount + 2;
  tempIndices[iCount++] = vCount + 3;
}

static void AddFaceTexCoords(int textureIndex){
  int uvIdx = vCount * 2;
  Vector2 uvMin;
  Vector2 uvMax;

  GetTextureUV(textureIndex, &uvMin, &uvMax);

  tempTexCoords[uvIdx++] = uvMin.x; tempTexCoords[uvIdx++] = uvMax.y;
  tempTexCoords[uvIdx++] = uvMax.x; tempTexCoords[uvIdx++] = uvMax.y;
  tempTexCoords[uvIdx++] = uvMax.x; tempTexCoords[uvIdx++] = uvMin.y;
  tempTexCoords[uvIdx++] = uvMin.x; tempTexCoords[uvIdx++] = uvMin.y;
}

static void AddFaceVerticies(Vector3 pos, BlockFace face){
  float x = pos.x;
  float y = pos.y;
  float z = pos.z;

  int vIdx = vCount * 3;

  switch (face) {
    case FACE_TOP:
      tempVertices[vIdx++] = x - BLOCK_HALF_SIZE; tempVertices[vIdx++] = y + BLOCK_HALF_SIZE; tempVertices[vIdx++] = z + BLOCK_HALF_SIZE; // Inferior-Esquerdo
      tempVertices[vIdx++] = x + BLOCK_HALF_SIZE; tempVertices[vIdx++] = y + BLOCK_HALF_SIZE; tempVertices[vIdx++] = z + BLOCK_HALF_SIZE; // Inferior-Direito
      tempVertices[vIdx++] = x + BLOCK_HALF_SIZE; tempVertices[vIdx++] = y + BLOCK_HALF_SIZE; tempVertices[vIdx++] = z - BLOCK_HALF_SIZE; // Superior-Direito
      tempVertices[vIdx++] = x - BLOCK_HALF_SIZE; tempVertices[vIdx++] = y + BLOCK_HALF_SIZE; tempVertices[vIdx++] = z - BLOCK_HALF_SIZE; // Superior-Esquerdo
      break;
    case FACE_BOTTOM:
      tempVertices[vIdx++] = x - BLOCK_HALF_SIZE; tempVertices[vIdx++] = y - BLOCK_HALF_SIZE; tempVertices[vIdx++] = z - BLOCK_HALF_SIZE; // Inferior-Esquerdo
      tempVertices[vIdx++] = x + BLOCK_HALF_SIZE; tempVertices[vIdx++] = y - BLOCK_HALF_SIZE; tempVertices[vIdx++] = z - BLOCK_HALF_SIZE; // Inferior-Direito
      tempVertices[vIdx++] = x + BLOCK_HALF_SIZE; tempVertices[vIdx++] = y - BLOCK_HALF_SIZE; tempVertices[vIdx++] = z + BLOCK_HALF_SIZE; // Superior-Direito
      tempVertices[vIdx++] = x - BLOCK_HALF_SIZE; tempVertices[vIdx++] = y - BLOCK_HALF_SIZE; tempVertices[vIdx++] = z + BLOCK_HALF_SIZE; // Superior-Esquerdo
      break;
    case FACE_FRONT: // +Z
      tempVertices[vIdx++] = x - BLOCK_HALF_SIZE; tempVertices[vIdx++] = y - BLOCK_HALF_SIZE; tempVertices[vIdx++] = z + BLOCK_HALF_SIZE; // Inferior-Esquerdo
      tempVertices[vIdx++] = x + BLOCK_HALF_SIZE; tempVertices[vIdx++] = y - BLOCK_HALF_SIZE; tempVertices[vIdx++] = z + BLOCK_HALF_SIZE; // Inferior-Direito
      tempVertices[vIdx++] = x + BLOCK_HALF_SIZE; tempVertices[vIdx++] = y + BLOCK_HALF_SIZE; tempVertices[vIdx++] = z + BLOCK_HALF_SIZE; // Superior-Direito
      tempVertices[vIdx++] = x - BLOCK_HALF_SIZE; tempVertices[vIdx++] = y + BLOCK_HALF_SIZE; tempVertices[vIdx++] = z + BLOCK_HALF_SIZE; // Superior-Esquerdo
      break;
    case FACE_BACK: // -Z
      tempVertices[vIdx++] = x + BLOCK_HALF_SIZE; tempVertices[vIdx++] = y - BLOCK_HALF_SIZE; tempVertices[vIdx++] = z - BLOCK_HALF_SIZE; // Inferior-Esquerdo
      tempVertices[vIdx++] = x - BLOCK_HALF_SIZE; tempVertices[vIdx++] = y - BLOCK_HALF_SIZE; tempVertices[vIdx++] = z - BLOCK_HALF_SIZE; // Inferior-Direito
      tempVertices[vIdx++] = x - BLOCK_HALF_SIZE; tempVertices[vIdx++] = y + BLOCK_HALF_SIZE; tempVertices[vIdx++] = z - BLOCK_HALF_SIZE; // Superior-Direito
      tempVertices[vIdx++] = x + BLOCK_HALF_SIZE; tempVertices[vIdx++] = y + BLOCK_HALF_SIZE; tempVertices[vIdx++] = z - BLOCK_HALF_SIZE; // Superior-Esquerdo
      break;
    case FACE_LEFT: // -X
      tempVertices[vIdx++] = x - BLOCK_HALF_SIZE; tempVertices[vIdx++] = y - BLOCK_HALF_SIZE; tempVertices[vIdx++] = z - BLOCK_HALF_SIZE; // Inferior-Esquerdo
      tempVertices[vIdx++] = x - BLOCK_HALF_SIZE; tempVertices[vIdx++] = y - BLOCK_HALF_SIZE; tempVertices[vIdx++] = z + BLOCK_HALF_SIZE; // Inferior-Direito
      tempVertices[vIdx++] = x - BLOCK_HALF_SIZE; tempVertices[vIdx++] = y + BLOCK_HALF_SIZE; tempVertices[vIdx++] = z + BLOCK_HALF_SIZE; // Superior-Direito
      tempVertices[vIdx++] = x - BLOCK_HALF_SIZE; tempVertices[vIdx++] = y + BLOCK_HALF_SIZE; tempVertices[vIdx++] = z - BLOCK_HALF_SIZE; // Superior-Esquerdo
      break;
    case FACE_RIGHT: // +X
      tempVertices[vIdx++] = x + BLOCK_HALF_SIZE; tempVertices[vIdx++] = y - BLOCK_HALF_SIZE; tempVertices[vIdx++] = z + BLOCK_HALF_SIZE; // Inferior-Esquerdo
      tempVertices[vIdx++] = x + BLOCK_HALF_SIZE; tempVertices[vIdx++] = y - BLOCK_HALF_SIZE; tempVertices[vIdx++] = z - BLOCK_HALF_SIZE; // Inferior-Direito
      tempVertices[vIdx++] = x + BLOCK_HALF_SIZE; tempVertices[vIdx++] = y + BLOCK_HALF_SIZE; tempVertices[vIdx++] = z - BLOCK_HALF_SIZE; // Superior-Direito
      tempVertices[vIdx++] = x + BLOCK_HALF_SIZE; tempVertices[vIdx++] = y + BLOCK_HALF_SIZE; tempVertices[vIdx++] = z + BLOCK_HALF_SIZE; // Superior-Esquerdo
      break;
  }
}

static void AddFaceToMeshBuilder(Vector3 pos, int textureIndex, BlockFace face){
  AddFaceIndices();
  AddFaceTexCoords(textureIndex);
  AddFaceVerticies(pos, face);

  vCount += 4;
}


static void AddCubeToMeshBuilder(World *world, Chunk *chunk, int localX, int localY, int localZ, BlockType *block){

  int globalX = (chunk->chunkX * CHUNK_SIZE) + localX;
  int globalY = (chunk->chunkY * CHUNK_SIZE) + localY;
  int globalZ = (chunk->chunkZ * CHUNK_SIZE) + localZ;
  Vector3 pos = { (float)globalX, (float)globalY, (float)globalZ };  

  if (IsNeighbourTransparent(world, chunk, localX, localY + 1, localZ)) { AddFaceToMeshBuilder(pos, block->texTop, FACE_TOP); }
  if (IsNeighbourTransparent(world, chunk, localX, localY - 1, localZ)) { AddFaceToMeshBuilder(pos, block->texBottom, FACE_BOTTOM); }

  if (IsNeighbourTransparent(world, chunk, localX + 1, localY, localZ)) { AddFaceToMeshBuilder(pos, block->texSide, FACE_RIGHT); }
  if (IsNeighbourTransparent(world, chunk, localX - 1, localY, localZ)) { AddFaceToMeshBuilder(pos, block->texSide, FACE_LEFT); }
  if (IsNeighbourTransparent(world, chunk, localX, localY, localZ + 1)) { AddFaceToMeshBuilder(pos, block->texSide, FACE_FRONT); }
  if (IsNeighbourTransparent(world, chunk, localX, localY, localZ - 1)) { AddFaceToMeshBuilder(pos, block->texSide, FACE_BACK); }
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
        AddCubeToMeshBuilder(world, chunk, x, y, z, GetBlockDef(id));
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

  chunk->mesh.texcoords = (float *)MemAlloc((size_t)vCount * 2 * sizeof(float));
  memcpy(chunk->mesh.texcoords, tempTexCoords, (size_t)vCount * 2 * sizeof(float));

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

static bool IsChunkInFrustum(Camera3D camera, Chunk *chunk){

  float const HALFCHUNKSIZE = CHUNK_SIZE / 2.0F;

  Vector3 chunkCenter = {
    (float)(chunk->chunkX * CHUNK_SIZE) + HALFCHUNKSIZE,
    (float)(chunk->chunkY * CHUNK_SIZE) + HALFCHUNKSIZE,
    (float)(chunk->chunkZ * CHUNK_SIZE) + HALFCHUNKSIZE,
  };

  Vector3 vecToChunk = Vector3Subtract(chunkCenter, camera.position);
  float distance = Vector3Length(vecToChunk);

  if(distance < CHUNK_SPHERE_RADIUS * 2.0F) { return true; }
  Vector3 dirToChunk = Vector3Scale(vecToChunk, 1.0F / distance);
  Vector3 camForward = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
  float dotProduct = Vector3DotProduct(camForward, dirToChunk);
  float safeMargin = CHUNK_SPHERE_RADIUS / distance;
  float limit = 0.4f - safeMargin;

  return dotProduct >= limit;
}

void DrawWorld(World *world, Camera3D camera){

  if(debugWireFrame){
    rlEnableWireMode();
  }

  for(int i = 0; i < world->chunkCount; i++){

    if(!IsChunkInFrustum(camera, &world->chunks[i])){
      continue;
    }

    DrawChunk(world, &world->chunks[i]);
  }

  if(debugWireFrame){
    rlDisableWireMode();
  }
}

void DrawBlockHighlight(Vector3 pos){
  DrawCubeWires(pos, BLOCK_HIGHLIGHT_SCALE, BLOCK_HIGHLIGHT_SCALE, BLOCK_HIGHLIGHT_SCALE, BLACK);
}

void DrawAABBDebug(World *world, Player *player){
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

void DrawBlockIcon(int blockID, int x, int y, int size) {
    if (blockID == 0) return;
    
    BlockType *def = GetBlockDef(blockID);

    Vector2 topMin;
    Vector2 topMax;
    Vector2 sideMin; 
    Vector2 sideMax;

    GetTextureUV(def->texTop, &topMin, &topMax);
    GetTextureUV(def->texSide, &sideMin, &sideMax);

    float cx = x + size / 2.0f;
    float cy = y + size / 2.0f;
    float w = size / 2.0f;
    float h = size / 4.0f;

    Vector2 pTop = {cx, y};
    Vector2 pTopLeft = {x, y + h};
    Vector2 pTopRight = {x + size, y + h};
    Vector2 pCenter = {cx, cy};
    Vector2 pBottomLeft = {x, cy + h};
    Vector2 pBottomRight = {x + size, cy + h};
    Vector2 pBottom = {cx, y + size};

    rlSetTexture(blockAtlas.id);
    rlBegin(RL_QUADS);

    rlColor4ub(255, 255, 255, 255);
    rlTexCoord2f(topMin.x, topMin.y); rlVertex2f(pTop.x, pTop.y);
    rlTexCoord2f(topMin.x, topMax.y); rlVertex2f(pTopLeft.x, pTopLeft.y);
    rlTexCoord2f(topMax.x, topMax.y); rlVertex2f(pCenter.x, pCenter.y);
    rlTexCoord2f(topMax.x, topMin.y); rlVertex2f(pTopRight.x, pTopRight.y);

    rlColor4ub(150, 150, 150, 255); 
    rlTexCoord2f(sideMin.x, sideMin.y); rlVertex2f(pTopLeft.x, pTopLeft.y);
    rlTexCoord2f(sideMin.x, sideMax.y); rlVertex2f(pBottomLeft.x, pBottomLeft.y);
    rlTexCoord2f(sideMax.x, sideMax.y); rlVertex2f(pBottom.x, pBottom.y);
    rlTexCoord2f(sideMax.x, sideMin.y); rlVertex2f(pCenter.x, pCenter.y);

    rlColor4ub(200, 200, 200, 255); 
    rlTexCoord2f(sideMin.x, sideMin.y); rlVertex2f(pCenter.x, pCenter.y);
    rlTexCoord2f(sideMin.x, sideMax.y); rlVertex2f(pBottom.x, pBottom.y);
    rlTexCoord2f(sideMax.x, sideMax.y); rlVertex2f(pBottomRight.x, pBottomRight.y);
    rlTexCoord2f(sideMax.x, sideMin.y); rlVertex2f(pTopRight.x, pTopRight.y);

    rlEnd();
    rlSetTexture(0);
}
