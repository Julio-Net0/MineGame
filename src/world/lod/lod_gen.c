#include "world/lod/lod_gen.h"

#include "world/biome.h"
#include "world/chunk.h"
#include "world/lod/lod_downsample.h"
#include "world/lod/lod_store.h"
#include "world/stone_filler.h"
#include "world/terrain_density.h"
#include "world/worldgen_strata.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>

enum {
  // Voxel columns of a section plus a one-column ring for neighbour tests.
  RING_COLUMNS = LOD_SECTION_SIZE + 2,
  // Shared corner samples around those columns.
  RING_CORNERS = RING_COLUMNS + 1,
  // Own corner samples: two per voxel column and axis.
  RING_OWN = RING_COLUMNS * 2,
  // Levels whose voxels sample their own footprint's corner columns; coarser
  // levels share corners between neighbouring voxels, at a quarter of the
  // probes.
  OWN_CORNER_MAX_LEVEL = 3,
  // Section rows kept below the lowest exposed voxel, so a coarser parent's
  // slightly lower cliff still finds rock to refine into.
  FLOOR_MARGIN_VOXELS = LOD_CELL_SIZE,
  SECTION_SHIFT = 4,
  // Below any overworld floor: the stand-in top of a column with no terrain.
  NO_TOP = -(1 << 20)
};

typedef struct {
  // Terrain top (block Y) of the column, the highest of its four corners.
  int Top;
  // Voxel index (absolute, level units) of the top.
  int TopVoxel;
} ColumnTop;

typedef struct {
  const BiomeType *Surface;
  int SurfaceBiome;
  int SeaBiome;
  bool Frozen;
  bool Steep;
  // Lowest voxel index whose solid voxel has a non-solid side neighbour.
  int SideExposedAbove;
} ColumnInfo;

struct LodGenScratch {
  // [RING_CORNERS]^2 shared or [RING_OWN]^2 own corner tops.
  int CornerTops[RING_OWN][RING_OWN];
  ColumnTop Columns[RING_COLUMNS][RING_COLUMNS];
  ColumnInfo Info[LOD_SECTION_SIZE][LOD_SECTION_SIZE];
  BlockID FillerRow[LOD_SECTION_SIZE];
  LodVoxel Voxels[LOD_SECTION_VOLUME];
  FillerCube Filler;
  int FillerChunkY;
  bool FillerValid;
};

LodGenScratch *LodGenScratchCreate(void) {
  LodGenScratch *Scratch = (LodGenScratch *)malloc(sizeof(LodGenScratch));
  if (Scratch != NULL) {
    Scratch->FillerValid = false;
  }
  return Scratch;
}

void LodGenScratchFree(LodGenScratch *Scratch) { free(Scratch); }

static int MinInt(int A, int B) { return (A < B) ? A : B; }
static int MaxInt(int A, int B) { return (A > B) ? A : B; }

// The highest voxel wholly at or below the sea level. Every level's sea then
// ends at the same height, the top of the sea level block row's voxel
// boundary, so the water of neighbouring levels meets without a step (a voxel
// merely touching the sea would put each level's surface 2^L blocks higher,
// and the crack between them shows the sea floor).
static int SeaTopVoxel(int Level) {
  return LodFloorShift(SEA_LEVEL + 1, Level) - 1;
}

static int SampleTop(int X, int Z, uint64_t Seed) {
  int Top = GetTerrainTopOnLattice(X, Z, Seed);
  return (Top == INT_MIN) ? NO_TOP : Top;
}

static void SetColumn(LodGenScratch *S, int A, int B, int Level, int Top) {
  S->Columns[A][B] = (ColumnTop){.Top = Top, .TopVoxel = LodFloorShift(Top, Level)};
}

// A voxel column is as high as the highest of its four corner samples. Up to
// OWN_CORNER_MAX_LEVEL those are the first and last block columns of its own
// footprint on each axis (at level 1, every column it covers); beyond, the
// corners shared with the neighbouring voxels, the far ones a block outside.
static void SampleColumnTops(LodGenScratch *S, int Level, int OriginX,
                             int OriginZ, uint64_t Seed, LodGenResult *Out) {
  int Step = 1 << Level;
  if (Level <= OWN_CORNER_MAX_LEVEL) {
    for (int A = 0; A < RING_OWN; A++) {
      for (int B = 0; B < RING_OWN; B++) {
        int X = (((A / 2) - 1) * Step) + (((A & 1) != 0) ? Step - 1 : 0);
        int Z = (((B / 2) - 1) * Step) + (((B & 1) != 0) ? Step - 1 : 0);
        S->CornerTops[A][B] = SampleTop(OriginX + X, OriginZ + Z, Seed);
      }
    }
    Out->TopSamples += RING_OWN * RING_OWN;
    for (int A = 0; A < RING_COLUMNS; A++) {
      for (int B = 0; B < RING_COLUMNS; B++) {
        int Near = 2 * A;
        int Far = Near + 1;
        int Left = 2 * B;
        int Right = Left + 1;
        SetColumn(S, A, B, Level,
                  MaxInt(MaxInt(S->CornerTops[Near][Left], S->CornerTops[Near][Right]),
                         MaxInt(S->CornerTops[Far][Left], S->CornerTops[Far][Right])));
      }
    }
    return;
  }
  for (int A = 0; A < RING_CORNERS; A++) {
    for (int B = 0; B < RING_CORNERS; B++) {
      S->CornerTops[A][B] =
          SampleTop(OriginX + ((A - 1) * Step), OriginZ + ((B - 1) * Step), Seed);
    }
  }
  Out->TopSamples += RING_CORNERS * RING_CORNERS;
  for (int A = 0; A < RING_COLUMNS; A++) {
    for (int B = 0; B < RING_COLUMNS; B++) {
      SetColumn(S, A, B, Level,
                MaxInt(MaxInt(S->CornerTops[A][B], S->CornerTops[A + 1][B]),
                       MaxInt(S->CornerTops[A][B + 1], S->CornerTops[A + 1][B + 1])));
    }
  }
}

// Slope from the neighbouring columns' tops, against the biome threshold the
// generator applies to the density gradient (for a height field the two are
// the same ratio).
static bool ColumnSteep(const LodGenScratch *S, int A, int B, int Step) {
  float Run = (float)(2 * Step);
  float Gx = (float)(S->Columns[A + 1][B].Top - S->Columns[A - 1][B].Top) / Run;
  float Gz = (float)(S->Columns[A][B + 1].Top - S->Columns[A][B - 1].Top) / Run;
  return hypotf(Gx, Gz) > GetBiomeParams()->SteepSlope;
}

static void ResolveColumns(LodGenScratch *S, int Level, int OriginX,
                           int OriginZ, uint64_t Seed, LodGenResult *Out) {
  int Step = 1 << Level;
  int Half = Step / 2;
  int SeaVoxel = SeaTopVoxel(Level);
  for (int I = 0; I < LOD_SECTION_SIZE; I++) {
    for (int K = 0; K < LOD_SECTION_SIZE; K++) {
      const ColumnTop *Column = &S->Columns[I + 1][K + 1];
      int CentreX = OriginX + (I * Step) + Half;
      int CentreZ = OriginZ + (K * Step) + Half;
      int Biome = SampleBiomeAt(CentreX, Column->Top, CentreZ, Seed, NULL);
      int SeaBiome = Biome;
      Out->BiomeSamples++;
      if (Column->TopVoxel < SeaVoxel) {
        SeaBiome = SampleBiomeAt(CentreX, SEA_LEVEL, CentreZ, Seed, NULL);
        Out->BiomeSamples++;
      }
      int Lowest = MinInt(MinInt(S->Columns[I][K + 1].TopVoxel,
                                 S->Columns[I + 2][K + 1].TopVoxel),
                          MinInt(S->Columns[I + 1][K].TopVoxel,
                                 S->Columns[I + 1][K + 2].TopVoxel));
      S->Info[I][K] = (ColumnInfo){.Surface = GetBiomeDef(Biome),
                                   .SurfaceBiome = Biome,
                                   .SeaBiome = SeaBiome,
                                   .Frozen = GetBiomeDef(SeaBiome)->FrozenWater,
                                   .Steep = ColumnSteep(S, I + 1, K + 1, Step),
                                   .SideExposedAbove = Lowest};
    }
  }
}

// The section rows from just below the lowest exposed voxel to the highest
// solid or water voxel.
static void ColumnRange(const LodGenScratch *S, int Level, int *OutFirst,
                        int *OutLast) {
  int SeaVoxel = SeaTopVoxel(Level);
  int Low = INT_MAX;
  int High = INT_MIN;
  for (int I = 0; I < LOD_SECTION_SIZE; I++) {
    for (int K = 0; K < LOD_SECTION_SIZE; K++) {
      int TopVoxel = S->Columns[I + 1][K + 1].TopVoxel;
      Low = MinInt(Low, MinInt(TopVoxel, S->Info[I][K].SideExposedAbove + 1));
      High = MaxInt(High, MaxInt(TopVoxel, SeaVoxel));
    }
  }
  int First = LodFloorShift(Low - FLOOR_MARGIN_VOXELS, SECTION_SHIFT);
  int Last = LodFloorShift(High, SECTION_SHIFT);
  if (Last - First + 1 > LOD_GEN_MAX_SECTIONS) {
    First = Last - LOD_GEN_MAX_SECTIONS + 1;
  }
  *OutFirst = First;
  *OutLast = Last;
}

// Layer filler of one block row, taken at the column's centre: pockets and ore
// veins are left out, they would only be noise at this scale.
static BlockID FillerAt(LodGenScratch *S, int CentreX, int Y, int CentreZ,
                        uint64_t Seed) {
  StratumGenerator Stratum = ResolveStratum(Y);
  if (Stratum == STRATUM_GEN_VOID) {
    return BLOCK_ID_AIR;
  }
  if (Stratum != STRATUM_GEN_OVERWORLD) {
    return TerrainStoneBlockId();
  }
  int ChunkY = LodFloorShift(Y, SECTION_SHIFT);
  if (!S->FillerValid || S->FillerChunkY != ChunkY) {
    FillerCubeInit(&S->Filler, LodFloorShift(CentreX, SECTION_SHIFT), ChunkY,
                   LodFloorShift(CentreZ, SECTION_SHIFT), Seed);
    S->FillerChunkY = ChunkY;
    S->FillerValid = true;
  }
  BlockID Row = S->Filler.RowBlock[Y - (ChunkY * LOD_SECTION_SIZE)];
  return (Row != 0) ? Row : FillerBlockAt(&S->Filler, CentreX, Y, CentreZ);
}

static LodVoxel FluidVoxel(const ColumnInfo *Info, bool SeaSurface) {
  BlockID Id = (Info->Frozen && SeaSurface)
                   ? TerrainIceBlockId()
                   : TerrainFluidBlock(CAVE_FLUID_WATER);
  return LodCanonicalVoxel(Id, (unsigned)Info->SeaBiome);
}

static LodVoxel SolidVoxel(const LodGenScratch *S, const ColumnInfo *Info,
                           const ColumnTop *Column, int Row, int Voxel,
                           int Step) {
  const BiomeType *Biome = Info->Surface;
  BlockID Id = S->FillerRow[Row];
  // The top voxel shows its top block; deeper voxels the block at their own
  // top row, the member an exposed side face would show.
  int Y = (Column->TopVoxel == Voxel) ? Column->Top : (Voxel * Step) + Step - 1;
  int Depth = Column->Top - Y;
  if (Depth < Biome->SubsurfaceDepth) {
    Id = TerrainPaletteBlock(Biome, Depth, Y, Info->Steep);
  }
  return LodCanonicalVoxel(Id, (unsigned)Info->SurfaceBiome);
}

// Fills one section; returns whether any voxel is not air and the present
// cells through OutPresent.
static bool FillSection(LodGenScratch *S, int Level, int SectionY,
                        int CentreX, int CentreZ, uint64_t Seed,
                        uint64_t *OutPresent) {
  int Step = 1 << Level;
  int SeaVoxel = SeaTopVoxel(Level);
  int BaseVoxel = SectionY * LOD_SECTION_SIZE;
  // Rows that can hold rock under the surface layers resolve their filler
  // once, not once per column.
  int HighestTop = INT_MIN;
  for (int I = 0; I < LOD_SECTION_SIZE; I++) {
    for (int K = 0; K < LOD_SECTION_SIZE; K++) {
      HighestTop = MaxInt(HighestTop, S->Columns[I + 1][K + 1].TopVoxel);
    }
  }
  for (int Row = 0; Row < LOD_SECTION_SIZE; Row++) {
    int Voxel = BaseVoxel + Row;
    S->FillerRow[Row] = (Voxel <= HighestTop)
                            ? FillerAt(S, CentreX, (Voxel * Step) + Step - 1,
                                       CentreZ, Seed)
                            : BLOCK_ID_AIR;
  }

  uint64_t Present = 0;
  bool Any = false;
  for (int I = 0; I < LOD_SECTION_SIZE; I++) {
    for (int K = 0; K < LOD_SECTION_SIZE; K++) {
      const ColumnTop *Column = &S->Columns[I + 1][K + 1];
      const ColumnInfo *Info = &S->Info[I][K];
      if (Column->TopVoxel < BaseVoxel && SeaVoxel < BaseVoxel) {
        for (int Row = 0; Row < LOD_SECTION_SIZE; Row++) {
          S->Voxels[LodVoxelIndex(I, Row, K)] = LOD_VOXEL_AIR;
        }
        continue;
      }
      for (int Row = 0; Row < LOD_SECTION_SIZE; Row++) {
        int Voxel = BaseVoxel + Row;
        LodVoxel Value = LOD_VOXEL_AIR;
        bool Shown = false;
        if (Voxel > Column->TopVoxel) {
          if (Voxel <= SeaVoxel) {
            Value = FluidVoxel(Info, Voxel == SeaVoxel);
            Shown = true;
          }
        } else {
          Value = SolidVoxel(S, Info, Column, Row, Voxel, Step);
          Shown = Voxel == Column->TopVoxel || Voxel > Info->SideExposedAbove;
        }
        S->Voxels[LodVoxelIndex(I, Row, K)] = Value;
        if (Value != LOD_VOXEL_AIR) {
          Any = true;
          if (Shown) {
            Present |= LodCellBit(I / LOD_CELL_SIZE, Row / LOD_CELL_SIZE,
                                  K / LOD_CELL_SIZE);
          }
        }
      }
    }
  }
  *OutPresent = Present;
  return Any;
}

void LodGenerateColumn(LodGenScratch *Scratch, int Level, int X, int Z,
                       uint64_t Seed, LodGenEmit Emit, void *User,
                       LodGenResult *Out) {
  *Out = (LodGenResult){.FirstY = 1, .LastY = 0};
  int Width = LOD_SECTION_SIZE << Level;
  int OriginX = X * Width;
  int OriginZ = Z * Width;
  int CentreX = OriginX + (Width / 2);
  int CentreZ = OriginZ + (Width / 2);
  Scratch->FillerValid = false;

  SampleColumnTops(Scratch, Level, OriginX, OriginZ, Seed, Out);
  ResolveColumns(Scratch, Level, OriginX, OriginZ, Seed, Out);
  ColumnRange(Scratch, Level, &Out->FirstY, &Out->LastY);
  for (int SectionY = Out->FirstY; SectionY <= Out->LastY; SectionY++) {
    uint64_t Present = 0;
    if (!FillSection(Scratch, Level, SectionY, CentreX, CentreZ, Seed, &Present)) {
      Out->AirSkipped++;
      continue;
    }
    Emit(User, SectionY, Scratch->Voxels, Present);
    Out->Emitted++;
  }
}
