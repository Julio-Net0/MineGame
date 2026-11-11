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
#include "world/surface.h"
#include "world/terrain_shape.h"
#include "world/worldgen_column_cache.h"
#include "world/worldgen_strata.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

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
            SampleBiomeAt(OriginX + Px, Surface, OriginZ + Pz, Surface, Seed);
        PutPixel(Biomes, Size, Px, Pz, HashColor((uint64_t)BiomeId));
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
  ColumnCache *Cache = GetColumnCache(COLUMN_CACHE_MAIN_SLOT);
  PlaceChunkFeatures(Scratch, Cache, true);
  // The structure pass may have reused this column's cache slot.
  Column = ColumnCacheLookup(Cache, ChunkX, ChunkZ, GetWorldSeed());
  PlaceChunkFlora(Scratch, Column);
  double Seconds = PlatformGetTime() - Start;
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
