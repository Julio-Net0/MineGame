#ifndef RENDER_BACKEND_H
#define RENDER_BACKEND_H

#include <stdbool.h>
#include "core/vecmath.h"
#include "core/mesh_handle.h"
#include "core/color.h"

// Abstract render backend interface. The engine issues backend-agnostic mesh,
// draw, and frame commands; a single backend implementation TU (currently
// render/raylib_backend.c) fulfils them. This header exposes no renderer
// (Raylib) types, so it can be implemented by any backend (e.g. Vulkan later).

// Backend-agnostic geometry to upload. Arrays are borrowed for the duration of
// the RenderUploadMesh call (the backend copies what it needs).
typedef struct {
  const float *Vertices;          // VertexCount * 3
  const unsigned short *Indices;  // IndexCount
  const float *TexCoords;         // VertexCount * 2
  const unsigned char *Colors;    // VertexCount * 4
  const float *TexLayers;         // VertexCount * 2 (texture array layer index)
  int VertexCount;
  int IndexCount;
} MeshData;

// Abstract camera for frame setup; the backend derives view/projection.
typedef struct {
  Vec3 Position;
  Vec3 Target;
  Vec3 Up;
  float FovY;
} RenderCamera;

// Lifecycle (GPU resources, shaders, texture atlas).
void RenderBackendInit(void);
void RenderBackendShutdown(void);

// Mesh pool.
MeshHandle RenderUploadMesh(const MeshData *Data);
void RenderFreeMesh(MeshHandle Handle);
void RenderDrawMesh(MeshHandle Handle);

// Frame lifecycle. 3D content is drawn between BeginFrame and End3D; 2D content
// (deferred HUD layer) between End3D and EndFrame.
void RenderBeginFrame(RenderCamera Camera);
void RenderEnd3D(void);
void RenderEndFrame(void);

// Translucent pass state (depth-mask off, color blend on, batch flush).
void RenderBeginTranslucentPass(void);
void RenderEndTranslucentPass(void);

void RenderSetWireframe(bool Enabled);

// 3D immediate-mode helpers.
void RenderDrawBlockHighlight(Vec3 Pos);
void RenderDrawChunkBorder(Vec3 Center, float Size);
// Debug marker cube: Wire selects wireframe vs solid; Solid tints it as a
// "hit"/solid sample vs a "miss"/empty sample.
void RenderDrawDebugCube(Vec3 Pos, float Size, bool Wire, bool Solid);

// 2D helper (deferred HUD layer; backend-owned).
void RenderDrawBlockIcon(int BlockId, int X, int Y, int Size);

// 2D drawing layer (deferred HUD layer). Renderer-agnostic: all color via
// Color8, geometry via plain ints, text via C strings. HUD and chat draw
// through these instead of calling the renderer directly.
void RenderDrawRect(int X, int Y, int W, int H, Color8 Color);
void RenderDrawRectLines(int X, int Y, int W, int H, Color8 Color);
void RenderDrawRectLinesEx(int X, int Y, int W, int H, float Thick, Color8 Color);
void RenderDrawText(const char *Text, int X, int Y, int Size, Color8 Color);
int RenderMeasureText(const char *Text, int Size);

#endif
