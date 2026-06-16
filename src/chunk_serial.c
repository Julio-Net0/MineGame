#include "chunk_serial.h"
#include <string.h>

int ChunkSerialize(const Chunk *chunk, uint8_t *outBuffer, bool *isRaw) {
  int outIndex = 0;
  unsigned char currentId = chunk->data[0][0][0];
  int currentRun = 0;

  for (int x = 0; x < CHUNK_SIZE; x++) {
    for (int y = 0; y < CHUNK_SIZE; y++) {
      for (int z = 0; z < CHUNK_SIZE; z++) {
        unsigned char id = chunk->data[x][y][z];
        if (id == currentId && currentRun < MAX_RLE_RUN_LENGTH) {
          currentRun++;
        } else {
          // Write the previous run
          if (outIndex + 2 >= CHUNK_VOLUME) {
            // RLE would reach or exceed CHUNK_VOLUME bytes: fall back to raw copy
            *isRaw = true;
            memcpy(outBuffer, chunk->data, CHUNK_VOLUME);
            return CHUNK_VOLUME;
          }
          outBuffer[outIndex++] = currentId;
          outBuffer[outIndex++] = (uint8_t)currentRun;
          currentId = id;
          currentRun = 1;
        }
      }
    }
  }

  // Write the final run
  if (outIndex + 2 >= CHUNK_VOLUME) {
    *isRaw = true;
    memcpy(outBuffer, chunk->data, CHUNK_VOLUME);
    return CHUNK_VOLUME;
  }
  outBuffer[outIndex++] = currentId;
  outBuffer[outIndex++] = (uint8_t)currentRun;

  *isRaw = false;
  return outIndex;
}

bool ChunkDeserialize(Chunk *chunk, const uint8_t *inBuffer, int dataSize,
                      bool isRaw) {
  unsigned char tempData[CHUNK_SIZE][CHUNK_SIZE][CHUNK_SIZE] = {0};
  int solidCount = 0;

  if (isRaw) {
    if (dataSize != CHUNK_VOLUME) {
      return false;
    }
    memcpy(tempData, inBuffer, CHUNK_VOLUME);
    for (int i = 0; i < CHUNK_VOLUME; i++) {
      if (inBuffer[i] != 0) {
        solidCount++;
      }
    }
  } else {
    int idx = 0;
    int blocksDecoded = 0;
    unsigned char *flatData = (unsigned char *)tempData;
    while (idx < dataSize) {
      if (idx + 2 > dataSize) {
        return false; // incomplete pair
      }
      unsigned char id = inBuffer[idx++];
      int run = inBuffer[idx++];
      if (run == 0) {
        return false; // invalid run length
      }
      if (blocksDecoded + run > CHUNK_VOLUME) {
        return false; // exceeds chunk boundaries
      }
      memset(flatData + blocksDecoded, id, run);
      if (id != 0) {
        solidCount += run;
      }
      blocksDecoded += run;
    }
    if (blocksDecoded != CHUNK_VOLUME) {
      return false; // run lengths don't sum to CHUNK_VOLUME
    }
  }

  // Copy values to the destination chunk
  memcpy(chunk->data, tempData, sizeof(chunk->data));
  chunk->solidBlockCount = solidCount;
  chunk->isDirty = true;
  return true;
}
