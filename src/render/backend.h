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

// Opaque and cutout passes are normally regrouped by GPU memory block, and
// which block a mesh lands in depends on when it was uploaded. Their order
// changes no filled pixel, but it does change wireframe lines and the odd seam
// pixel, so a run that compares screenshots asks for every pass to keep the
// order meshes were added in, at the cost of more draw calls.
void RenderSetExactChunkOrder(bool Exact);

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
  int64_t UsedBytes;
  int64_t SlackBytes;
  // Block texture mip chain: levels allocated (1 when LOD is off), layers whose
  // tiles hold transparent texels, and layers whose smallest level drifted from
  // the alpha-weighted mean of the full tile (should be 0). The anisotropy the
  // LOD sampler uses (0 when there is none).
  int MipLevels;
  int MipAlphaLayers;
  int MipCheckFailures;
  float LodSamplerAnisotropy;
} RenderStats;
void RenderGetStats(RenderStats *Out);

// What the graphics device can do, detected once at backend init. Versions are
// the graphics API's: the one the context was asked for, the one the driver
// provides, and the effective one capabilities were detected at, which a
// --gl-max cap can lower below what the driver provides. With a cap given only
// features that are core at or below the effective version count; extensions
// are ignored, so the run behaves like a driver that stops there.
typedef struct {
  int RequestedMajor;
  int RequestedMinor;
  int ContextMajor;
  int ContextMinor;
  int EffectiveMajor;
  int EffectiveMinor;
  bool ContextFellBack;
  bool VersionCapped;

  // Draw records read from GPU memory, many per call.
  bool MultiDrawIndirect;
  // Draws can offset the instance index, which is how indirect draws find
  // their per-draw data without shader draw parameters.
  bool BaseInstance;
  // Compute dispatch writing a storage buffer was probed and read back.
  bool ComputeShaders;
  // Optional: draw count read from GPU memory, depth range control (reversed
  // depth), per-draw shader parameters.
  bool IndirectCount;
  bool ClipControl;
  bool ShaderDrawParameters;
  // 1 when anisotropic filtering is unavailable.
  float MaxAnisotropy;

  // The GPU-driven LOD renderer can run. When false, LodUnavailableReason says
  // which requirement failed; it is empty when true.
  bool LodSupported;
  const char *LodUnavailableReason;
} RenderCapabilities;

// Valid after RenderBackendInit; zeroed before it.
const RenderCapabilities *RenderGetCapabilities(void);

// ---------------------------------------------------------------------------
// LOD
// ---------------------------------------------------------------------------
//
// Coarse sections beyond the 1:1 chunks, drawn between the 1:1 opaque passes
// and the translucent pass. Available only when the device can draw LOD (see
// RenderCapabilities); otherwise every call below is a no-op and nothing is
// allocated.

// Whether LOD is drawn this run.
bool RenderLodActive(void);

// The LOD's edge fog at runtime: on or off, and the distance in blocks where
// it reaches the sky colour (it starts a quarter before).
void RenderLodSetFog(bool Enabled, float End);

// A LOD section's mesh in GPU memory, or RENDER_LOD_MESH_INVALID.
typedef int RenderLodMesh;
#define RENDER_LOD_MESH_INVALID (-1)

// A LOD mesh is a sequence of face runs: consecutive quads of one face group,
// in the order the mesher emitted them. The order is never changed, so culling
// whole runs cannot change which of two equal-depth fragments wins. Groups 0..5
// are the six axis directions in the packed normal's order (top, bottom, -X,
// +X, +Z, -Z); group 6 holds every other quad (cross billboards) and is never
// culled by direction. A mesh has at most RENDER_LOD_MAX_RUNS runs; the
// mesher's per-direction passes need far fewer.
enum {
  RENDER_LOD_FACE_GROUPS = 7,
  RENDER_LOD_AXIS_GROUPS = 6,
  RENDER_LOD_MAX_RUNS = 24
};

typedef struct {
  int Group;
  int Quads;
} RenderLodRun;

// Vertices are section-local voxel units, packed like chunk vertices, laid out
// as Runs describe: Runs[0, OpaqueRunCount) hold the faces drawn opaque, the
// rest the see-through faces drawn blended after them. Invalid when the LOD
// arena is full.
RenderLodMesh RenderLodUploadMesh(const ChunkVertex *Vertices,
                                  const RenderLodRun *Runs, int RunCount,
                                  int OpaqueRunCount);

// Whether a block texture layer is see-through: some texel only partly
// opaque, or most texels fully transparent (water, ice, glass, but not
// leaves). Known only on a device that draws LOD; false elsewhere.
bool RenderBlockLayerSeeThrough(int Layer);
void RenderLodFreeMesh(RenderLodMesh Mesh);

// Bumped whenever the LOD arena gains free space (pages released after their
// delay, or the arena grown): an upload that failed for room may succeed now.
uint32_t RenderLodArenaGeneration(void);

// One section to draw: its mesh, its origin in blocks and its blocks per
// voxel (2^level).
typedef struct {
  RenderLodMesh Mesh;
  int OriginX;
  int OriginY;
  int OriginZ;
  int Scale;
  int VertexCount;
} RenderLodDraw;

// Which chunks are drawn 1:1, one bit per chunk: a toroidal grid of columns
// (x and z modulo RENDER_LOD_COVERAGE_SPAN), each holding chunk layers
// -RENDER_LOD_COVERAGE_LAYER_BIAS .. +127-BIAS over four 32-bit words.
enum {
  RENDER_LOD_COVERAGE_SPAN = 512,
  RENDER_LOD_COVERAGE_WORDS = 4,
  RENDER_LOD_COVERAGE_LAYER_BIAS = 64,
  RENDER_LOD_COVERAGE_LAYERS = 128
};
void RenderLodSetCoverage(int ColumnX, int ColumnZ,
                          const uint32_t Words[RENDER_LOD_COVERAGE_WORDS]);

// Draws[0, OpaqueCount) lie wholly outside the 1:1 chunks; the BorderCount
// after them overlap some, and their fragments inside 1:1 chunks are dropped.
// ListRevision changes whenever the list's content does: an unchanged list is
// not sent to the GPU again, only culled again for the frame's camera.
// Call between the 1:1 cutout pass and the translucent pass.
void RenderDrawLod(const RenderLodDraw *Draws, int OpaqueCount, int BorderCount,
                   uint32_t ListRevision);

typedef struct {
  int Meshes;
  // Arena pages in use, the arena's current size, its ceiling, and the
  // vertex bytes the live meshes actually hold (the rest of the used pages
  // is rounding padding).
  int64_t ArenaUsedBytes;
  int64_t ArenaCapacityBytes;
  int64_t ArenaLimitBytes;
  int64_t ArenaLiveBytes;
  // Reversed-Z depth in the LOD passes.
  bool ReversedZ;
  int OpaqueDraws;
  int BorderDraws;
  // Vertices of every section handed to the last LOD pass, before culling.
  int64_t CandidateVertices;
  // The GPU cull's results as of the last RenderLodReadCullStats; CullValid
  // is false until the first one.
  bool CullValid;
  bool IndirectCount;
  int CandidateRuns;
  int FrustumCulledSections;
  int FacingCulledRuns;
  int DrawnCommands;
  int64_t DrawnVertices;
  // Two-phase Hi-Z occlusion: sections in the frustum skipped because the
  // depth drawn before them hid them, and what each phase drew.
  bool HiZ;
  int HizCulledSections;
  int Phase1Commands;
  int Phase2Commands;
  int64_t Phase1Vertices;
  int64_t Phase2Vertices;
  // The blended pass of see-through LOD faces (water, ice, glass).
  int TranslucentCommands;
  int64_t TranslucentVertices;
  // First GL error seen after a LOD pass (0 = none).
  unsigned GlError;
} RenderLodStats;
void RenderGetLodStats(RenderLodStats *Out);
// Reads the last LOD cull's counters back from the GPU. It waits for the GPU
// and the driver, so it is for automation dumps and debug views only; a normal
// frame never reads anything back.
void RenderLodReadCullStats(void);

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
