#include "world/lod/lod_downsample.h"

#include "world/block_system.h"

_Static_assert(CHUNK_SIZE == LOD_SECTION_SIZE,
               "a chunk downsamples through the section path");

enum {
  GROUP_MEMBERS = 8,
  // Exposure score: a face toward a lower class above the voxel outweighs any
  // number of exposed side or bottom faces, so a surface layer seen from the
  // sky beats the rock beside it.
  EXPOSED_ABOVE_SCORE = 2,
  EXPOSED_SIDE_SCORE = 1
};

typedef struct {
  uint8_t ById[BLOCK_REGISTRY_SIZE];
  uint8_t Unregistered;
} LodClassTable;

static LodClassTable *GetClassTable(void) {
  static LodClassTable Table;
  return &Table;
}

static LodClass ClassifyDef(const BlockType *Def) {
  if (Def->RenderType == BLOCK_RENDER_CROSS) {
    return LOD_CLASS_AIR;
  }
  if (!Def->IsTransparent) {
    return LOD_CLASS_OPAQUE;
  }
  return Def->IsSolid ? LOD_CLASS_LEAVES : LOD_CLASS_WATER;
}

void LodDownsampleInit(void) {
  LodClassTable *Table = GetClassTable();
  for (int Id = 0; Id < BLOCK_REGISTRY_SIZE; Id++) {
    Table->ById[Id] = (uint8_t)ClassifyDef(GetBlockDef(Id));
  }
  // Air is decided by id, never by its definition.
  Table->ById[BLOCK_ID_AIR] = (uint8_t)LOD_CLASS_AIR;
  Table->Unregistered = (uint8_t)ClassifyDef(GetBlockDef(BLOCK_REGISTRY_SIZE));
}

LodClass LodClassifyBlock(BlockID Id) {
  const LodClassTable *Table = GetClassTable();
  if (Id < BLOCK_REGISTRY_SIZE) {
    return (LodClass)Table->ById[Id];
  }
  return (LodClass)Table->Unregistered;
}

LodVoxel LodCanonicalVoxel(BlockID Id, unsigned Biome) {
  if (LodClassifyBlock(Id) == LOD_CLASS_AIR) {
    return LOD_VOXEL_AIR;
  }
  return LodVoxelMake(Id, Biome);
}

typedef struct {
  LodVoxel Voxel;
  int Y;
  int Score;
  int Frequency;
} GroupCandidate;

static int ClassAt(const uint8_t *Classes, int X, int Y, int Z) {
  return Classes[LodVoxelIndex(X, Y, Z)];
}

// Faces of the voxel that touch a lower class inside the input grid; faces on
// the grid's border count as unknown, never as exposed.
static int ExposureScore(const uint8_t *Classes, int X, int Y, int Z, int Class) {
  int Score = 0;
  if (Y + 1 < LOD_SECTION_SIZE && ClassAt(Classes, X, Y + 1, Z) < Class) {
    Score += EXPOSED_ABOVE_SCORE;
  }
  bool Side = (Y > 0 && ClassAt(Classes, X, Y - 1, Z) < Class) ||
              (X > 0 && ClassAt(Classes, X - 1, Y, Z) < Class) ||
              (X + 1 < LOD_SECTION_SIZE && ClassAt(Classes, X + 1, Y, Z) < Class) ||
              (Z > 0 && ClassAt(Classes, X, Y, Z - 1) < Class) ||
              (Z + 1 < LOD_SECTION_SIZE && ClassAt(Classes, X, Y, Z + 1) < Class);
  if (Side) {
    Score += EXPOSED_SIDE_SCORE;
  }
  return Score;
}

// Strict order over candidates: exposure, then height inside the group, then
// how many members share the value, then lowest block id, then lowest biome.
static bool CandidateBeats(const GroupCandidate *A, const GroupCandidate *B) {
  if (A->Score != B->Score) {
    return A->Score > B->Score;
  }
  if (A->Y != B->Y) {
    return A->Y > B->Y;
  }
  if (A->Frequency != B->Frequency) {
    return A->Frequency > B->Frequency;
  }
  BlockID IdA = LodVoxelBlock(A->Voxel);
  BlockID IdB = LodVoxelBlock(B->Voxel);
  if (IdA != IdB) {
    return IdA < IdB;
  }
  return LodVoxelBiome(A->Voxel) < LodVoxelBiome(B->Voxel);
}

static LodVoxel ReduceGroup(const LodVoxel *In, const uint8_t *Classes, int BaseX,
                            int BaseY, int BaseZ) {
  int Members[GROUP_MEMBERS];
  int Best = LOD_CLASS_AIR;
  for (int M = 0; M < GROUP_MEMBERS; M++) {
    int X = BaseX + (M & 1);
    int Y = BaseY + ((M >> 1) & 1);
    int Z = BaseZ + ((M >> 2) & 1);
    Members[M] = LodVoxelIndex(X, Y, Z);
    if (Classes[Members[M]] > Best) {
      Best = Classes[Members[M]];
    }
  }
  if (Best == LOD_CLASS_AIR) {
    return LOD_VOXEL_AIR;
  }

  GroupCandidate Winner = {0};
  bool HaveWinner = false;
  for (int M = 0; M < GROUP_MEMBERS; M++) {
    if (Classes[Members[M]] != Best) {
      continue;
    }
    int X = BaseX + (M & 1);
    int Y = BaseY + ((M >> 1) & 1);
    int Z = BaseZ + ((M >> 2) & 1);
    GroupCandidate Candidate = {.Voxel = In[Members[M]],
                                .Y = Y,
                                .Score = ExposureScore(Classes, X, Y, Z, Best),
                                .Frequency = 0};
    for (int Other = 0; Other < GROUP_MEMBERS; Other++) {
      if (In[Members[Other]] == Candidate.Voxel) {
        Candidate.Frequency++;
      }
    }
    if (!HaveWinner || CandidateBeats(&Candidate, &Winner)) {
      Winner = Candidate;
      HaveWinner = true;
    }
  }
  return Winner.Voxel;
}

void LodDownsampleVoxels(const LodVoxel In[LOD_SECTION_VOLUME], LodPatch8 *Out) {
  uint8_t Classes[LOD_SECTION_VOLUME];
  for (int Idx = 0; Idx < LOD_SECTION_VOLUME; Idx++) {
    Classes[Idx] = (uint8_t)LodClassifyBlock(LodVoxelBlock(In[Idx]));
  }
  for (int X = 0; X < LOD_PATCH_SIZE; X++) {
    for (int Y = 0; Y < LOD_PATCH_SIZE; Y++) {
      for (int Z = 0; Z < LOD_PATCH_SIZE; Z++) {
        Out->Voxels[LodPatchIndex(X, Y, Z)] =
            ReduceGroup(In, Classes, X * 2, Y * 2, Z * 2);
      }
    }
  }
}

void LodDownsampleSnapshot(const ChunkSnapshot *Snap, LodPatch8 *Out) {
  LodVoxel In[LOD_SECTION_VOLUME];
  for (int X = 0; X < CHUNK_SIZE; X++) {
    for (int Y = 0; Y < CHUNK_SIZE; Y++) {
      for (int Z = 0; Z < CHUNK_SIZE; Z++) {
        BlockID Id = Snap->Blocks[X + 1][Y + 1][Z + 1];
        In[LodVoxelIndex(X, Y, Z)] =
            LodCanonicalVoxel(Id, SnapshotLocalBiome(Snap, X, Y, Z));
      }
    }
  }
  LodDownsampleVoxels(In, Out);
}

void LodDownsampleSection(const LodSection *Section, LodPatch8 *Out) {
  LodVoxel In[LOD_SECTION_VOLUME];
  LodSectionDecode(Section, In);
  LodDownsampleVoxels(In, Out);
}
