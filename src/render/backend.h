#ifndef RENDER_BACKEND_H
#define RENDER_BACKEND_H

#include <stdbool.h>
#include <stdint.h>
#include "core/vecmath.h"
#include "core/mesh_handle.h"
#include "core/color.h"
#include "core/image.h"
#include "ui/draw_list.h"

// Abstract render backend interface. The engine issues backend-agnostic mesh,
// draw, and frame commands; a single backend implementation TU (currently
// render/gl_backend.c) fulfils them. This header exposes no graphics-API type,
// so it can be implemented by any backend (e.g. Vulkan later).

// Sized to what a 16-bit index buffer can address (65532 vertices = 16383
// four-vertex quads). Shared between the mesher and the backend's static EBO.
enum {
  CHUNK_VERTICES_PER_QUAD = 4,
  CHUNK_INDICES_PER_QUAD = 6,
  CHUNK_MAX_QUADS = 16383,
  CHUNK_MAX_VERTICES = CHUNK_MAX_QUADS * CHUNK_VERTICES_PER_QUAD,
  CHUNK_MAX_INDICES = CHUNK_MAX_QUADS * CHUNK_INDICES_PER_QUAD
};

// 8-byte packed chunk vertex (uvec2 in chunk.vert):
//   Word0: X(5b) | Y(5b) | Z(5b) | U(5b) | V(5b) | AO(2b) | Normal(3b)
//   Word1: TexLayer(12b) | OverlaySlot(4b, 0 = none) | TintRGB565(16b)
// The overlay slot indexes a 16-entry slot -> layer table uploaded as a shader
// uniform (see GetOverlaySlotLayers), so overlays cost 4 bits, not a layer.
typedef struct {
  uint32_t Word0;
  uint32_t Word1;
} ChunkVertex;

// Backend-agnostic geometry to upload. Borrowed for the duration of the
// RenderUploadMesh call (the backend uploads it directly to the GPU). Origin is
// the world-space position the chunk-local vertex coordinates are relative to;
// it belongs to the mesh, so drawing the mesh later needs only its handle.
typedef struct {
  const ChunkVertex *Vertices;
  int VertexCount;
  Vec3 Origin;
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

// Adds one chunk mesh to the pass in progress. Valid only between
// RenderBeginChunkPass and RenderEndChunkPass; the pass is drawn when it ends,
// in as few draw calls as the active render tier allows. Meshes keep the order
// they were added in wherever that order affects the image.
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

// Most recent completed occlusion query for the solid chunk pass: how many
// fragments passed the depth test, and that count divided by the current
// framebuffer's pixel count. Read from an earlier frame so the query never
// stalls the pipeline; false before the first result is ready or when built
// with profiling disabled.
bool RenderQuerySolidSamples(uint64_t *OutSamples, float *OutOverdraw);

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

// Frame capture, for an automated run that has to look at what was drawn.
// RenderRequestCapture marks the frame currently being built to be read back;
// RenderEndFrame performs the read after the UI is submitted and before the
// buffers are swapped, so the pixels are exactly the frame being presented, UI
// included, and a frame nobody asked about reads nothing back.
//
// RenderTakeCapture then hands those pixels over once: on true it has written a
// top-row-first RGBA8 image into Out and given up ownership of it, so the caller
// releases it with CoreImageFree. False means no capture is waiting, which is
// also the answer while the requested frame has not finished yet.
void RenderRequestCapture(void);
bool RenderTakeCapture(CoreImage *Out);

// Render tier the backend chose at startup, and the state of the GPU memory
// chunk meshes live in, for the debug overlay and scripted state dumps.
typedef struct {
  int Tier;
  int ArenaBlocks;
  int64_t UsedPages;
  int64_t FreePages;
  int64_t PendingPages;
  int64_t SlackBytes;
} RenderStats;
void RenderGetStats(RenderStats *Out);

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

// The block-icon atlas holds texture layer i in cell (i % GRID, i / GRID) of a
// GRID x GRID grid, which is all the UI needs to address it. GRID grows with the
// registered texture count (never below 16) and is fixed once the backend has
// built the atlas.
int RenderGetBlockIconGrid(void);

// Registered block textures that resolved to the checkerboard fallback at
// startup, so a test run can fail on a missing or misspelled texture.
int RenderGetMissingBlockTextureCount(void);

// Driver-reported device and API version strings, for logs and for tagging a
// golden capture with the GPU it was taken on. Never NULL; empty before init.
const char *RenderGetDeviceName(void);
const char *RenderGetApiVersion(void);

// UI layer. The GUI atlas is uploaded once at startup; each frame's UI reaches
// the backend as one draw list, drawn after the 3D pass with alpha blending and
// no depth test.
void RenderUploadUiAtlas(const CoreImage *Atlas);
void RenderSubmitUI(const UiDrawList *List);

#endif
