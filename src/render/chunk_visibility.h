#ifndef RENDER_CHUNK_VISIBILITY_H
#define RENDER_CHUNK_VISIBILITY_H

#include "world/chunk.h"
#include "world/chunk_snapshot.h"
#include <stdbool.h>
#include <stdint.h>

// Which faces of a chunk can see each other through it.
//
// Two faces are connected when a 6-connected path of non-opaque cells links a
// cell on one to a cell on the other. The occlusion traversal only leaves a
// chunk through a face connected to the one it entered by, so a chunk whose
// faces are all walled off by rock hides everything behind it.
//
// The 15 unordered face pairs occupy bits 0-14. Bit 15 marks a result that was
// actually computed, so the zero value means "never analysed" and reads as
// fully connected: a chunk nobody has looked at yet must never hide anything.

typedef enum {
  CHUNK_FACE_NEG_X = 0,
  CHUNK_FACE_POS_X,
  CHUNK_FACE_NEG_Y,
  CHUNK_FACE_POS_Y,
  CHUNK_FACE_NEG_Z,
  CHUNK_FACE_POS_Z,
  CHUNK_FACE_COUNT
} ChunkFace;

typedef uint16_t FaceVisibility;

#define FACE_VIS_UNKNOWN ((FaceVisibility)0x0000U)
#define FACE_VIS_PAIRS ((FaceVisibility)0x7FFFU)
#define FACE_VIS_KNOWN ((FaceVisibility)0x8000U)
#define FACE_VIS_ALL ((FaceVisibility)(FACE_VIS_PAIRS | FACE_VIS_KNOWN))
#define FACE_VIS_NONE FACE_VIS_KNOWN

enum {
  FACE_VIS_WORDS = (int)(CHUNK_VOLUME / 64)
};

// Per-thread flood-fill scratch; one lives in each mesher slot.
typedef struct {
  uint64_t Blocked[FACE_VIS_WORDS];
  uint16_t Stack[CHUNK_VOLUME];
} FaceVisScratch;

// Main thread, after block definitions load and before the mesh workers start:
// resolves which block ids occlude. Read-only afterwards.
void InitChunkVisibility(void);

bool IsBlockOpaqueForVisibility(BlockID Id);

// The face on the other side of a shared boundary: -X of one chunk touches +X of
// its neighbour.
static inline ChunkFace OppositeChunkFace(ChunkFace Face) {
  return (ChunkFace)((int)Face ^ 1);
}

FaceVisibility FacePairBit(ChunkFace A, ChunkFace B);

// True when A and B are connected, or when Vis was never computed.
bool FaceVisConnects(FaceVisibility Vis, ChunkFace A, ChunkFace B);

// Faces a chunk can be left through when entered through any face in
// EntryMask (one bit per ChunkFace). Never-analysed chunks connect everything.
unsigned FaceVisExits(FaceVisibility Vis, unsigned EntryMask);

// Worker-safe: reads only the snapshot's own 16^3 cells and the scratch.
FaceVisibility ComputeFaceVisibility(const ChunkSnapshot *Snap,
                                     FaceVisScratch *Scratch);

// Checks ComputeFaceVisibility against hand-built snapshots using real block
// ids (stone, water, glass, a cross flower). Logs each failure; returns false if
// any case disagrees with its expected pair set.
bool RunFaceVisibilitySelfTest(void);

#endif
