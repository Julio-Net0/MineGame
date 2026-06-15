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
        if (id == currentId && currentRun < 255) {
          currentRun++;
        } else {
          // Write the previous run
          if (outIndex + 2 >= 4096) {
            // RLE would reach or exceed 4096 bytes: fall back to raw copy
            *isRaw = true;
            for (int rx = 0; rx < CHUNK_SIZE; rx++) {
              for (int ry = 0; ry < CHUNK_SIZE; ry++) {
                for (int rz = 0; rz < CHUNK_SIZE; rz++) {
                  outBuffer[rx * CHUNK_SIZE * CHUNK_SIZE + ry * CHUNK_SIZE + rz] = chunk->data[rx][ry][rz];
                }
              }
            }
            return 4096;
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
  if (outIndex + 2 >= 4096) {
    *isRaw = true;
    for (int rx = 0; rx < CHUNK_SIZE; rx++) {
      for (int ry = 0; ry < CHUNK_SIZE; ry++) {
        for (int rz = 0; rz < CHUNK_SIZE; rz++) {
          outBuffer[rx * CHUNK_SIZE * CHUNK_SIZE + ry * CHUNK_SIZE + rz] = chunk->data[rx][ry][rz];
        }
      }
    }
    return 4096;
  }
  outBuffer[outIndex++] = currentId;
  outBuffer[outIndex++] = (uint8_t)currentRun;

  *isRaw = false;
  return outIndex;
}

bool ChunkDeserialize(Chunk *chunk, const uint8_t *inBuffer, int dataSize, bool isRaw) {
  unsigned char tempData[CHUNK_SIZE][CHUNK_SIZE][CHUNK_SIZE] = {0};
  int solidCount = 0;

  if (isRaw) {
    if (dataSize != 4096) {
      return false;
    }
    int idx = 0;
    for (int x = 0; x < CHUNK_SIZE; x++) {
      for (int y = 0; y < CHUNK_SIZE; y++) {
        for (int z = 0; z < CHUNK_SIZE; z++) {
          unsigned char id = inBuffer[idx++];
          tempData[x][y][z] = id;
          if (id != 0) {
            solidCount++;
          }
        }
      }
    }
  } else {
    int idx = 0;
    int blocksDecoded = 0;
    while (idx < dataSize) {
      if (idx + 2 > dataSize) {
        return false; // incomplete pair
      }
      unsigned char id = inBuffer[idx++];
      int run = inBuffer[idx++];
      if (run == 0) {
        return false; // invalid run length
      }
      if (blocksDecoded + run > 4096) {
        return false; // exceeds chunk boundaries
      }
      for (int i = 0; i < run; i++) {
        int pos = blocksDecoded + i;
        int x = pos / (CHUNK_SIZE * CHUNK_SIZE);
        int y = (pos / CHUNK_SIZE) % CHUNK_SIZE;
        int z = pos % CHUNK_SIZE;
        tempData[x][y][z] = id;
        if (id != 0) {
          solidCount++;
        }
      }
      blocksDecoded += run;
    }
    if (blocksDecoded != 4096) {
      return false; // run lengths don't sum to 4096
    }
  }

  // Copy values to the destination chunk
  memcpy(chunk->data, tempData, sizeof(chunk->data));
  chunk->solidBlockCount = solidCount;
  chunk->isDirty = true;
  return true;
}
