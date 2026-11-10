#ifndef NET_PROTOCOL_H
#define NET_PROTOCOL_H

#include "core/vecmath.h"
#include "player/player.h"
#include "world/chunk.h"

#include <stdbool.h>
#include <stdint.h>

// Binary client/server wire format. Every packet starts with a one-byte
// PacketType tag; multi-byte fields are little-endian and written field by
// field, so the wire layout never depends on C struct layout or padding.

typedef enum {
  PKT_INVALID = 0,
  PKT_CLIENT_INPUT = 1,
  PKT_SERVER_PLAYER_STATE = 2,
  PKT_SERVER_CHUNK_DATA = 3,
  PKT_SERVER_BLOCK_DELTA = 4,
} PacketType;

// Wire sizes are int enum constants so size arithmetic stays in int and never
// mixes with the size_t CHUNK_VOLUME.
enum {
  NET_BIOME_MAP_BYTES =
      BIOME_CELLS_PER_CHUNK * BIOME_CELLS_PER_CHUNK * BIOME_CELLS_PER_CHUNK,

  // tag + ChunkX/Y/Z + IsRaw + DataSize
  NET_CHUNK_HEADER_BYTES = 1 + (3 * 4) + 1 + 2,

  // tag + ClientTick + MoveX + MoveForward + buttons + HotbarSelect +
  // HotbarScroll + Forward
  NET_CLIENT_INPUT_BYTES = 1 + 8 + 1 + 1 + 1 + 1 + 1 + (3 * 4),

  // tag + ServerTick + LastProcessedClientTick + Position + Velocity + flags +
  // SelectedHotbarSlot
  NET_SERVER_PLAYER_STATE_BYTES = 1 + 8 + 8 + (3 * 4) + (3 * 4) + 1 + 1,

  // tag + BlockX/Y/Z + BlockId
  NET_BLOCK_DELTA_BYTES = 1 + (3 * 4) + 1,

  // Largest packet: chunk data with a raw (uncompressed) voxel payload.
  NET_MAX_PACKET_BYTES =
      NET_CHUNK_HEADER_BYTES + (int)CHUNK_VOLUME + NET_BIOME_MAP_BYTES,
};

// Axis quantization scale: MoveX/MoveForward travel as round(v * 127).
#define NET_AXIS_QUANT_SCALE 127.0F

typedef struct {
  uint64_t ClientTick;
  PlayerInput Input;
  Vec3 Forward;
} PktClientInput;

typedef struct {
  uint64_t ServerTick;
  uint64_t LastProcessedClientTick;
  Vec3 Position;
  Vec3 Velocity;
  bool IsGrounded;
  bool Noclip;
  int SelectedHotbarSlot;
} PktServerPlayerState;

// Header of a PKT_SERVER_CHUNK_DATA packet. The voxel payload and biome map
// follow it on the wire and are decoded straight into a Chunk.
typedef struct {
  int ChunkX;
  int ChunkY;
  int ChunkZ;
  bool IsRaw;
  uint16_t DataSize;
} PktServerChunkData;

typedef struct {
  int BlockX;
  int BlockY;
  int BlockZ;
  uint8_t BlockId;
} PktServerBlockDelta;

// Tag of the packet in Buf, or PKT_INVALID for an empty buffer or unknown tag.
PacketType NetPeekPacketType(const uint8_t *Buf, int Size);

// Encoders return the number of bytes written, or 0 when Capacity is too small.
// Decoders return false on a wrong tag, wrong length, or out-of-range field and
// never read past Size.
int NetEncodeClientInput(const PktClientInput *Pkt, uint8_t *OutBuf,
                         int Capacity);
bool NetDecodeClientInput(PktClientInput *OutPkt, const uint8_t *Buf, int Size);

int NetEncodeServerPlayerState(const PktServerPlayerState *Pkt, uint8_t *OutBuf,
                               int Capacity);
bool NetDecodeServerPlayerState(PktServerPlayerState *OutPkt,
                                const uint8_t *Buf, int Size);

int NetEncodeChunkData(const Chunk *ChunkVal, uint8_t *OutBuf, int Capacity);

// Validates the whole packet and returns only its header, so a receiver can
// pick the destination chunk before decoding the payload into it.
bool NetDecodeChunkHeader(PktServerChunkData *OutHeader, const uint8_t *Buf,
                          int Size);

// On success OutChunk receives the coordinates, voxels, SolidBlockCount and
// BiomeMap, and is marked IsGenerated and IsDirty. On failure it is untouched.
bool NetDecodeChunkData(Chunk *OutChunk, const uint8_t *Buf, int Size);

int NetEncodeBlockDelta(const PktServerBlockDelta *Pkt, uint8_t *OutBuf,
                        int Capacity);
bool NetDecodeBlockDelta(PktServerBlockDelta *OutPkt, const uint8_t *Buf,
                         int Size);

#endif
