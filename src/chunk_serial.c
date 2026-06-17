#include "chunk_serial.h"
#include "chunk.h"
#include <stdbool.h>
#include <stdint.h>

int ChunkSerialize(const Chunk *ChunkVal, uint8_t *OutBuffer, bool *IsRaw) {
  int OutIndex = 0;
  unsigned char CurrentId = ChunkVal->Data[0][0][0];
  int CurrentRun = 0;

  #pragma unroll 4
  for (int IdxX = 0; IdxX < CHUNK_SIZE; IdxX++) {
    #pragma unroll 4
    for (int IdxY = 0; IdxY < CHUNK_SIZE; IdxY++) {
      #pragma unroll 4
      for (int IdxZ = 0; IdxZ < CHUNK_SIZE; IdxZ++) {
        unsigned char BlockId = ChunkVal->Data[IdxX][IdxY][IdxZ];
        if (BlockId == CurrentId && CurrentRun < MAX_RLE_RUN_LENGTH) {
          CurrentRun++;
        } else {
          // Write the previous run
          if (OutIndex + 2 >= (int)CHUNK_VOLUME) {
            // RLE would reach or exceed CHUNK_VOLUME bytes: fall back to raw copy
            *IsRaw = true;
            const uint8_t *SrcFlat = (const uint8_t *)ChunkVal->Data;
            #pragma unroll 4
            for (int IdxI = 0; IdxI < (int)CHUNK_VOLUME; IdxI++) {
              OutBuffer[IdxI] = SrcFlat[IdxI];
            }
            return (int)CHUNK_VOLUME;
          }
          OutBuffer[OutIndex++] = CurrentId;
          OutBuffer[OutIndex++] = (uint8_t)CurrentRun;
          CurrentId = BlockId;
          CurrentRun = 1;
        }
      }
    }
  }

  // Write the final run
  if (OutIndex + 2 >= (int)CHUNK_VOLUME) {
    *IsRaw = true;
    const uint8_t *SrcFlat = (const uint8_t *)ChunkVal->Data;
    #pragma unroll 4
    for (int IdxI = 0; IdxI < (int)CHUNK_VOLUME; IdxI++) {
      OutBuffer[IdxI] = SrcFlat[IdxI];
    }
    return (int)CHUNK_VOLUME;
  }
  OutBuffer[OutIndex++] = CurrentId;
  OutBuffer[OutIndex++] = (uint8_t)CurrentRun;

  *IsRaw = false;
  return OutIndex;
}

bool ChunkDeserialize(Chunk *ChunkVal, const uint8_t *InBuffer, int DataSize,
                      bool IsRaw) {
  unsigned char TempData[CHUNK_SIZE][CHUNK_SIZE][CHUNK_SIZE] = {0};
  int SolidCount = 0;

  if (IsRaw) {
    if (DataSize != (int)CHUNK_VOLUME) {
      return false;
    }
    uint8_t *DestFlat = (uint8_t *)TempData;
    #pragma unroll 4
    for (int IdxI = 0; IdxI < (int)CHUNK_VOLUME; IdxI++) {
      DestFlat[IdxI] = InBuffer[IdxI];
    }
    #pragma unroll 4
    for (int IdxI = 0; IdxI < (int)CHUNK_VOLUME; IdxI++) {
      if (InBuffer[IdxI] != 0) {
        SolidCount++;
      }
    }
  } else {
    int Idx = 0;
    int BlocksDecoded = 0;
    unsigned char *FlatData = (unsigned char *)TempData;
    while (Idx < DataSize) {
      if (Idx + 2 > DataSize) {
        return false; // incomplete pair
      }
      unsigned char BlockId = InBuffer[Idx++];
      int Run = InBuffer[Idx++];
      if (Run == 0) {
        return false; // invalid run length
      }
      if (BlocksDecoded + Run > (int)CHUNK_VOLUME) {
        return false; // exceeds chunk boundaries
      }
      int RunLimit = Run;
      if (RunLimit > MAX_RLE_RUN_LENGTH) {
        RunLimit = MAX_RLE_RUN_LENGTH;
      }
      #pragma unroll 4
      for (int IdxI = 0; IdxI < MAX_RLE_RUN_LENGTH; IdxI++) {
        if (IdxI < RunLimit) {
          FlatData[BlocksDecoded + IdxI] = BlockId;
        }
      }
      if (BlockId != 0) {
        SolidCount += Run;
      }
      BlocksDecoded += Run;
    }
    if (BlocksDecoded != (int)CHUNK_VOLUME) {
      return false; // run lengths don't sum to CHUNK_VOLUME
    }
  }

  // Copy values to the destination chunk
  uint8_t *DestFlat = (uint8_t *)ChunkVal->Data;
  const uint8_t *SrcFlat = (const uint8_t *)TempData;
  #pragma unroll 4
  for (int IdxI = 0; IdxI < (int)CHUNK_VOLUME; IdxI++) {
    DestFlat[IdxI] = SrcFlat[IdxI];
  }
  ChunkVal->SolidBlockCount = SolidCount;
  ChunkVal->IsDirty = true;
  return true;
}
