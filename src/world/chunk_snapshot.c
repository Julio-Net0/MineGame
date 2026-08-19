#include "world/chunk_snapshot.h"

#include "world/world.h"

#ifdef MINEGAME_SNAPSHOT_VERIFY
#include "core/log.h"
#endif

#include <stddef.h>

// CHUNK_SIZE is a power of two, so masking gives the chunk-local coordinate for
// the margin values too: -1 masks to CHUNK_SIZE-1 (the neighbour's last row) and
// CHUNK_SIZE masks to 0 (its first).
#define CHUNK_INDEX_MASK (CHUNK_SIZE - 1)
#define BIOME_INDEX_MASK (BIOME_CELLS_PER_CHUNK - 1)

// -1 selects the low neighbour, 0..CHUNK_SIZE-1 the chunk itself, CHUNK_SIZE the
// high one.
static int NeighbourSlot(int Local) {
  if (Local < 0) {
    return 0;
  }
  if (Local >= CHUNK_SIZE) {
    return 2;
  }
  return 1;
}

static int BiomeNeighbourSlot(int Local) {
  if (Local < 0) {
    return 0;
  }
  if (Local >= BIOME_CELLS_PER_CHUNK) {
    return 2;
  }
  return 1;
}

void CaptureChunkSnapshot(World *WorldVal, const Chunk *ChunkVal,
                          ChunkSnapshot *Out) {
  if (WorldVal == NULL || ChunkVal == NULL || Out == NULL) {
    return;
  }

  Out->ChunkX = ChunkVal->ChunkX;
  Out->ChunkY = ChunkVal->ChunkY;
  Out->ChunkZ = ChunkVal->ChunkZ;
  Out->SolidBlockCount = ChunkVal->SolidBlockCount;

  // Resolve the 3x3x3 chunk neighbourhood once. The ambient-occlusion offsets
  // reach diagonally past edges and corners, so the diagonal chunks are needed
  // and not just the six sharing a face. Twenty-seven map lookups here replace
  // one per sampled cell.
  const Chunk *Neighbourhood[3][3][3];
  for (int Dx = -1; Dx <= 1; Dx++) {
    for (int Dy = -1; Dy <= 1; Dy++) {
      for (int Dz = -1; Dz <= 1; Dz++) {
        Neighbourhood[Dx + 1][Dy + 1][Dz + 1] =
            GetChunkFromWorld(WorldVal, ChunkVal->ChunkX + Dx,
                              ChunkVal->ChunkY + Dy, ChunkVal->ChunkZ + Dz);
      }
    }
  }

  for (int LocalX = -1; LocalX <= CHUNK_SIZE; LocalX++) {
    int Sx = NeighbourSlot(LocalX);
    int Ix = LocalX & CHUNK_INDEX_MASK;
    for (int LocalY = -1; LocalY <= CHUNK_SIZE; LocalY++) {
      int Sy = NeighbourSlot(LocalY);
      int Iy = LocalY & CHUNK_INDEX_MASK;
      for (int LocalZ = -1; LocalZ <= CHUNK_SIZE; LocalZ++) {
        int Sz = NeighbourSlot(LocalZ);
        const Chunk *Source = Neighbourhood[Sx][Sy][Sz];
        // Absent neighbour stores air, which is what GetBlockIDFromWorld
        // returns for an unloaded chunk.
        Out->Blocks[LocalX + 1][LocalY + 1][LocalZ + 1] =
            (Source != NULL) ? Source->Data[Ix][Iy][LocalZ & CHUNK_INDEX_MASK]
                             : 0;
      }
    }
  }

  int BaseCellX = (ChunkVal->ChunkX * BIOME_CELLS_PER_CHUNK) - 1;
  int BaseCellY = ChunkVal->ChunkY * BIOME_CELLS_PER_CHUNK;
  int BaseCellZ = (ChunkVal->ChunkZ * BIOME_CELLS_PER_CHUNK) - 1;

  for (int Ox = 0; Ox < SNAPSHOT_BIOME_SPAN_XZ; Ox++) {
    int LocalCellX = (BaseCellX + Ox) - (ChunkVal->ChunkX * BIOME_CELLS_PER_CHUNK);
    int Sx = BiomeNeighbourSlot(LocalCellX);
    int Ix = LocalCellX & BIOME_INDEX_MASK;
    for (int Oy = 0; Oy < SNAPSHOT_BIOME_SPAN_Y; Oy++) {
      int LocalCellY = (BaseCellY + Oy) - (ChunkVal->ChunkY * BIOME_CELLS_PER_CHUNK);
      int Sy = BiomeNeighbourSlot(LocalCellY);
      int Iy = LocalCellY & BIOME_INDEX_MASK;
      for (int Oz = 0; Oz < SNAPSHOT_BIOME_SPAN_XZ; Oz++) {
        int LocalCellZ =
            (BaseCellZ + Oz) - (ChunkVal->ChunkZ * BIOME_CELLS_PER_CHUNK);
        int Sz = BiomeNeighbourSlot(LocalCellZ);
        const Chunk *Source = Neighbourhood[Sx][Sy][Sz];

        if (Source != NULL) {
          Out->BiomeCells[Ox][Oy][Oz] =
              Source->BiomeMap[Ix][Iy][LocalCellZ & BIOME_INDEX_MASK];
          Out->BiomeCellPresent[Ox][Oy][Oz] = 1;
        } else {
          Out->BiomeCells[Ox][Oy][Oz] = 0;
          Out->BiomeCellPresent[Ox][Oy][Oz] = 0;
        }
      }
    }
  }
}

unsigned char SnapshotBlockAt(const ChunkSnapshot *Snap, int LocalX, int LocalY,
                              int LocalZ) {
  if (LocalX < -1 || LocalX > CHUNK_SIZE || LocalY < -1 || LocalY > CHUNK_SIZE ||
      LocalZ < -1 || LocalZ > CHUNK_SIZE) {
    return 0;
  }
  return Snap->Blocks[LocalX + 1][LocalY + 1][LocalZ + 1];
}

// Mirrors GetChunkBiomeAtLocal, including its clamp: the caller passes block
// coordinates that the AO and tint paths may push one past the chunk.
unsigned char SnapshotLocalBiome(const ChunkSnapshot *Snap, int LocalX,
                                 int LocalY, int LocalZ) {
  int CellX = LocalX / BIOME_CELL_SIZE;
  int CellY = LocalY / BIOME_CELL_SIZE;
  int CellZ = LocalZ / BIOME_CELL_SIZE;

  if (CellX < 0) { CellX = 0; }
  if (CellX >= BIOME_CELLS_PER_CHUNK) { CellX = BIOME_CELLS_PER_CHUNK - 1; }
  if (CellY < 0) { CellY = 0; }
  if (CellY >= BIOME_CELLS_PER_CHUNK) { CellY = BIOME_CELLS_PER_CHUNK - 1; }
  if (CellZ < 0) { CellZ = 0; }
  if (CellZ >= BIOME_CELLS_PER_CHUNK) { CellZ = BIOME_CELLS_PER_CHUNK - 1; }

  // The chunk's own cells sit one in from the snapshot origin on X and Z.
  return Snap->BiomeCells[CellX + 1][CellY][CellZ + 1];
}

#ifdef MINEGAME_SNAPSHOT_VERIFY

enum {
  VERIFY_MAX_REPORTED = 8
};

int VerifyChunkSnapshot(World *WorldVal, const Chunk *ChunkVal,
                        const ChunkSnapshot *Snap) {
  int Mismatches = 0;

  // Every block the snapshot holds, against what the mesher's old reader would
  // have resolved for the same chunk-local coordinate.
  for (int LocalX = -1; LocalX <= CHUNK_SIZE; LocalX++) {
    for (int LocalY = -1; LocalY <= CHUNK_SIZE; LocalY++) {
      for (int LocalZ = -1; LocalZ <= CHUNK_SIZE; LocalZ++) {
        unsigned char FromSnapshot = SnapshotBlockAt(Snap, LocalX, LocalY, LocalZ);

        unsigned char FromWorld;
        if (LocalX >= 0 && LocalX < CHUNK_SIZE && LocalY >= 0 &&
            LocalY < CHUNK_SIZE && LocalZ >= 0 && LocalZ < CHUNK_SIZE) {
          FromWorld = ChunkVal->Data[LocalX][LocalY][LocalZ];
        } else {
          int Gx = (ChunkVal->ChunkX * CHUNK_SIZE) + LocalX;
          int Gy = (ChunkVal->ChunkY * CHUNK_SIZE) + LocalY;
          int Gz = (ChunkVal->ChunkZ * CHUNK_SIZE) + LocalZ;
          FromWorld = (unsigned char)GetBlockIDFromWorld(
              WorldVal, (Vec3){(float)Gx, (float)Gy, (float)Gz});
        }

        if (FromSnapshot != FromWorld) {
          if (Mismatches < VERIFY_MAX_REPORTED) {
            LogError("SNAPSHOT: block (%d,%d,%d) local (%d,%d,%d): snapshot %u, world %u",
                     ChunkVal->ChunkX, ChunkVal->ChunkY, ChunkVal->ChunkZ,
                     LocalX, LocalY, LocalZ, (unsigned)FromSnapshot,
                     (unsigned)FromWorld);
          }
          Mismatches++;
        }
      }
    }
  }

  // Biome cells over the span tint interpolation samples. The fallback is
  // exercised with a value no palette uses, so a cell that should have fallen
  // back but did not is caught rather than coincidentally matching.
  const unsigned char PROBE_FALLBACK = 0xFEU;
  int BaseCellX = (ChunkVal->ChunkX * BIOME_CELLS_PER_CHUNK) - 1;
  int BaseCellY = ChunkVal->ChunkY * BIOME_CELLS_PER_CHUNK;
  int BaseCellZ = (ChunkVal->ChunkZ * BIOME_CELLS_PER_CHUNK) - 1;

  for (int Ox = 0; Ox < SNAPSHOT_BIOME_SPAN_XZ; Ox++) {
    for (int Oy = 0; Oy < SNAPSHOT_BIOME_SPAN_Y; Oy++) {
      for (int Oz = 0; Oz < SNAPSHOT_BIOME_SPAN_XZ; Oz++) {
        int CellX = BaseCellX + Ox;
        int CellY = BaseCellY + Oy;
        int CellZ = BaseCellZ + Oz;

        unsigned char FromSnapshot =
            SnapshotBiomeCell(Snap, CellX, CellY, CellZ, PROBE_FALLBACK);
        unsigned char FromWorld =
            GetBiomeCellFromWorld(WorldVal, CellX, CellY, CellZ, PROBE_FALLBACK);

        if (FromSnapshot != FromWorld) {
          if (Mismatches < VERIFY_MAX_REPORTED) {
            LogError("SNAPSHOT: biome cell (%d,%d,%d): snapshot %u, world %u",
                     CellX, CellY, CellZ, (unsigned)FromSnapshot,
                     (unsigned)FromWorld);
          }
          Mismatches++;
        }
      }
    }
  }

  // The per-block local lookup the tint path uses for its fallback.
  for (int LocalX = 0; LocalX < CHUNK_SIZE; LocalX++) {
    for (int LocalY = 0; LocalY < CHUNK_SIZE; LocalY++) {
      for (int LocalZ = 0; LocalZ < CHUNK_SIZE; LocalZ++) {
        unsigned char FromSnapshot =
            SnapshotLocalBiome(Snap, LocalX, LocalY, LocalZ);
        unsigned char FromWorld =
            GetChunkBiomeAtLocal(ChunkVal, LocalX, LocalY, LocalZ);
        if (FromSnapshot != FromWorld) {
          if (Mismatches < VERIFY_MAX_REPORTED) {
            LogError("SNAPSHOT: local biome (%d,%d,%d): snapshot %u, chunk %u",
                     LocalX, LocalY, LocalZ, (unsigned)FromSnapshot,
                     (unsigned)FromWorld);
          }
          Mismatches++;
        }
      }
    }
  }

  return Mismatches;
}

#endif

unsigned char SnapshotBiomeCell(const ChunkSnapshot *Snap, int CellX, int CellY,
                                int CellZ, unsigned char Fallback) {
  int Ox = CellX - ((Snap->ChunkX * BIOME_CELLS_PER_CHUNK) - 1);
  int Oy = CellY - (Snap->ChunkY * BIOME_CELLS_PER_CHUNK);
  int Oz = CellZ - ((Snap->ChunkZ * BIOME_CELLS_PER_CHUNK) - 1);

  if (Ox < 0 || Ox >= SNAPSHOT_BIOME_SPAN_XZ || Oy < 0 ||
      Oy >= SNAPSHOT_BIOME_SPAN_Y || Oz < 0 || Oz >= SNAPSHOT_BIOME_SPAN_XZ) {
    return Fallback;
  }
  if (Snap->BiomeCellPresent[Ox][Oy][Oz] == 0) {
    return Fallback;
  }
  return Snap->BiomeCells[Ox][Oy][Oz];
}
