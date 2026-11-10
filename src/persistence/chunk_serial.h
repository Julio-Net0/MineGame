#ifndef CHUNK_SERIAL_H
#define CHUNK_SERIAL_H

#include "world/chunk.h"
#include <stdbool.h>
#include <stdint.h>

#define MAX_RLE_RUN_LENGTH 255

// Largest encoded chunk in any mode: the raw fallback stores every voxel as a
// little-endian uint16. RLE runs are (uint16 LE id, uint8 run) triples and fall
// back to raw before reaching this size.
#define CHUNK_SERIAL_MAX_BYTES ((int)(CHUNK_VOLUME * sizeof(BlockID)))

int ChunkSerialize(const Chunk *ChunkVal, uint8_t *OutBuffer, bool *IsRaw);

bool ChunkDeserialize(Chunk *ChunkVal, const uint8_t *InBuffer, int DataSize,
                      bool IsRaw);

#endif // CHUNK_SERIAL_H
