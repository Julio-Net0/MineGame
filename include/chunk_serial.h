#ifndef CHUNK_SERIAL_H
#define CHUNK_SERIAL_H

#include "chunk.h"
#include <stdbool.h>
#include <stdint.h>

#define MAX_RLE_RUN_LENGTH 255

int ChunkSerialize(const Chunk *ChunkVal, uint8_t *OutBuffer, bool *IsRaw);

bool ChunkDeserialize(Chunk *ChunkVal, const uint8_t *InBuffer, int DataSize,
                      bool IsRaw);

#endif // CHUNK_SERIAL_H
