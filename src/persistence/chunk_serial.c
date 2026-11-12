#include "persistence/chunk_serial.h"
#include "world/chunk.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
  SERIAL_BYTE_MASK = 0xFF,
  SERIAL_BYTE_SHIFT = 8,
  RLE_PAIR_BYTES = 3,
  RAW_ID_BYTES = 2
};

static void WriteIdLe(uint8_t *Out, BlockID Id) {
  Out[0] = (uint8_t)(Id & SERIAL_BYTE_MASK);
  Out[1] = (uint8_t)((unsigned)Id >> SERIAL_BYTE_SHIFT);
}

static BlockID ReadIdLe(const uint8_t *In) {
  return (BlockID)((unsigned)In[0] | ((unsigned)In[1] << SERIAL_BYTE_SHIFT));
}

static int WriteRaw(const Chunk *ChunkVal, uint8_t *OutBuffer, bool *IsRaw) {
  int IdxI = 0;
  for (int IdxX = 0; IdxX < CHUNK_SIZE; IdxX++) {
    for (int IdxY = 0; IdxY < CHUNK_SIZE; IdxY++) {
      for (int IdxZ = 0; IdxZ < CHUNK_SIZE; IdxZ++) {
        WriteIdLe(&OutBuffer[(size_t)IdxI * RAW_ID_BYTES],
                  ChunkGetLocal(ChunkVal, IdxX, IdxY, IdxZ));
        IdxI++;
      }
    }
  }
  *IsRaw = true;
  return CHUNK_SERIAL_MAX_BYTES;
}

// The run byte caps a run at MAX_RLE_RUN_LENGTH, so a uniform chunk is the
// same run list the cell walk would produce, emitted without reading a cell.
static int WriteUniformRuns(BlockID BlockId, uint8_t *OutBuffer) {
  int OutIndex = 0;
  int Remaining = (int)CHUNK_VOLUME;
  while (Remaining > 0) {
    int Run = (Remaining < MAX_RLE_RUN_LENGTH) ? Remaining : MAX_RLE_RUN_LENGTH;
    WriteIdLe(&OutBuffer[OutIndex], BlockId);
    OutBuffer[OutIndex + RAW_ID_BYTES] = (uint8_t)Run;
    OutIndex += RLE_PAIR_BYTES;
    Remaining -= Run;
  }
  return OutIndex;
}

int ChunkSerialize(const Chunk *ChunkVal, uint8_t *OutBuffer, bool *IsRaw) {
  if (ChunkVal->Blocks == (DenseBlockBuffer *)0) {
    *IsRaw = false;
    return WriteUniformRuns(ChunkVal->UniformBlockId, OutBuffer);
  }

  int OutIndex = 0;
  BlockID CurrentId = ChunkGetLocal(ChunkVal, 0, 0, 0);
  int CurrentRun = 0;

  for (int IdxX = 0; IdxX < CHUNK_SIZE; IdxX++) {
    for (int IdxY = 0; IdxY < CHUNK_SIZE; IdxY++) {
      for (int IdxZ = 0; IdxZ < CHUNK_SIZE; IdxZ++) {
        BlockID BlockId = ChunkGetLocal(ChunkVal, IdxX, IdxY, IdxZ);
        if (BlockId == CurrentId && CurrentRun < MAX_RLE_RUN_LENGTH) {
          CurrentRun++;
        } else {
          // RLE that would reach the raw size gains nothing: store raw instead.
          if (OutIndex + RLE_PAIR_BYTES >= CHUNK_SERIAL_MAX_BYTES) {
            return WriteRaw(ChunkVal, OutBuffer, IsRaw);
          }
          WriteIdLe(&OutBuffer[OutIndex], CurrentId);
          OutBuffer[OutIndex + RAW_ID_BYTES] = (uint8_t)CurrentRun;
          OutIndex += RLE_PAIR_BYTES;
          CurrentId = BlockId;
          CurrentRun = 1;
        }
      }
    }
  }

  if (OutIndex + RLE_PAIR_BYTES >= CHUNK_SERIAL_MAX_BYTES) {
    return WriteRaw(ChunkVal, OutBuffer, IsRaw);
  }
  WriteIdLe(&OutBuffer[OutIndex], CurrentId);
  OutBuffer[OutIndex + RAW_ID_BYTES] = (uint8_t)CurrentRun;
  OutIndex += RLE_PAIR_BYTES;

  *IsRaw = false;
  return OutIndex;
}

bool ChunkDeserialize(Chunk *ChunkVal, const uint8_t *InBuffer, int DataSize,
                      bool IsRaw) {
  BlockID TempData[CHUNK_SIZE][CHUNK_SIZE][CHUNK_SIZE] = {0};
  BlockID *FlatData = &TempData[0][0][0];
  int SolidCount = 0;
  bool Uniform = true;

  if (IsRaw) {
    if (DataSize != CHUNK_SERIAL_MAX_BYTES) {
      return false;
    }
    for (int IdxI = 0; IdxI < (int)CHUNK_VOLUME; IdxI++) {
      FlatData[IdxI] = ReadIdLe(&InBuffer[(size_t)IdxI * RAW_ID_BYTES]);
      if (FlatData[IdxI] != BLOCK_ID_AIR) {
        SolidCount++;
      }
      Uniform &= (FlatData[IdxI] == FlatData[0]);
    }
  } else {
    if (DataSize <= 0 || DataSize % RLE_PAIR_BYTES != 0) {
      return false; // truncated or misaligned run list
    }
    int BlocksDecoded = 0;
    for (int Idx = 0; Idx < DataSize; Idx += RLE_PAIR_BYTES) {
      BlockID BlockId = ReadIdLe(&InBuffer[Idx]);
      int Run = InBuffer[Idx + RAW_ID_BYTES];
      if (Run == 0) {
        return false; // invalid run length
      }
      if (BlocksDecoded + Run > (int)CHUNK_VOLUME) {
        return false; // exceeds chunk boundaries
      }
      for (int IdxI = 0; IdxI < Run; IdxI++) {
        FlatData[BlocksDecoded + IdxI] = BlockId;
      }
      if (BlockId != BLOCK_ID_AIR) {
        SolidCount += Run;
      }
      Uniform &= (BlockId == ReadIdLe(&InBuffer[0]));
      BlocksDecoded += Run;
    }
    if (BlocksDecoded != (int)CHUNK_VOLUME) {
      return false; // run lengths don't sum to CHUNK_VOLUME
    }
  }

  // A pooled chunk loads a homogeneous stream as scalar storage, leasing
  // nothing; a standalone chunk keeps its attached buffer and is filled.
  if (Uniform && ChunkVal->Pool != (struct DenseBlockPool *)0) {
    ChunkResetStorage(ChunkVal, FlatData[0]);
    ChunkVal->SolidBlockCount = SolidCount;
    ChunkVal->IsDirty = true;
    return true;
  }
  if (!ChunkBeginFill(ChunkVal)) {
    return false;
  }
  BlockID *DestFlat = &(*ChunkVal->Blocks)[0][0][0];
  for (int IdxI = 0; IdxI < (int)CHUNK_VOLUME; IdxI++) {
    DestFlat[IdxI] = FlatData[IdxI];
  }
  ChunkVal->SolidBlockCount = SolidCount;
  ChunkVal->IsDirty = true;
  return true;
}
