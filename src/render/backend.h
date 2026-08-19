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

// Draws one chunk mesh. Valid only between RenderBeginChunkPass and
// RenderEndChunkPass, which establish the shader, texture and matrix every chunk
// shares.
void RenderDrawMesh(MeshHandle Handle);

// Which fragment shader a chunk pass binds. The opaque variant omits the alpha
// test so the hardware keeps early depth rejection; the cutout variant keeps it
// for geometry that genuinely discards texels.
typedef enum {
  CHUNK_PASS_OPAQUE,
  CHUNK_PASS_CUTOUT
} ChunkPassKind;

// Bind the state every chunk draw in the pass shares: shader, block texture
// array, and view-projection matrix. Setting these per chunk instead set
// identical values hundreds of times a frame.
//
// Nothing between begin and end may issue a draw that rebinds the shader, the
// texture unit or the matrix uniform. The immediate-mode helpers below all do,
// so they belong outside the pass -- or the pass must be re-established after
// them.
void RenderBeginChunkPass(ChunkPassKind Kind);
void RenderEndChunkPass(void);

// Combined view-projection for the current frame, column-major, so the engine
// can derive frustum planes without knowing the backend's matrix type.
enum {
  RENDER_MATRIX_ELEMENTS = 16
};
void RenderGetViewProjection(float OutMatrix[RENDER_MATRIX_ELEMENTS]);

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
// Colored wireframe box centered at Center with the given per-axis Size. When
// XRay is true it is drawn without depth testing so it shows through blocks.
void RenderDrawWireBox(Vec3 Center, Vec3 Size, Color8 Color, bool XRay);

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
