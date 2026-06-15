#ifndef CHUNK_SERIAL_H
#define CHUNK_SERIAL_H

#include "chunk.h"
#include <stdint.h>
#include <stdbool.h>

// Serializes chunk data into outBuffer.
// Returns the size of the serialized data (bytes written).
// Sets *isRaw to true if it falls back to raw 4096-byte copy, false if RLE.
int ChunkSerialize(const Chunk *chunk, uint8_t *outBuffer, bool *isRaw);

// Deserializes data from inBuffer of size dataSize.
// isRaw indicates if the data is raw 4096-byte copy or RLE.
// Returns true on success, false on validation error (destination chunk remains untouched).
bool ChunkDeserialize(Chunk *chunk, const uint8_t *inBuffer, int dataSize, bool isRaw);

#endif // CHUNK_SERIAL_H
