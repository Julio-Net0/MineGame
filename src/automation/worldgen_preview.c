#include "automation/worldgen_preview.h"
#include "core/fileio.h"
#include "core/hash.h"
#include "persistence/world_save.h"
#include "platform/platform.h"
#include "third_party/cJSON.h"
#include "third_party/png_write.h"
#include "world/biome.h"
#include "world/cave_fields.h"
#include "world/block_system.h"
#include "world/chunk.h"
#include "world/feature.h"
#include "world/prefab.h"
#include "world/shape_modifiers.h"
#include "world/stone_filler.h"
#include "world/surface.h"
#include "world/terrain_shape.h"
#include "world/worldgen_column_cache.h"
#include "world/worldgen_strata.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  RGB_CHANNELS = 3,
  DEFAULT_BEDROCK_FLOOR = -516,
  LAND_SHADE_MIN = 40,
  LAND_SHADE_RANGE = 215,
  HASH_COLOR_FLOOR = 64,
  HASH_COLOR_RANGE = 192,
  BITS_PER_BYTE = 8
};

typedef struct {
  unsigned char R;
  unsigned char G;
  unsigned char B;
} PreviewColor;

// Fixed colors for the blocks a slice is mostly made of, so the layout reads at
// a glance; every other block gets a stable color hashed from its id.
typedef struct {
  int StoneId;
  int WaterId;
  int BedrockId;
  int GrassId;
  int DirtId;
  int SandId;
  int LavaId;
  int DeepslateId;
} PreviewPalette;

enum {
  CAVE_BAND_HEIGHT = 64,
  CAVE_BAND_COUNT = 9
};

typedef struct {
  int AirCubes;
  int SolidCubes;
  int MixedCubes;
  int GeneratedCubes;
  double GenerationSeconds;
  double AirSeconds;
  double SolidSeconds;
  double MixedSeconds;
  // Biome map plus terrain, without the structure and flora passes.
  double TerrainSeconds;
  double MixedTerrainSeconds;
  bool BedrockFloorComplete;
  int SolidAboveCeiling;
  // Per 64-block band from the overworld floor up to sea level: positions
  // deeper than the cave shell below the preliminary surface, and how many of
  // them are open (air, water, or lava).
  int CaveBandTotal[CAVE_BAND_COUNT];
  int CaveBandOpen[CAVE_BAND_COUNT];
  int CaveBandMinY;
  int WaterBlocks;
  // Water below the ocean band: flooded caves.
  int CaveWaterBlocks;
  int LavaBlocks;
  int DeepslateBlocks;
  int PocketBlocks;
  // Blocks the structure and flora passes changed in the slice cubes.
  int StructureBlocks;
  int FloraBlocks;
  int OreBlocks[FILLER_MAX_ORE_KINDS];
} SliceStats;

static int FloorDiv(int Numerator, int Denominator) {
  int Quotient = Numerator / Denominator;
  if ((Numerator % Denominator != 0) && ((Numerator < 0) != (Denominator < 0))) {
    Quotient--;
  }
  return Quotient;
}

static void SetError(char *Error, int ErrorSize, const char *Reason,
                     const char *Detail) {
  (void)snprintf(Error, (size_t)ErrorSize, "%s%s", Reason, Detail);
}

static PreviewColor HashColor(uint64_t Value) {
  uint64_t Hash = SplitMix64(Value);
  PreviewColor Color = {
      (unsigned char)(HASH_COLOR_FLOOR + (int)(Hash % HASH_COLOR_RANGE)),
      (unsigned char)(HASH_COLOR_FLOOR +
                      (int)((Hash >> BITS_PER_BYTE) % HASH_COLOR_RANGE)),
      (unsigned char)(HASH_COLOR_FLOOR +
                      (int)((Hash >> (2 * BITS_PER_BYTE)) % HASH_COLOR_RANGE))};
  return Color;
}

static PreviewColor BlockColor(const PreviewPalette *Palette, int BlockId) {
  const PreviewColor SKY = {150, 200, 255};
  const PreviewColor STONE = {125, 125, 125};
  const PreviewColor WATER = {40, 80, 200};
  const PreviewColor BEDROCK = {30, 30, 30};
  const PreviewColor GRASS = {90, 160, 60};
  const PreviewColor DIRT = {130, 90, 50};
  const PreviewColor SAND = {220, 210, 150};
  const PreviewColor LAVA = {230, 90, 20};
  const PreviewColor DEEPSLATE = {70, 70, 82};

  if (BlockId == 0) {
    return SKY;
  }
  if (BlockId == Palette->StoneId) {
    return STONE;
  }
  if (BlockId == Palette->WaterId) {
    return WATER;
  }
  if (BlockId == Palette->BedrockId) {
    return BEDROCK;
  }
  if (BlockId == Palette->GrassId) {
    return GRASS;
  }
  if (BlockId == Palette->DirtId) {
    return DIRT;
  }
  if (BlockId == Palette->LavaId) {
    return LAVA;
  }
  if (BlockId == Palette->DeepslateId) {
    return DEEPSLATE;
  }
  if (BlockId == Palette->SandId) {
    return SAND;
  }
  return HashColor((uint64_t)(uint32_t)BlockId);
}

static void PutPixel(unsigned char *Pixels, int Width, int X, int Y,
                     PreviewColor Color) {
  size_t Offset = (((size_t)Y * (size_t)Width) + (size_t)X) * RGB_CHANNELS;
  Pixels[Offset] = Color.R;
  Pixels[Offset + 1] = Color.G;
  Pixels[Offset + 2] = Color.B;
}

static bool WritePng(const char *OutDir, const char *Name, const char *Suffix,
                     int Width, int Height, const unsigned char *Pixels) {
  char Path[FILEIO_MAX_PATH];
  int Written =
      snprintf(Path, sizeof(Path), "%s/%s_%s.png", OutDir, Name, Suffix);
  if (Written < 0 || (size_t)Written >= sizeof(Path)) {
    return false;
  }
  return StbWritePng(Path, Width, Height, RGB_CHANNELS, Pixels,
                     Width * RGB_CHANNELS);
}

// The biome's declared preview color, or a stable hash color when it has none.
static PreviewColor BiomePreviewColor(int BiomeId) {
  Color8 Declared = GetBiomeDef(BiomeId)->PreviewColor;
  if (Declared.A == 0) {
    return HashColor((uint64_t)BiomeId);
  }
  return (PreviewColor){Declared.R, Declared.G, Declared.B};
}

// Height and biome maps come from the pure samplers: they are 2D by definition
// and need no chunk.
static bool WriteSurfaceMaps(const char *OutDir, const char *Name, int OriginX,
                             int OriginZ, int Size, int *SurfaceMin,
                             int *SurfaceMax) {
  uint64_t Seed = GetWorldSeed();
  size_t Count = (size_t)Size * (size_t)Size;
  int *Surfaces = (int *)malloc(Count * sizeof(int));
  unsigned char *Height = (unsigned char *)malloc(Count * RGB_CHANNELS);
  unsigned char *Biomes = (unsigned char *)malloc(Count * RGB_CHANNELS);
  bool Ok = (Surfaces != NULL) && (Height != NULL) && (Biomes != NULL);

  if (Ok) {
    int LandMin = 0;
    int LandMax = 0;
    bool AnyLand = false;
    for (int Pz = 0; Pz < Size; Pz++) {
      for (int Px = 0; Px < Size; Px++) {
        int Surface = GetPreliminarySurface(OriginX + Px, OriginZ + Pz, Seed);
        Surfaces[((size_t)Pz * (size_t)Size) + (size_t)Px] = Surface;
        if (Px == 0 && Pz == 0) {
          *SurfaceMin = Surface;
          *SurfaceMax = Surface;
        }
        *SurfaceMin = (Surface < *SurfaceMin) ? Surface : *SurfaceMin;
        *SurfaceMax = (Surface > *SurfaceMax) ? Surface : *SurfaceMax;
        if (Surface > SEA_LEVEL) {
          LandMin = (!AnyLand || Surface < LandMin) ? Surface : LandMin;
          LandMax = (!AnyLand || Surface > LandMax) ? Surface : LandMax;
          AnyLand = true;
        }
      }
    }

    int LandSpan = (LandMax > LandMin) ? (LandMax - LandMin) : 1;
    for (int Pz = 0; Pz < Size; Pz++) {
      for (int Px = 0; Px < Size; Px++) {
        int Surface = Surfaces[((size_t)Pz * (size_t)Size) + (size_t)Px];
        PreviewColor Shade;
        if (Surface > SEA_LEVEL) {
          int Value =
              LAND_SHADE_MIN + ((Surface - LandMin) * LAND_SHADE_RANGE / LandSpan);
          Shade = (PreviewColor){(unsigned char)Value, (unsigned char)Value,
                                 (unsigned char)Value};
        } else {
          const PreviewColor SEA = {20, 60, 160};
          Shade = SEA;
        }
        PutPixel(Height, Size, Px, Pz, Shade);

        int BiomeId =
            SampleBiomeAt(OriginX + Px, Surface, OriginZ + Pz, Seed, NULL);
        PutPixel(Biomes, Size, Px, Pz, BiomePreviewColor(BiomeId));
      }
    }

    Ok = WritePng(OutDir, Name, "height", Size, Size, Height) &&
         WritePng(OutDir, Name, "biome", Size, Size, Biomes);
  }

  free(Surfaces);
  free(Height);
  free(Biomes);
  return Ok;
}

// Classified by content, not by the path generation took, so these counts stay
// comparable whether or not the generator short-circuits a cube.
static void CountCubeClass(const Chunk *Scratch, double Seconds,
                           SliceStats *Stats) {
  if (Scratch->SolidBlockCount == 0) {
    Stats->AirCubes++;
    Stats->AirSeconds += Seconds;
  } else if (Scratch->SolidBlockCount == (int)CHUNK_VOLUME) {
    Stats->SolidCubes++;
    Stats->SolidSeconds += Seconds;
  } else {
    Stats->MixedCubes++;
    Stats->MixedSeconds += Seconds;
  }
}

static int CountChanged(const Chunk *Before, const Chunk *After) {
  int Changed = 0;
  const BlockID *Old = &Before->Data[0][0][0];
  const BlockID *New = &After->Data[0][0][0];
  for (size_t Idx = 0; Idx < CHUNK_VOLUME; Idx++) {
    Changed += (Old[Idx] != New[Idx]) ? 1 : 0;
  }
  return Changed;
}

// Generates one scratch cube exactly as a chunk worker would on a fresh
// (not disk-loaded) chunk, and times it.
static void GenerateScratch(Chunk *Scratch, int ChunkX, int ChunkY, int ChunkZ,
                            SliceStats *Stats) {
  Scratch->ChunkX = ChunkX;
  Scratch->ChunkY = ChunkY;
  Scratch->ChunkZ = ChunkZ;

  double Start = PlatformGetTime();
  const ColumnEntry *Column = ColumnCacheLookup(
      GetColumnCache(COLUMN_CACHE_MAIN_SLOT), ChunkX, ChunkZ, GetWorldSeed());
  FillChunkBiomeMap(Scratch, Column);
  GenerateChunkTerrain(Scratch, Column);
  double TerrainEnd = PlatformGetTime();
  // Snapshots between passes are taken outside the timed spans.
  static Chunk SBefore;
  SBefore = *Scratch;
  double FeatureStart = PlatformGetTime();
  ColumnCache *Cache = GetColumnCache(COLUMN_CACHE_MAIN_SLOT);
  PlaceChunkFeatures(Scratch, Cache, true);
  double FeatureEnd = PlatformGetTime();
  Stats->StructureBlocks += CountChanged(&SBefore, Scratch);
  SBefore = *Scratch;
  double FloraStart = PlatformGetTime();
  // The structure pass may have reused this column's cache slot.
  Column = ColumnCacheLookup(Cache, ChunkX, ChunkZ, GetWorldSeed());
  PlaceChunkFlora(Scratch, Column);
  double FloraEnd = PlatformGetTime();
  Stats->FloraBlocks += CountChanged(&SBefore, Scratch);
  double Seconds = (TerrainEnd - Start) + (FeatureEnd - FeatureStart) +
                   (FloraEnd - FloraStart);
  Stats->TerrainSeconds += TerrainEnd - Start;
  if (Scratch->SolidBlockCount != 0 &&
      Scratch->SolidBlockCount != (int)CHUNK_VOLUME) {
    Stats->MixedTerrainSeconds += TerrainEnd - Start;
  }
  Stats->GenerationSeconds += Seconds;
  Stats->GeneratedCubes++;
  CountCubeClass(Scratch, Seconds, Stats);
}

static void ResolvePalette(PreviewPalette *Palette) {
  Palette->StoneId = GetBlockIdByName("Stone");
  Palette->WaterId = GetBlockIdByName("Water");
  Palette->BedrockId = GetBlockIdByName("bedrock");
  Palette->GrassId = GetBlockIdByName("Grass");
  Palette->DirtId = GetBlockIdByName("Dirt");
  Palette->SandId = GetBlockIdByName("Sand");
  Palette->LavaId = GetBlockIdByName("Lava");
  Palette->DeepslateId = GetBlockIdByName("deepslate");
}

static bool WriteSlice(const char *OutDir, const char *Name, int OriginX,
                       int CenterZ, int Size, int FloorY, int CeilingY,
                       SliceStats *Stats) {
  PreviewPalette Palette;
  ResolvePalette(&Palette);

  int Height = CeilingY - FloorY + 1;
  unsigned char *Pixels =
      (unsigned char *)malloc((size_t)Size * (size_t)Height * RGB_CHANNELS);
  Chunk *Scratch = (Chunk *)calloc(1, sizeof(Chunk));
  if (Pixels == NULL || Scratch == NULL) {
    free(Pixels);
    free(Scratch);
    return false;
  }

  int ChunkZ = FloorDiv(CenterZ, CHUNK_SIZE);
  int LocalZ = CenterZ - (ChunkZ * CHUNK_SIZE);
  uint64_t Seed = GetWorldSeed();
  int ShellBase = (int)GetCaveParams()->ShellBase;
  int OverworldFloor = FloorY;
  int OverworldTop = 0;
  (void)GetStratumRange(STRATUM_GEN_OVERWORLD, &OverworldFloor, &OverworldTop);
  Stats->CaveBandMinY = OverworldFloor;
  int FirstChunkX = FloorDiv(OriginX, CHUNK_SIZE);
  int LastChunkX = FloorDiv(OriginX + Size - 1, CHUNK_SIZE);
  int FirstChunkY = FloorDiv(FloorY, CHUNK_SIZE);
  int LastChunkY = FloorDiv(CeilingY, CHUNK_SIZE);

  Stats->BedrockFloorComplete = true;
  for (int ChunkX = FirstChunkX; ChunkX <= LastChunkX; ChunkX++) {
    for (int ChunkY = FirstChunkY; ChunkY <= LastChunkY; ChunkY++) {
      GenerateScratch(Scratch, ChunkX, ChunkY, ChunkZ, Stats);

      for (int LocalX = 0; LocalX < CHUNK_SIZE; LocalX++) {
        int Px = (ChunkX * CHUNK_SIZE) + LocalX - OriginX;
        if (Px < 0 || Px >= Size) {
          continue;
        }
        int CaveTop = GetPreliminarySurface((ChunkX * CHUNK_SIZE) + LocalX,
                                            CenterZ, Seed) -
                      ShellBase;
        for (int LocalY = 0; LocalY < CHUNK_SIZE; LocalY++) {
          int GlobalY = (ChunkY * CHUNK_SIZE) + LocalY;
          if (GlobalY < FloorY || GlobalY > CeilingY) {
            continue;
          }
          int BlockId = (int)Scratch->Data[LocalX][LocalY][LocalZ];
          if (GlobalY == FloorY && BlockId != Palette.BedrockId) {
            Stats->BedrockFloorComplete = false;
          }
          if (GlobalY >= CeilingY && BlockId != 0) {
            Stats->SolidAboveCeiling++;
          }
          Stats->WaterBlocks += (BlockId == Palette.WaterId) ? 1 : 0;
          Stats->LavaBlocks += (BlockId == Palette.LavaId) ? 1 : 0;
          Stats->DeepslateBlocks += (BlockId == Palette.DeepslateId) ? 1 : 0;
          Stats->PocketBlocks +=
              (BlockId != 0 && FillerIsPocketBlock((BlockID)BlockId)) ? 1 : 0;
          int OreKind = FillerOreKindOf((BlockID)BlockId);
          if (OreKind >= 0) {
            Stats->OreBlocks[OreKind]++;
          }
          Stats->CaveWaterBlocks +=
              (BlockId == Palette.WaterId && GlobalY < CaveTop) ? 1 : 0;
          int Band = (GlobalY - OverworldFloor) / CAVE_BAND_HEIGHT;
          if (GlobalY >= OverworldFloor && GlobalY < CaveTop &&
              GlobalY <= SEA_LEVEL && Band < CAVE_BAND_COUNT) {
            bool Open = BlockId == 0 || BlockId == Palette.WaterId ||
                        BlockId == Palette.LavaId;
            Stats->CaveBandTotal[Band]++;
            Stats->CaveBandOpen[Band] += Open ? 1 : 0;
          }
          PutPixel(Pixels, Size, Px, CeilingY - GlobalY,
                   BlockColor(&Palette, BlockId));
        }
      }
    }
  }

  bool Ok = WritePng(OutDir, Name, "slice", Size, Height, Pixels);
  free(Pixels);
  free(Scratch);
  return Ok;
}

static void AddCaveSummary(cJSON *Root, const SliceStats *Stats) {
  cJSON *Bands = cJSON_CreateArray();
  for (int Band = 0; Bands != NULL && Band < CAVE_BAND_COUNT; Band++) {
    if (Stats->CaveBandTotal[Band] == 0) {
      continue;
    }
    cJSON *Entry = cJSON_CreateObject();
    if (Entry == NULL) {
      continue;
    }
    cJSON_AddNumberToObject(Entry, "minY",
                            Stats->CaveBandMinY + (Band * CAVE_BAND_HEIGHT));
    cJSON_AddNumberToObject(Entry, "fraction",
                            (double)Stats->CaveBandOpen[Band] /
                                (double)Stats->CaveBandTotal[Band]);
    cJSON_AddItemToArray(Bands, Entry);
  }
  cJSON_AddItemToObject(Root, "caveOpenByBand", Bands);
  cJSON_AddNumberToObject(Root, "waterBlocks", Stats->WaterBlocks);
  cJSON_AddNumberToObject(Root, "caveWaterBlocks", Stats->CaveWaterBlocks);
  cJSON_AddNumberToObject(Root, "lavaBlocks", Stats->LavaBlocks);
  cJSON_AddNumberToObject(Root, "deepslateBlocks", Stats->DeepslateBlocks);
  cJSON_AddNumberToObject(Root, "pocketBlocks", Stats->PocketBlocks);
  cJSON *Ores = cJSON_CreateObject();
  for (int Kind = 0; Ores != NULL && Kind < FillerOreKindCount(); Kind++) {
    cJSON_AddNumberToObject(Ores, FillerOreKindName(Kind),
                            Stats->OreBlocks[Kind]);
  }
  cJSON_AddItemToObject(Root, "oreBlocks", Ores);
  cJSON_AddNumberToObject(Root, "structureBlocks", Stats->StructureBlocks);
  cJSON_AddNumberToObject(Root, "floraBlocks", Stats->FloraBlocks);
}

static double MeanMicros(double Seconds, int Count) {
  const double MICROS_PER_SECOND = 1000000.0;
  return (Count > 0) ? (Seconds * MICROS_PER_SECOND) / (double)Count : 0.0;
}

enum {
  SHAPE_QUANTILE_COUNT = 5
};

static const float SHAPE_QUANTILES[SHAPE_QUANTILE_COUNT] = {0.05F, 0.25F, 0.5F,
                                                            0.75F, 0.95F};
static const char *const SHAPE_COORD_KEYS[SHAPE_COORD_COUNT] = {
    "continentalness", "erosion", "weirdness", "peaksValleys"};

typedef struct {
  float Quantiles[SHAPE_COORD_COUNT][SHAPE_QUANTILE_COUNT];
  float BaseQuantiles[SHAPE_QUANTILE_COUNT];
} ShapeSummary;

static int CompareFloats(const void *Left, const void *Right) {
  float A = *(const float *)Left;
  float B = *(const float *)Right;
  return (A > B) - (A < B);
}

static void FillQuantiles(float *Values, int Count, float *Out) {
  qsort(Values, (size_t)Count, sizeof(float), CompareFloats);
  for (int Idx = 0; Idx < SHAPE_QUANTILE_COUNT; Idx++) {
    int Rank = (int)(SHAPE_QUANTILES[Idx] * (float)(Count - 1));
    Out[Idx] = Values[Rank];
  }
}

// Maps [-1, 1] to [0, 255], clamping outliers.
static unsigned char UnitToByte(float Value) {
  const float BYTE_MAX_F = 255.0F;
  const float HALF = 0.5F;
  float Unit = fminf(fmaxf((Value + 1.0F) * HALF, 0.0F), 1.0F);
  return (unsigned char)(Unit * BYTE_MAX_F);
}

// Shape fields at lattice resolution: red continentalness, green erosion, blue
// peaks-and-valleys, each mapped from [-1, 1]. Their quantiles go into the
// statistics, which is what spline locations are tuned against.
static bool WriteShapeMap(const char *OutDir, const char *Name, int OriginX,
                          int OriginZ, int Size, ShapeSummary *Summary) {
  uint64_t Seed = GetWorldSeed();
  int Side = (Size + SHAPE_LATTICE_STEP - 1) / SHAPE_LATTICE_STEP;
  size_t Count = (size_t)Side * (size_t)Side;
  unsigned char *Pixels = (unsigned char *)malloc(Count * RGB_CHANNELS);
  float *Samples = (float *)malloc(Count * sizeof(float) * (SHAPE_COORD_COUNT + 1));
  if (Pixels == NULL || Samples == NULL) {
    free(Pixels);
    free(Samples);
    return false;
  }

  for (int Pz = 0; Pz < Side; Pz++) {
    for (int Px = 0; Px < Side; Px++) {
      int X = OriginX + (Px * SHAPE_LATTICE_STEP);
      int Z = OriginZ + (Pz * SHAPE_LATTICE_STEP);
      ShapeCoords Coords = SampleShapeCoords(X, Z, Seed);
      size_t Cell = ((size_t)Pz * (size_t)Side) + (size_t)Px;
      for (int Coord = 0; Coord < SHAPE_COORD_COUNT; Coord++) {
        Samples[((size_t)Coord * Count) + Cell] = Coords.Values[Coord];
      }
      Samples[((size_t)SHAPE_COORD_COUNT * Count) + Cell] =
          EvaluateColumnShape(X, Z, Seed).Base;
      PreviewColor Color = {UnitToByte(Coords.Values[SHAPE_COORD_CONTINENTALNESS]),
                            UnitToByte(Coords.Values[SHAPE_COORD_EROSION]),
                            UnitToByte(Coords.Values[SHAPE_COORD_PEAKS_VALLEYS])};
      PutPixel(Pixels, Side, Px, Pz, Color);
    }
  }

  for (int Coord = 0; Coord < SHAPE_COORD_COUNT; Coord++) {
    FillQuantiles(&Samples[(size_t)Coord * Count], (int)Count,
                  Summary->Quantiles[Coord]);
  }
  FillQuantiles(&Samples[(size_t)SHAPE_COORD_COUNT * Count], (int)Count,
                Summary->BaseQuantiles);

  bool Ok = WritePng(OutDir, Name, "shape", Side, Side, Pixels);
  free(Pixels);
  free(Samples);
  return Ok;
}

static cJSON *QuantileArray(const float *Values) {
  cJSON *Array = cJSON_CreateArray();
  for (int Idx = 0; Array != NULL && Idx < SHAPE_QUANTILE_COUNT; Idx++) {
    cJSON_AddItemToArray(Array, cJSON_CreateNumber((double)Values[Idx]));
  }
  return Array;
}

static void AddShapeSummary(cJSON *Root, const ShapeSummary *Summary) {
  cJSON *Node = cJSON_CreateObject();
  if (Node == NULL) {
    return;
  }
  cJSON_AddItemToObject(Node, "quantiles", QuantileArray(SHAPE_QUANTILES));
  for (int Coord = 0; Coord < SHAPE_COORD_COUNT; Coord++) {
    cJSON_AddItemToObject(Node, SHAPE_COORD_KEYS[Coord],
                          QuantileArray(Summary->Quantiles[Coord]));
  }
  cJSON_AddItemToObject(Node, "base", QuantileArray(Summary->BaseQuantiles));
  cJSON_AddItemToObject(Root, "shape", Node);
}

static bool WriteStats(const char *OutDir, const char *Name, int SurfaceMin,
                       int SurfaceMax, const SliceStats *Stats,
                       const ShapeSummary *Summary) {
  cJSON *Root = cJSON_CreateObject();
  if (Root == NULL) {
    return false;
  }

  double MeanUs = MeanMicros(Stats->GenerationSeconds, Stats->GeneratedCubes);

  cJSON_AddNumberToObject(Root, "generatorVersion", WORLDGEN_VERSION);
  cJSON_AddNumberToObject(Root, "surfaceMin", SurfaceMin);
  cJSON_AddNumberToObject(Root, "surfaceMax", SurfaceMax);
  cJSON_AddNumberToObject(Root, "generationCeiling", GetGenerationCeiling());
  cJSON_AddNumberToObject(Root, "airCubes", Stats->AirCubes);
  cJSON_AddNumberToObject(Root, "solidCubes", Stats->SolidCubes);
  cJSON_AddNumberToObject(Root, "mixedCubes", Stats->MixedCubes);
  cJSON_AddNumberToObject(Root, "generatedCubes", Stats->GeneratedCubes);
  cJSON_AddNumberToObject(Root, "meanGenerationUs", MeanUs);
  cJSON_AddNumberToObject(Root, "meanAirUs",
                          MeanMicros(Stats->AirSeconds, Stats->AirCubes));
  cJSON_AddNumberToObject(Root, "meanSolidUs",
                          MeanMicros(Stats->SolidSeconds, Stats->SolidCubes));
  cJSON_AddNumberToObject(Root, "meanTerrainUs",
                          MeanMicros(Stats->TerrainSeconds, Stats->GeneratedCubes));
  cJSON_AddNumberToObject(
      Root, "meanMixedTerrainUs",
      MeanMicros(Stats->MixedTerrainSeconds, Stats->MixedCubes));
  cJSON_AddNumberToObject(Root, "meanMixedUs",
                          MeanMicros(Stats->MixedSeconds, Stats->MixedCubes));
  cJSON_AddBoolToObject(Root, "bedrockFloorComplete",
                        Stats->BedrockFloorComplete);
  cJSON_AddNumberToObject(Root, "solidAboveCeiling", Stats->SolidAboveCeiling);
  AddShapeSummary(Root, Summary);
  AddCaveSummary(Root, Stats);

  char *Text = cJSON_Print(Root);
  cJSON_Delete(Root);
  if (Text == NULL) {
    return false;
  }

  char Path[FILEIO_MAX_PATH];
  int Written =
      snprintf(Path, sizeof(Path), "%s/%s_worldgen.json", OutDir, Name);
  bool Ok = false;
  if (Written >= 0 && (size_t)Written < sizeof(Path)) {
    FILE *File = fopen(Path, "wb");
    if (File != NULL) {
      Ok = fputs(Text, File) >= 0;
      Ok = (fclose(File) == 0) && Ok;
    }
  }
  cJSON_free(Text);
  return Ok;
}

bool WorldgenPreviewWrite(const char *OutDir, const char *Name, int CenterX,
                          int CenterZ, int Size, char *Error, int ErrorSize) {
  if (Size < WORLDGEN_PREVIEW_MIN_SIZE || Size > WORLDGEN_PREVIEW_MAX_SIZE) {
    SetError(Error, ErrorSize, "size out of range", "");
    return false;
  }

  int OriginX = CenterX - (Size / 2);
  int OriginZ = CenterZ - (Size / 2);

  int SurfaceMin = 0;
  int SurfaceMax = 0;
  if (!WriteSurfaceMaps(OutDir, Name, OriginX, OriginZ, Size, &SurfaceMin,
                        &SurfaceMax)) {
    SetError(Error, ErrorSize, "could not write the height or biome map for ",
             Name);
    return false;
  }

  int FloorY = DEFAULT_BEDROCK_FLOOR;
  int BandMaxY = 0;
  (void)GetStratumRange(STRATUM_GEN_BEDROCK_BAND, &FloorY, &BandMaxY);

  SliceStats Stats = {0};
  if (!WriteSlice(OutDir, Name, OriginX, CenterZ, Size, FloorY,
                  GetGenerationCeiling(), &Stats)) {
    SetError(Error, ErrorSize, "could not write the slice for ", Name);
    return false;
  }

  ShapeSummary Summary = {0};
  if (!WriteShapeMap(OutDir, Name, OriginX, OriginZ, Size, &Summary)) {
    SetError(Error, ErrorSize, "could not write the shape map for ", Name);
    return false;
  }

  if (!WriteStats(OutDir, Name, SurfaceMin, SurfaceMax, &Stats, &Summary)) {
    SetError(Error, ErrorSize, "could not write the statistics for ", Name);
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Large biome map
// ---------------------------------------------------------------------------

enum {
  TREE_TALLY_PREFABS = 32
};

typedef struct {
  int Cells[BIOME_REGISTRY_SIZE];
  int LandCells[BIOME_REGISTRY_SIZE];
  int TotalCells;
  int TotalLand;
  int OceanCells;
  int OceanWet;
  int PeakCells;
  int PeakHigh;
  // Per landform modifier: samples with positive and with full weight, and
  // full-weight samples whose biome is the modifier's own.
  // Structures the pass would place at the sampled grid cells, per biome and
  // prefab index.
  int Trees[BIOME_REGISTRY_SIZE][TREE_TALLY_PREFABS];
  int ModifierPositive[SHAPE_MODIFIER_MAX];
  int ModifierFull[SHAPE_MODIFIER_MAX];
  int ModifierAgree[SHAPE_MODIFIER_MAX];
} BiomeCoverage;

// Weights at the lattice column under the sample, as EvaluateColumnShape
// computes them.
static void CountModifiers(BiomeCoverage *Coverage, int X, int Z, int BiomeId,
                           uint64_t Seed) {
  int Count = GetShapeModifierCount();
  if (Count == 0) {
    return;
  }
  int LatticeX = FloorDiv(X, SHAPE_LATTICE_STEP) * SHAPE_LATTICE_STEP;
  int LatticeZ = FloorDiv(Z, SHAPE_LATTICE_STEP) * SHAPE_LATTICE_STEP;
  const char *BiomeName = GetBiomeDef(BiomeId)->Name;
  for (int Idx = 0; Idx < Count; Idx++) {
    float Weight = ShapeModifierWeightAt(Idx, LatticeX, LatticeZ, Seed);
    Coverage->ModifierPositive[Idx] += (Weight > 0.0F) ? 1 : 0;
    if (Weight >= 1.0F) {
      Coverage->ModifierFull[Idx]++;
      Coverage->ModifierAgree[Idx] +=
          (strcmp(GetShapeModifier(Idx)->Biome, BiomeName) == 0) ? 1 : 0;
    }
  }
}

static bool FamilyIs(const BiomeType *Biome, const char *Family) {
  return strcmp(Biome->Family, Family) == 0;
}

static void CountCoverage(BiomeCoverage *Coverage, int BiomeId, int Surface) {
  const int PEAK_HEIGHT = 64;
  const BiomeType *Biome = GetBiomeDef(BiomeId);
  bool Land = Surface > SEA_LEVEL;
  Coverage->Cells[BiomeId]++;
  Coverage->LandCells[BiomeId] += Land ? 1 : 0;
  Coverage->TotalCells++;
  Coverage->TotalLand += Land ? 1 : 0;
  if (FamilyIs(Biome, "ocean")) {
    Coverage->OceanCells++;
    Coverage->OceanWet += (Surface <= SEA_LEVEL) ? 1 : 0;
  }
  if (FamilyIs(Biome, "peak")) {
    Coverage->PeakCells++;
    Coverage->PeakHigh += (Surface > SEA_LEVEL + PEAK_HEIGHT) ? 1 : 0;
  }
}

static void CountTree(BiomeCoverage *Coverage, int X, int Z, uint64_t Seed) {
  int TreeBiome = 0;
  const char *PrefabName = NULL;
  if (!ResolveStructureAt(X, Z, Seed, &TreeBiome, &PrefabName)) {
    return;
  }
  int PrefabIdx = GetPrefabIndexByName(PrefabName);
  if (PrefabIdx >= 0 && PrefabIdx < TREE_TALLY_PREFABS && TreeBiome >= 0 &&
      TreeBiome < BIOME_REGISTRY_SIZE) {
    Coverage->Trees[TreeBiome][PrefabIdx]++;
  }
}

static void AddTreeTally(cJSON *Root, const BiomeCoverage *Coverage) {
  cJSON *Trees = cJSON_CreateArray();
  for (int Biome = 0; Trees != NULL && Biome < BIOME_REGISTRY_SIZE; Biome++) {
    for (int PrefabIdx = 0; PrefabIdx < TREE_TALLY_PREFABS; PrefabIdx++) {
      int Count = Coverage->Trees[Biome][PrefabIdx];
      const Prefab *Shape = GetPrefabByIndex(PrefabIdx);
      if (Count == 0 || Shape == NULL) {
        continue;
      }
      cJSON *Entry = cJSON_CreateObject();
      if (Entry == NULL) {
        continue;
      }
      cJSON_AddStringToObject(Entry, "biome", GetBiomeDef(Biome)->Name);
      cJSON_AddStringToObject(Entry, "prefab", Shape->Name);
      cJSON_AddNumberToObject(Entry, "count", Count);
      cJSON_AddItemToArray(Trees, Entry);
    }
  }
  cJSON_AddItemToObject(Root, "trees", Trees);
}

static double Fraction(int Part, int Whole) {
  return (Whole > 0) ? (double)Part / (double)Whole : 0.0;
}

static bool WriteCoverage(const char *OutDir, const char *Name,
                          const BiomeCoverage *Coverage) {
  cJSON *Root = cJSON_CreateObject();
  cJSON *Biomes = cJSON_CreateArray();
  if (Root == NULL || Biomes == NULL) {
    cJSON_Delete(Root);
    cJSON_Delete(Biomes);
    return false;
  }
  int Distinct = 0;
  for (int Id = 0; Id < BIOME_REGISTRY_SIZE; Id++) {
    if (Coverage->Cells[Id] == 0) {
      continue;
    }
    Distinct++;
    const BiomeType *Biome = GetBiomeDef(Id);
    cJSON *Entry = cJSON_CreateObject();
    if (Entry == NULL) {
      continue;
    }
    cJSON_AddNumberToObject(Entry, "id", Id);
    cJSON_AddStringToObject(Entry, "name", Biome->Name);
    cJSON_AddStringToObject(Entry, "family", Biome->Family);
    cJSON_AddNumberToObject(Entry, "fraction",
                            Fraction(Coverage->Cells[Id], Coverage->TotalCells));
    cJSON_AddNumberToObject(
        Entry, "landFraction",
        Fraction(Coverage->LandCells[Id], Coverage->TotalLand));
    cJSON_AddItemToArray(Biomes, Entry);
  }
  cJSON_AddItemToObject(Root, "biomes", Biomes);
  cJSON_AddNumberToObject(Root, "distinctBiomes", Distinct);
  cJSON_AddNumberToObject(Root, "landFraction",
                          Fraction(Coverage->TotalLand, Coverage->TotalCells));
  cJSON_AddNumberToObject(Root, "oceanWetFraction",
                          Fraction(Coverage->OceanWet, Coverage->OceanCells));
  cJSON_AddNumberToObject(Root, "peakHighFraction",
                          Fraction(Coverage->PeakHigh, Coverage->PeakCells));

  cJSON *Modifiers = cJSON_CreateArray();
  for (int Idx = 0; Modifiers != NULL && Idx < GetShapeModifierCount(); Idx++) {
    cJSON *Entry = cJSON_CreateObject();
    if (Entry == NULL) {
      continue;
    }
    cJSON_AddStringToObject(Entry, "biome", GetShapeModifier(Idx)->Biome);
    cJSON_AddNumberToObject(Entry, "type", (double)GetShapeModifier(Idx)->Type);
    cJSON_AddNumberToObject(
        Entry, "positiveFraction",
        Fraction(Coverage->ModifierPositive[Idx], Coverage->TotalCells));
    cJSON_AddNumberToObject(
        Entry, "fullFraction",
        Fraction(Coverage->ModifierFull[Idx], Coverage->TotalCells));
    cJSON_AddNumberToObject(
        Entry, "biomeAgreement",
        Fraction(Coverage->ModifierAgree[Idx], Coverage->ModifierFull[Idx]));
    cJSON_AddItemToArray(Modifiers, Entry);
  }
  cJSON_AddItemToObject(Root, "modifiers", Modifiers);
  AddTreeTally(Root, Coverage);

  char *Text = cJSON_Print(Root);
  cJSON_Delete(Root);
  if (Text == NULL) {
    return false;
  }
  char Path[FILEIO_MAX_PATH];
  int Written = snprintf(Path, sizeof(Path), "%s/%s_biomes.json", OutDir, Name);
  bool Ok = false;
  if (Written >= 0 && (size_t)Written < sizeof(Path)) {
    FILE *File = fopen(Path, "wb");
    if (File != NULL) {
      Ok = fputs(Text, File) >= 0;
      Ok = (fclose(File) == 0) && Ok;
    }
  }
  cJSON_free(Text);
  return Ok;
}

bool WorldgenBiomeMapWrite(const char *OutDir, const char *Name, int CenterX,
                           int CenterZ, int Size, int Step, char *Error,
                           int ErrorSize) {
  if (Size <= 0 || Step <= 0 || Size % Step != 0 ||
      Size / Step > WORLDGEN_PREVIEW_MAX_SIZE) {
    SetError(Error, ErrorSize, "size must be a positive multiple of step with "
                               "at most 1024 samples per side", "");
    return false;
  }
  int Side = Size / Step;
  int OriginX = CenterX - (Size / 2);
  int OriginZ = CenterZ - (Size / 2);
  uint64_t Seed = GetWorldSeed();

  unsigned char *Pixels =
      (unsigned char *)malloc((size_t)Side * (size_t)Side * RGB_CHANNELS);
  BiomeCoverage *Coverage = (BiomeCoverage *)calloc(1, sizeof(BiomeCoverage));
  bool Ok = (Pixels != NULL) && (Coverage != NULL);
  for (int Pz = 0; Ok && Pz < Side; Pz++) {
    for (int Px = 0; Px < Side; Px++) {
      int X = OriginX + (Px * Step) + (Step / 2);
      int Z = OriginZ + (Pz * Step) + (Step / 2);
      int Surface =
          GetPreliminarySurface(BiomeCellCenter(X), BiomeCellCenter(Z), Seed);
      int BiomeId = SampleBiomeAt(X, Surface, Z, Seed, NULL);
      PutPixel(Pixels, Side, Px, Pz, BiomePreviewColor(BiomeId));
      CountCoverage(Coverage, BiomeId, Surface);
      CountModifiers(Coverage, X, Z, BiomeId, Seed);
      CountTree(Coverage, X, Z, Seed);
    }
  }
  Ok = Ok && WritePng(OutDir, Name, "biomes", Side, Side, Pixels) &&
       WriteCoverage(OutDir, Name, Coverage);
  free(Pixels);
  free(Coverage);
  if (!Ok) {
    SetError(Error, ErrorSize, "could not write the biome map for ", Name);
  }
  return Ok;
}
