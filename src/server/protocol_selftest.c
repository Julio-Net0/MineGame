#include "server/protocol_selftest.h"

#include "core/log.h"
#include "net/loopback.h"
#include "net/protocol.h"
#include "world/block_system.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
  GUARD_BYTE = 0xAA,
  GUARD_BYTES = 16,
  UNKNOWN_PACKET_TAG = 0x7F,
  INPUT_FLAG_COMBINATIONS = 32,
  LCG_MULTIPLIER = 1103515245,
  LCG_INCREMENT = 12345,
  LCG_SHIFT = 16,
  RAW_BLOCK_ID_RANGE = BLOCK_REGISTRY_SIZE,
  WIDE_BLOCK_ID = 300,
  WRAP_ITERATIONS = 400,
  WRAP_PACKET_BYTES = 3001,
  SMALL_PACKET_BYTES = 100,
  SMALL_RECV_BYTES = 50,
  CHUNK_DATA_SIZE_OFFSET = 14,
  CHUNK_RAW_FLAG_OFFSET = 13,
  INPUT_FLAGS_OFFSET = 11,
  INPUT_HOTBAR_OFFSET = 12,
  INPUT_MOVE_X_OFFSET = 9,
  INPUT_FORWARD_OFFSET = 14,
  STONE_LAYER_TOP = 8,
  STONE_BLOCK_ID = 3,
  BYTE_MASK = 0xFF,
  BITS_PER_BYTE = 8,
  RLE_RUN_BYTE_OFFSET = 2,
};

static uint8_t GPacketBuf[NET_MAX_PACKET_BYTES + GUARD_BYTES];
static uint8_t GScratchBuf[NET_MAX_PACKET_BYTES + GUARD_BYTES];

static bool Expect(bool Condition, const char *CheckName) {
  if (!Condition) {
    LogError("PROTOCOL_TEST: FAIL %s", CheckName);
    return false;
  }
  LogInfo("PROTOCOL_TEST: ok %s", CheckName);
  return true;
}

static bool Vec3Equal(Vec3 A, Vec3 B) {
  return A.x == B.x && A.y == B.y && A.z == B.z;
}

static bool InputsEqual(const PktClientInput *A, const PktClientInput *B) {
  return A->ClientTick == B->ClientTick && A->Input.MoveX == B->Input.MoveX &&
         A->Input.MoveForward == B->Input.MoveForward &&
         A->Input.Jump == B->Input.Jump &&
         A->Input.AscendHeld == B->Input.AscendHeld &&
         A->Input.DescendHeld == B->Input.DescendHeld &&
         A->Input.HotbarSelect == B->Input.HotbarSelect &&
         A->Input.HotbarScroll == B->Input.HotbarScroll &&
         A->Input.Break == B->Input.Break && A->Input.Place == B->Input.Place &&
         A->Forward.x == B->Forward.x && A->Forward.y == B->Forward.y &&
         A->Forward.z == B->Forward.z;
}

static PktClientInput MakeSampleInput(void) {
  const uint64_t SAMPLE_TICK = 0x0102030405060708ULL;
  const int SAMPLE_SCROLL = -3;
  const Vec3 SAMPLE_FORWARD = {0.25F, -0.5F, 0.75F};
  return (PktClientInput){
      .ClientTick = SAMPLE_TICK,
      .Input =
          {
              .MoveX = -1.0F,
              .MoveForward = 1.0F,
              .Jump = true,
              .AscendHeld = false,
              .DescendHeld = true,
              .HotbarSelect = HOTBAR_SIZE - 1,
              .HotbarScroll = SAMPLE_SCROLL,
              .Break = true,
              .Place = false,
          },
      .Forward = SAMPLE_FORWARD,
  };
}

// Every shorter prefix and a one-byte-longer buffer must be rejected.
typedef bool (*DecodeProbe)(const uint8_t *Buf, int Size);

static bool ProbeClientInput(const uint8_t *Buf, int Size) {
  PktClientInput Out = {0};
  return NetDecodeClientInput(&Out, Buf, Size);
}

static bool ProbePlayerState(const uint8_t *Buf, int Size) {
  PktServerPlayerState Out = {0};
  return NetDecodeServerPlayerState(&Out, Buf, Size);
}

static bool ProbeBlockDelta(const uint8_t *Buf, int Size) {
  PktServerBlockDelta Out = {0};
  return NetDecodeBlockDelta(&Out, Buf, Size);
}

static bool ProbeChunkHeader(const uint8_t *Buf, int Size) {
  PktServerChunkData Out = {0};
  return NetDecodeChunkHeader(&Out, Buf, Size);
}

static bool RejectsBadLengths(DecodeProbe Probe, const uint8_t *Packet,
                              int Size) {
  for (int Len = 0; Len < Size; Len++) {
    if (Probe(Packet, Len)) {
      return false;
    }
  }
  memcpy(GScratchBuf, Packet, (size_t)Size);
  GScratchBuf[Size] = 0;
  return !Probe(GScratchBuf, Size + 1);
}

static bool RejectsWrongTag(DecodeProbe Probe, const uint8_t *Packet,
                            int Size) {
  memcpy(GScratchBuf, Packet, (size_t)Size);
  GScratchBuf[0] = UNKNOWN_PACKET_TAG;
  return !Probe(GScratchBuf, Size);
}

// Encoding into a buffer one byte too small must fail without touching the
// bytes past Capacity.
typedef int (*EncodeProbe)(uint8_t *OutBuf, int Capacity);

static int EncodeSampleInput(uint8_t *OutBuf, int Capacity) {
  PktClientInput Pkt = MakeSampleInput();
  return NetEncodeClientInput(&Pkt, OutBuf, Capacity);
}

static bool RefusesSmallCapacity(EncodeProbe Probe, int FullSize) {
  int Capacity = FullSize - 1;
  memset(GScratchBuf, GUARD_BYTE, sizeof(GScratchBuf));
  if (Probe(GScratchBuf, Capacity) != 0) {
    return false;
  }
  for (int Idx = Capacity; Idx < FullSize + GUARD_BYTES; Idx++) {
    if (GScratchBuf[Idx] != GUARD_BYTE) {
      return false;
    }
  }
  return true;
}

static bool TestClientInput(void) {
  PktClientInput Sent = MakeSampleInput();
  int Size = NetEncodeClientInput(&Sent, GPacketBuf, NET_MAX_PACKET_BYTES);
  PktClientInput Got = {0};
  if (!Expect(Size == NET_CLIENT_INPUT_BYTES &&
                  NetPeekPacketType(GPacketBuf, Size) == PKT_CLIENT_INPUT &&
                  NetDecodeClientInput(&Got, GPacketBuf, Size) &&
                  InputsEqual(&Sent, &Got),
              "client_input_roundtrip")) {
    return false;
  }

  bool AllFlags = true;
  for (int Mask = 0; Mask < INPUT_FLAG_COMBINATIONS; Mask++) {
    PktClientInput Pkt = MakeSampleInput();
    Pkt.Input.Jump = (Mask & (1 << 0)) != 0;
    Pkt.Input.AscendHeld = (Mask & (1 << 1)) != 0;
    Pkt.Input.DescendHeld = (Mask & (1 << 2)) != 0;
    Pkt.Input.Break = (Mask & (1 << 3)) != 0;
    Pkt.Input.Place = (Mask & (1 << 4)) != 0;
    Pkt.Input.MoveX = 0.0F;
    Pkt.Input.HotbarSelect = -1;
    PktClientInput Out = {0};
    int Len = NetEncodeClientInput(&Pkt, GPacketBuf, NET_MAX_PACKET_BYTES);
    AllFlags = AllFlags && NetDecodeClientInput(&Out, GPacketBuf, Len) &&
               InputsEqual(&Pkt, &Out);
  }
  if (!Expect(AllFlags, "client_input_all_button_combinations")) {
    return false;
  }

  const float HALF_AXIS = 0.5F;
  const float OUT_OF_RANGE_AXIS = 3.0F;
  PktClientInput Analog = MakeSampleInput();
  Analog.Input.MoveX = HALF_AXIS;
  Analog.Input.MoveForward = OUT_OF_RANGE_AXIS;
  PktClientInput AnalogOut = {0};
  int AnalogLen =
      NetEncodeClientInput(&Analog, GPacketBuf, NET_MAX_PACKET_BYTES);
  if (!Expect(NetDecodeClientInput(&AnalogOut, GPacketBuf, AnalogLen) &&
                  fabsf(AnalogOut.Input.MoveX - HALF_AXIS) <=
                      1.0F / NET_AXIS_QUANT_SCALE &&
                  AnalogOut.Input.MoveForward == 1.0F,
              "client_input_axis_quantization")) {
    return false;
  }

  Size = NetEncodeClientInput(&Sent, GPacketBuf, NET_MAX_PACKET_BYTES);
  if (!Expect(RejectsBadLengths(ProbeClientInput, GPacketBuf, Size) &&
                  RejectsWrongTag(ProbeClientInput, GPacketBuf, Size),
              "client_input_rejects_truncated_and_wrong_tag")) {
    return false;
  }

  const uint8_t UNKNOWN_FLAG_BIT = 0x80;
  const int8_t OUT_OF_RANGE_HOTBAR = HOTBAR_SIZE;
  const float NOT_A_NUMBER = NAN;
  bool Malformed = true;
  memcpy(GScratchBuf, GPacketBuf, (size_t)Size);
  GScratchBuf[INPUT_FLAGS_OFFSET] |= UNKNOWN_FLAG_BIT;
  Malformed = Malformed && !ProbeClientInput(GScratchBuf, Size);
  memcpy(GScratchBuf, GPacketBuf, (size_t)Size);
  GScratchBuf[INPUT_HOTBAR_OFFSET] = (uint8_t)OUT_OF_RANGE_HOTBAR;
  Malformed = Malformed && !ProbeClientInput(GScratchBuf, Size);
  memcpy(GScratchBuf, GPacketBuf, (size_t)Size);
  GScratchBuf[INPUT_MOVE_X_OFFSET] = (uint8_t)INT8_MIN;
  Malformed = Malformed && !ProbeClientInput(GScratchBuf, Size);
  memcpy(GScratchBuf, GPacketBuf, (size_t)Size);
  memcpy(GScratchBuf + INPUT_FORWARD_OFFSET, &NOT_A_NUMBER, sizeof(float));
  Malformed = Malformed && !ProbeClientInput(GScratchBuf, Size);

  PktClientInput Untouched = MakeSampleInput();
  PktClientInput Before = Untouched;
  Malformed = Malformed &&
              !NetDecodeClientInput(&Untouched, GScratchBuf, Size) &&
              InputsEqual(&Untouched, &Before);
  if (!Expect(Malformed, "client_input_rejects_out_of_range_fields")) {
    return false;
  }

  return Expect(RefusesSmallCapacity(EncodeSampleInput, NET_CLIENT_INPUT_BYTES),
                "client_input_refuses_small_capacity");
}

static bool TestClientInputHotbarRequest(void) {
  PktClientInput Sent = MakeSampleInput();
  Sent.Input.HotbarSetRequested = true;
  for (int Slot = 0; Slot < HOTBAR_SIZE; Slot++) {
    Sent.Input.HotbarSet[Slot] = (BlockID)(WIDE_BLOCK_ID + Slot);
  }
  int Size = NetEncodeClientInput(&Sent, GPacketBuf, NET_MAX_PACKET_BYTES);
  PktClientInput Got = {0};
  bool Ok = Size == NET_CLIENT_INPUT_BYTES + NET_HOTBAR_TAIL_BYTES &&
            NetDecodeClientInput(&Got, GPacketBuf, Size) &&
            InputsEqual(&Got, &Sent) && Got.Input.HotbarSetRequested;
  for (int Slot = 0; Ok && Slot < HOTBAR_SIZE; Slot++) {
    Ok = Got.Input.HotbarSet[Slot] == Sent.Input.HotbarSet[Slot];
  }
  if (!Expect(Ok, "client_input_hotbar_request_roundtrip")) {
    return false;
  }

  // The tail without the flag, and the flag without the tail, are both
  // malformed: the flag decides which of the two lengths is exact.
  const uint8_t HOTBAR_FLAG_BIT = 0x20;
  memcpy(GScratchBuf, GPacketBuf, (size_t)Size);
  GScratchBuf[INPUT_FLAGS_OFFSET] &= (uint8_t)~HOTBAR_FLAG_BIT;
  bool Rejected = !ProbeClientInput(GScratchBuf, Size);
  memcpy(GScratchBuf, GPacketBuf, (size_t)NET_CLIENT_INPUT_BYTES);
  Rejected = Rejected && !ProbeClientInput(GScratchBuf, NET_CLIENT_INPUT_BYTES);
  return Expect(Rejected, "client_input_hotbar_flag_length_mismatch_rejected");
}

static PktServerPlayerState MakeSampleState(void) {
  const uint64_t SAMPLE_SERVER_TICK = 987654321ULL;
  const uint64_t SAMPLE_CLIENT_TICK = 0xFFFFFFFF00000001ULL;
  const Vec3 SAMPLE_POSITION = {-1234.5F, 22.5F, 98765.25F};
  const Vec3 SAMPLE_VELOCITY = {0.0F, -9.75F, 4.3F};
  return (PktServerPlayerState){
      .ServerTick = SAMPLE_SERVER_TICK,
      .LastProcessedClientTick = SAMPLE_CLIENT_TICK,
      .Position = SAMPLE_POSITION,
      .Velocity = SAMPLE_VELOCITY,
      .IsGrounded = true,
      .Noclip = false,
      .SelectedHotbarSlot = HOTBAR_SIZE - 1,
  };
}

static int EncodeSampleState(uint8_t *OutBuf, int Capacity) {
  PktServerPlayerState Pkt = MakeSampleState();
  return NetEncodeServerPlayerState(&Pkt, OutBuf, Capacity);
}

static bool TestPlayerState(void) {
  PktServerPlayerState Sent = MakeSampleState();
  int Size =
      NetEncodeServerPlayerState(&Sent, GPacketBuf, NET_MAX_PACKET_BYTES);
  PktServerPlayerState Got = {0};
  bool Ok = Size == NET_SERVER_PLAYER_STATE_BYTES &&
            NetPeekPacketType(GPacketBuf, Size) == PKT_SERVER_PLAYER_STATE &&
            NetDecodeServerPlayerState(&Got, GPacketBuf, Size) &&
            Got.ServerTick == Sent.ServerTick &&
            Got.LastProcessedClientTick == Sent.LastProcessedClientTick &&
            Vec3Equal(Got.Position, Sent.Position) &&
            Vec3Equal(Got.Velocity, Sent.Velocity) &&
            Got.IsGrounded == Sent.IsGrounded && Got.Noclip == Sent.Noclip &&
            Got.SelectedHotbarSlot == Sent.SelectedHotbarSlot;
  if (!Expect(Ok, "player_state_roundtrip")) {
    return false;
  }
  if (!Expect(RejectsBadLengths(ProbePlayerState, GPacketBuf, Size) &&
                  RejectsWrongTag(ProbePlayerState, GPacketBuf, Size),
              "player_state_rejects_truncated_and_wrong_tag")) {
    return false;
  }
  return Expect(
      RefusesSmallCapacity(EncodeSampleState, NET_SERVER_PLAYER_STATE_BYTES),
      "player_state_refuses_small_capacity");
}

static const PktServerBlockDelta SAMPLE_DELTA = {
    .BlockX = -17, .BlockY = 22, .BlockZ = 1000001, .BlockId = WIDE_BLOCK_ID};

static int EncodeSampleDelta(uint8_t *OutBuf, int Capacity) {
  return NetEncodeBlockDelta(&SAMPLE_DELTA, OutBuf, Capacity);
}

static bool TestBlockDelta(void) {
  const PktServerBlockDelta *Sent = &SAMPLE_DELTA;
  int Size = NetEncodeBlockDelta(Sent, GPacketBuf, NET_MAX_PACKET_BYTES);
  PktServerBlockDelta Got = {0};
  bool Ok = Size == NET_BLOCK_DELTA_BYTES &&
            NetPeekPacketType(GPacketBuf, Size) == PKT_SERVER_BLOCK_DELTA &&
            NetDecodeBlockDelta(&Got, GPacketBuf, Size) &&
            Got.BlockX == Sent->BlockX && Got.BlockY == Sent->BlockY &&
            Got.BlockZ == Sent->BlockZ && Got.BlockId == Sent->BlockId;
  if (!Expect(Ok, "block_delta_roundtrip")) {
    return false;
  }
  if (!Expect(RejectsBadLengths(ProbeBlockDelta, GPacketBuf, Size) &&
                  RejectsWrongTag(ProbeBlockDelta, GPacketBuf, Size),
              "block_delta_rejects_truncated_and_wrong_tag")) {
    return false;
  }
  return Expect(RefusesSmallCapacity(EncodeSampleDelta, NET_BLOCK_DELTA_BYTES),
                "block_delta_refuses_small_capacity");
}

static void FillLayeredChunk(Chunk *ChunkVal) {
  const int SAMPLE_CHUNK_X = -3;
  const int SAMPLE_CHUNK_Y = 1;
  const int SAMPLE_CHUNK_Z = 70000;
  ChunkVal->ChunkX = SAMPLE_CHUNK_X;
  ChunkVal->ChunkY = SAMPLE_CHUNK_Y;
  ChunkVal->ChunkZ = SAMPLE_CHUNK_Z;
  for (int X = 0; X < CHUNK_SIZE; X++) {
    for (int Y = 0; Y < CHUNK_SIZE; Y++) {
      for (int Z = 0; Z < CHUNK_SIZE; Z++) {
        // One layer above the stone carries an id past 8 bits, so the RLE
        // path must preserve both id bytes.
        BlockID Id = BLOCK_ID_AIR;
        if (Y < STONE_LAYER_TOP) {
          Id = (BlockID)STONE_BLOCK_ID;
        } else if (Y == STONE_LAYER_TOP) {
          Id = (BlockID)WIDE_BLOCK_ID;
        }
        ChunkVal->Data[X][Y][Z] = Id;
      }
    }
  }
  unsigned char *Biomes = (unsigned char *)ChunkVal->BiomeMap;
  for (int Idx = 0; Idx < NET_BIOME_MAP_BYTES; Idx++) {
    Biomes[Idx] = (unsigned char)(Idx % BIOME_CELL_SIZE);
  }
}

// Noisy voxels defeat RLE, forcing the raw payload path.
static void FillNoisyChunk(Chunk *ChunkVal) {
  const int SAMPLE_CHUNK_Y = -2;
  ChunkVal->ChunkX = 0;
  ChunkVal->ChunkY = SAMPLE_CHUNK_Y;
  ChunkVal->ChunkZ = 0;
  uint32_t State = 1U;
  BlockID *Flat = &ChunkVal->Data[0][0][0];
  for (int Idx = 0; Idx < (int)CHUNK_VOLUME; Idx++) {
    State = (State * (uint32_t)LCG_MULTIPLIER) + (uint32_t)LCG_INCREMENT;
    Flat[Idx] = (BlockID)((State >> LCG_SHIFT) % RAW_BLOCK_ID_RANGE);
  }
  unsigned char *Biomes = (unsigned char *)ChunkVal->BiomeMap;
  for (int Idx = 0; Idx < NET_BIOME_MAP_BYTES; Idx++) {
    Biomes[Idx] = (unsigned char)(NET_BIOME_MAP_BYTES - Idx);
  }
}

static int CountSolid(const Chunk *ChunkVal) {
  const BlockID *Flat = &ChunkVal->Data[0][0][0];
  int Count = 0;
  for (int Idx = 0; Idx < (int)CHUNK_VOLUME; Idx++) {
    Count += (Flat[Idx] != 0U) ? 1 : 0;
  }
  return Count;
}

static bool ChunkRoundTrips(const Chunk *Source, Chunk *Target,
                            bool ExpectRaw) {
  int Size = NetEncodeChunkData(Source, GPacketBuf, NET_MAX_PACKET_BYTES);
  PktServerChunkData Header = {0};
  if (Size <= 0 || NetPeekPacketType(GPacketBuf, Size) != PKT_SERVER_CHUNK_DATA ||
      !NetDecodeChunkHeader(&Header, GPacketBuf, Size) ||
      Header.IsRaw != ExpectRaw || Header.ChunkX != Source->ChunkX ||
      Header.ChunkY != Source->ChunkY || Header.ChunkZ != Source->ChunkZ) {
    return false;
  }

  Target->IsGenerated = false;
  Target->IsDirty = false;
  if (!NetDecodeChunkData(Target, GPacketBuf, Size)) {
    return false;
  }
  return Target->ChunkX == Source->ChunkX && Target->ChunkY == Source->ChunkY &&
         Target->ChunkZ == Source->ChunkZ &&
         memcmp(Target->Data, Source->Data, sizeof(Source->Data)) == 0 &&
         memcmp(Target->BiomeMap, Source->BiomeMap,
                sizeof(Source->BiomeMap)) == 0 &&
         Target->SolidBlockCount == CountSolid(Source) && Target->IsGenerated &&
         Target->IsDirty;
}

static bool ChunkRejectsMalformed(const Chunk *Source, Chunk *Target) {
  int Size = NetEncodeChunkData(Source, GPacketBuf, NET_MAX_PACKET_BYTES);
  if (Size <= 0 || !RejectsBadLengths(ProbeChunkHeader, GPacketBuf, Size) ||
      !RejectsWrongTag(ProbeChunkHeader, GPacketBuf, Size)) {
    return false;
  }

  const uint8_t INVALID_RAW_FLAG = 2;
  const int OVERSIZED_DATA_SIZE = CHUNK_SERIAL_MAX_BYTES + 1;
  memcpy(GScratchBuf, GPacketBuf, (size_t)Size);
  GScratchBuf[CHUNK_RAW_FLAG_OFFSET] = INVALID_RAW_FLAG;
  if (ProbeChunkHeader(GScratchBuf, Size)) {
    return false;
  }

  // An RLE packet relabelled as raw has a payload that is not CHUNK_VOLUME.
  memcpy(GScratchBuf, GPacketBuf, (size_t)Size);
  GScratchBuf[CHUNK_RAW_FLAG_OFFSET] = 1U;
  if (ProbeChunkHeader(GScratchBuf, Size)) {
    return false;
  }

  memcpy(GScratchBuf, GPacketBuf, (size_t)Size);
  GScratchBuf[CHUNK_DATA_SIZE_OFFSET] =
      (uint8_t)((unsigned)OVERSIZED_DATA_SIZE & (unsigned)BYTE_MASK);
  GScratchBuf[CHUNK_DATA_SIZE_OFFSET + 1] =
      (uint8_t)((unsigned)OVERSIZED_DATA_SIZE >> (unsigned)BITS_PER_BYTE);
  if (ProbeChunkHeader(GScratchBuf, Size)) {
    return false;
  }

  // Corrupting the first RLE run length to zero passes the header checks but
  // must fail payload decoding and leave the target chunk untouched.
  memcpy(GScratchBuf, GPacketBuf, (size_t)Size);
  GScratchBuf[NET_CHUNK_HEADER_BYTES + RLE_RUN_BYTE_OFFSET] = 0U;
  memset(Target->Data, 0, sizeof(Target->Data));
  bool Rejected = !NetDecodeChunkData(Target, GScratchBuf, Size);
  if (!Rejected || CountSolid(Target) != 0) {
    return false;
  }

  // Dropping the last payload byte (and shifting the biome map down) keeps the
  // header consistent but leaves a run list that is not a multiple of 3 bytes.
  int RleSize = Size - NET_CHUNK_HEADER_BYTES - NET_BIOME_MAP_BYTES;
  if (RleSize < 2 || RleSize > CHUNK_SERIAL_MAX_BYTES) {
    return false;
  }
  int ShortSize = RleSize - 1;
  memcpy(GScratchBuf, GPacketBuf, (size_t)Size);
  GScratchBuf[CHUNK_DATA_SIZE_OFFSET] =
      (uint8_t)((unsigned)ShortSize & (unsigned)BYTE_MASK);
  GScratchBuf[CHUNK_DATA_SIZE_OFFSET + 1] =
      (uint8_t)((unsigned)ShortSize >> (unsigned)BITS_PER_BYTE);
  memmove(&GScratchBuf[NET_CHUNK_HEADER_BYTES + ShortSize],
          &GPacketBuf[NET_CHUNK_HEADER_BYTES + RleSize],
          (size_t)NET_BIOME_MAP_BYTES);
  return !NetDecodeChunkData(Target, GScratchBuf, Size - 1) &&
         CountSolid(Target) == 0;
}

static int EncodeSampleChunk(uint8_t *OutBuf, int Capacity) {
  Chunk *Source = (Chunk *)calloc(1, sizeof(Chunk));
  if (Source == (Chunk *)0) {
    return -1;
  }
  FillLayeredChunk(Source);
  int Size = NetEncodeChunkData(Source, OutBuf, Capacity);
  free(Source);
  return Size;
}

static bool TestChunkData(void) {
  Chunk *Source = (Chunk *)calloc(1, sizeof(Chunk));
  Chunk *Target = (Chunk *)calloc(1, sizeof(Chunk));
  if (Source == (Chunk *)0 || Target == (Chunk *)0) {
    free(Source);
    free(Target);
    return Expect(false, "chunk_allocation");
  }

  FillLayeredChunk(Source);
  bool Ok = Expect(ChunkRoundTrips(Source, Target, false),
                   "chunk_data_rle_roundtrip");
  if (Ok) {
    int RleSize = NetEncodeChunkData(Source, GPacketBuf, NET_MAX_PACKET_BYTES);
    Ok = Expect(RefusesSmallCapacity(EncodeSampleChunk, RleSize),
                "chunk_data_refuses_small_capacity");
  }
  if (Ok) {
    Ok = Expect(ChunkRejectsMalformed(Source, Target),
                "chunk_data_rejects_truncated_and_malformed");
  }
  if (Ok) {
    FillNoisyChunk(Source);
    Ok = Expect(ChunkRoundTrips(Source, Target, true) &&
                    NetEncodeChunkData(Source, GPacketBuf,
                                       NET_MAX_PACKET_BYTES) ==
                        NET_MAX_PACKET_BYTES,
                "chunk_data_raw_roundtrip");
  }

  free(Source);
  free(Target);
  return Ok;
}

static bool TestPeek(void) {
  const uint8_t UNKNOWN_TAG[1] = {UNKNOWN_PACKET_TAG};
  const uint8_t INVALID_TAG[1] = {PKT_INVALID};
  return Expect(NetPeekPacketType(UNKNOWN_TAG, 0) == PKT_INVALID &&
                    NetPeekPacketType((const uint8_t *)0, 1) == PKT_INVALID &&
                    NetPeekPacketType(UNKNOWN_TAG, 1) == PKT_INVALID &&
                    NetPeekPacketType(INVALID_TAG, 1) == PKT_INVALID,
                "peek_rejects_empty_and_unknown_tags");
}

static void FillPattern(uint8_t *Buf, int Size, int Seed) {
  for (int Idx = 0; Idx < Size; Idx++) {
    Buf[Idx] = (uint8_t)((Idx * (Seed + 1)) + Seed);
  }
}

static bool MatchesPattern(const uint8_t *Buf, int Size, int Seed) {
  for (int Idx = 0; Idx < Size; Idx++) {
    if (Buf[Idx] != (uint8_t)((Idx * (Seed + 1)) + Seed)) {
      return false;
    }
  }
  return true;
}

static bool LoopbackFifo(NetLoopback *Channel) {
  const int SIZES[] = {1, SMALL_PACKET_BYTES, NET_MAX_PACKET_BYTES};
  const int PACKET_COUNT = (int)(sizeof(SIZES) / sizeof(SIZES[0]));

  if (NetRecvFromClient(Channel, GPacketBuf, NET_MAX_PACKET_BYTES) != 0 ||
      NetRecvFromServer(Channel, GPacketBuf, NET_MAX_PACKET_BYTES) != 0) {
    return false;
  }
  for (int Idx = 0; Idx < PACKET_COUNT; Idx++) {
    FillPattern(GPacketBuf, SIZES[Idx], Idx);
    if (!NetSendToServer(Channel, GPacketBuf, SIZES[Idx])) {
      return false;
    }
    FillPattern(GPacketBuf, SIZES[Idx], Idx + PACKET_COUNT);
    if (!NetSendToClient(Channel, GPacketBuf, SIZES[Idx])) {
      return false;
    }
  }
  for (int Idx = 0; Idx < PACKET_COUNT; Idx++) {
    int Got = NetRecvFromClient(Channel, GPacketBuf, NET_MAX_PACKET_BYTES);
    if (Got != SIZES[Idx] || !MatchesPattern(GPacketBuf, Got, Idx)) {
      return false;
    }
  }
  for (int Idx = 0; Idx < PACKET_COUNT; Idx++) {
    int Got = NetRecvFromServer(Channel, GPacketBuf, NET_MAX_PACKET_BYTES);
    if (Got != SIZES[Idx] || !MatchesPattern(GPacketBuf, Got, Idx + PACKET_COUNT)) {
      return false;
    }
  }
  return NetRecvFromClient(Channel, GPacketBuf, NET_MAX_PACKET_BYTES) == 0 &&
         NetRecvFromServer(Channel, GPacketBuf, NET_MAX_PACKET_BYTES) == 0;
}

// Keeps one frame queued while cycling far more bytes than the ring holds, so
// frames (and their length headers) straddle the end of the ring.
static bool LoopbackWrapAround(NetLoopback *Channel) {
  FillPattern(GPacketBuf, WRAP_PACKET_BYTES, 0);
  if (!NetSendToClient(Channel, GPacketBuf, WRAP_PACKET_BYTES)) {
    return false;
  }
  for (int Iter = 1; Iter <= WRAP_ITERATIONS; Iter++) {
    FillPattern(GPacketBuf, WRAP_PACKET_BYTES, Iter);
    if (!NetSendToClient(Channel, GPacketBuf, WRAP_PACKET_BYTES)) {
      return false;
    }
    int Got = NetRecvFromServer(Channel, GPacketBuf, NET_MAX_PACKET_BYTES);
    if (Got != WRAP_PACKET_BYTES || !MatchesPattern(GPacketBuf, Got, Iter - 1)) {
      return false;
    }
  }
  int Last = NetRecvFromServer(Channel, GPacketBuf, NET_MAX_PACKET_BYTES);
  return Last == WRAP_PACKET_BYTES &&
         MatchesPattern(GPacketBuf, Last, WRAP_ITERATIONS) &&
         NetRecvFromServer(Channel, GPacketBuf, NET_MAX_PACKET_BYTES) == 0;
}

static bool LoopbackFullRing(NetLoopback *Channel) {
  int Accepted = 0;
  for (;;) {
    FillPattern(GPacketBuf, NET_MAX_PACKET_BYTES, Accepted);
    if (!NetSendToServer(Channel, GPacketBuf, NET_MAX_PACKET_BYTES)) {
      break;
    }
    Accepted++;
  }
  int Expected = NET_LOOPBACK_RING_BYTES / (NET_MAX_PACKET_BYTES + 2);
  if (Accepted != Expected) {
    return false;
  }
  for (int Idx = 0; Idx < Accepted; Idx++) {
    int Got = NetRecvFromClient(Channel, GPacketBuf, NET_MAX_PACKET_BYTES);
    if (Got != NET_MAX_PACKET_BYTES || !MatchesPattern(GPacketBuf, Got, Idx)) {
      return false;
    }
  }
  return NetRecvFromClient(Channel, GPacketBuf, NET_MAX_PACKET_BYTES) == 0;
}

static bool LoopbackSizeLimits(NetLoopback *Channel) {
  return !NetSendToServer(Channel, GPacketBuf, 0) &&
         !NetSendToServer(Channel, GPacketBuf, NET_MAX_PACKET_BYTES + 1) &&
         !NetSendToClient(Channel, (const uint8_t *)0, 1) &&
         NetRecvFromClient(Channel, GPacketBuf, NET_MAX_PACKET_BYTES) == 0;
}

static bool LoopbackUndersizedRecv(NetLoopback *Channel) {
  FillPattern(GPacketBuf, SMALL_PACKET_BYTES, 1);
  if (!NetSendToClient(Channel, GPacketBuf, SMALL_PACKET_BYTES)) {
    return false;
  }
  memset(GPacketBuf, 0, SMALL_PACKET_BYTES);
  if (NetRecvFromServer(Channel, GPacketBuf, SMALL_RECV_BYTES) != -1) {
    return false;
  }
  int Got = NetRecvFromServer(Channel, GPacketBuf, SMALL_PACKET_BYTES);
  return Got == SMALL_PACKET_BYTES && MatchesPattern(GPacketBuf, Got, 1);
}

static bool TestLoopback(void) {
  NetLoopback *Channel = NetLoopbackCreate();
  if (!Expect(Channel != (NetLoopback *)0, "loopback_create")) {
    return false;
  }
  bool Ok = Expect(LoopbackFifo(Channel), "loopback_fifo_both_directions") &&
            Expect(LoopbackWrapAround(Channel), "loopback_ring_wrap_around") &&
            Expect(LoopbackFullRing(Channel), "loopback_full_ring_rejects") &&
            Expect(LoopbackSizeLimits(Channel), "loopback_size_limits") &&
            Expect(LoopbackUndersizedRecv(Channel),
                   "loopback_undersized_receive_keeps_frame");
  NetLoopbackDestroy(Channel);
  return Ok;
}

bool RunProtocolCodecSelfTest(void) {
  return TestPeek() && TestClientInput() && TestClientInputHotbarRequest() &&
         TestPlayerState() &&
         TestBlockDelta() && TestChunkData() && TestLoopback();
}

enum {
  MAX_INBOX_DELTAS = 64,
  LANDING_TICK_LIMIT = 200,
  WALK_TICKS = 10,
  TRUNCATED_INPUT_BYTES = 5,
  EXPECTED_DROPPED_PACKETS = 2,
  OVERFLOW_LAYERS = 2,
};

static const float MIN_WALK_DISTANCE_SQ = 0.25F;
static const Vec3 LOOK_EAST = {1.0F, 0.0F, 0.0F};
static const Vec3 LOOK_DOWN = {0.0F, -1.0F, 0.0F};

// What the test client has received from the server since the last drain.
typedef struct {
  PktServerPlayerState LastState;
  int StateCount;
  PktServerBlockDelta Deltas[MAX_INBOX_DELTAS];
  int DeltaCount;
  int ChunkCount;
  int WantChunkX;
  int WantChunkY;
  int WantChunkZ;
  Chunk *ClientChunk;
  bool ChunkDecoded;
  int InvalidCount;
} ClientInbox;

static void DrainServerPackets(NetLoopback *Channel, ClientInbox *Inbox) {
  for (;;) {
    int Size = NetRecvFromServer(Channel, GPacketBuf, NET_MAX_PACKET_BYTES);
    if (Size <= 0) {
      return;
    }
    switch (NetPeekPacketType(GPacketBuf, Size)) {
    case PKT_SERVER_PLAYER_STATE:
      if (NetDecodeServerPlayerState(&Inbox->LastState, GPacketBuf, Size)) {
        Inbox->StateCount++;
      } else {
        Inbox->InvalidCount++;
      }
      break;
    case PKT_SERVER_BLOCK_DELTA: {
      PktServerBlockDelta Delta = {0};
      if (NetDecodeBlockDelta(&Delta, GPacketBuf, Size) &&
          Inbox->DeltaCount < MAX_INBOX_DELTAS) {
        Inbox->Deltas[Inbox->DeltaCount++] = Delta;
      } else {
        Inbox->InvalidCount++;
      }
      break;
    }
    case PKT_SERVER_CHUNK_DATA: {
      PktServerChunkData Header = {0};
      if (!NetDecodeChunkHeader(&Header, GPacketBuf, Size)) {
        Inbox->InvalidCount++;
        break;
      }
      Inbox->ChunkCount++;
      if (Inbox->ClientChunk != (Chunk *)0 &&
          Header.ChunkX == Inbox->WantChunkX &&
          Header.ChunkY == Inbox->WantChunkY &&
          Header.ChunkZ == Inbox->WantChunkZ) {
        Inbox->ChunkDecoded =
            NetDecodeChunkData(Inbox->ClientChunk, GPacketBuf, Size);
      }
      break;
    }
    default:
      Inbox->InvalidCount++;
      break;
    }
  }
}

static void ResetInbox(ClientInbox *Inbox) {
  Chunk *ClientChunk = Inbox->ClientChunk;
  *Inbox = (ClientInbox){0};
  Inbox->ClientChunk = ClientChunk;
}

static bool SendInput(NetLoopback *Channel, uint64_t Tick, PlayerInput Input,
                      Vec3 Forward) {
  PktClientInput Pkt = {.ClientTick = Tick, .Input = Input, .Forward = Forward};
  int Size = NetEncodeClientInput(&Pkt, GPacketBuf, NET_MAX_PACKET_BYTES);
  return Size > 0 && NetSendToServer(Channel, GPacketBuf, Size);
}

static PlayerInput IdleInput(void) {
  PlayerInput Input = {0};
  Input.HotbarSelect = -1;
  return Input;
}

static bool StateMatchesServer(const ClientInbox *Inbox,
                               const ServerState *Server) {
  const PktServerPlayerState *State = &Inbox->LastState;
  return Inbox->StateCount > 0 && State->ServerTick == Server->TickCount &&
         Vec3Equal(State->Position, Server->PlayerVal.Position) &&
         Vec3Equal(State->Velocity, Server->PlayerVal.Velocity) &&
         State->IsGrounded == Server->PlayerVal.IsGrounded &&
         State->Noclip == Server->PlayerVal.Noclip &&
         State->SelectedHotbarSlot == Server->PlayerVal.SelectedHotbarSlot;
}

static int BlockToChunk(int BlockCoord) {
  return (BlockCoord >= 0) ? (BlockCoord / CHUNK_SIZE)
                           : -((CHUNK_SIZE - 1 - BlockCoord) / CHUNK_SIZE);
}

static bool ClientChunkMatchesServer(const ClientInbox *Inbox,
                                     ServerState *Server) {
  const Chunk *ServerChunk =
      GetChunkFromWorld(Server->WorldVal, Inbox->WantChunkX, Inbox->WantChunkY,
                        Inbox->WantChunkZ);
  const Chunk *ClientChunk = Inbox->ClientChunk;
  return ServerChunk != (const Chunk *)0 && Inbox->ChunkDecoded &&
         memcmp(ClientChunk->Data, ServerChunk->Data,
                sizeof(ServerChunk->Data)) == 0 &&
         memcmp(ClientChunk->BiomeMap, ServerChunk->BiomeMap,
                sizeof(ServerChunk->BiomeMap)) == 0 &&
         ClientChunk->SolidBlockCount == CountSolid(ServerChunk);
}

static bool TestServerLanding(ServerState *Server, NetLoopback *Channel,
                              ClientInbox *Inbox) {
  int Ticks = 0;
  while (Ticks < LANDING_TICK_LIMIT && !Server->PlayerVal.IsGrounded) {
    ServerStepTick(Server);
    Ticks++;
  }
  ServerStepTick(Server);
  Ticks++;
  DrainServerPackets(Channel, Inbox);
  return Expect(Server->PlayerVal.IsGrounded && Inbox->StateCount == Ticks &&
                    Inbox->InvalidCount == 0 &&
                    StateMatchesServer(Inbox, Server),
                "server_state_every_tick_until_landing");
}

static bool TestServerWalk(ServerState *Server, NetLoopback *Channel,
                           ClientInbox *Inbox, uint64_t *NextTick) {
  Vec3 Start = Server->PlayerVal.Position;
  PlayerInput Walk = IdleInput();
  Walk.MoveForward = 1.0F;
  bool Sent = true;
  for (int Idx = 0; Idx < WALK_TICKS; Idx++) {
    Sent = Sent && SendInput(Channel, (*NextTick)++, Walk, LOOK_EAST);
    ServerStepTick(Server);
  }
  Sent = Sent && SendInput(Channel, *NextTick, IdleInput(), LOOK_EAST);
  ServerStepTick(Server);
  ResetInbox(Inbox);
  DrainServerPackets(Channel, Inbox);

  float DeltaX = Server->PlayerVal.Position.x - Start.x;
  float DeltaZ = Server->PlayerVal.Position.z - Start.z;
  float DistanceSq = (DeltaX * DeltaX) + (DeltaZ * DeltaZ);
  bool Ok = Sent && DistanceSq >= MIN_WALK_DISTANCE_SQ &&
            Inbox->LastState.LastProcessedClientTick == *NextTick &&
            StateMatchesServer(Inbox, Server);
  (*NextTick)++;
  return Expect(Ok, "server_walk_input_and_last_processed_tick");
}

static bool TestServerMalformedInput(ServerState *Server, NetLoopback *Channel,
                                     ClientInbox *Inbox, uint64_t *NextTick) {
  uint64_t DroppedBefore = Server->DroppedClientPackets;
  PktClientInput Pkt = {
      .ClientTick = *NextTick, .Input = IdleInput(), .Forward = LOOK_EAST};
  int Size = NetEncodeClientInput(&Pkt, GPacketBuf, NET_MAX_PACKET_BYTES);
  int DeltaSize = NetEncodeBlockDelta(&SAMPLE_DELTA, GScratchBuf,
                                      NET_MAX_PACKET_BYTES);
  bool Sent = Size > 0 && DeltaSize > 0 &&
              NetSendToServer(Channel, GPacketBuf, TRUNCATED_INPUT_BYTES) &&
              NetSendToServer(Channel, GScratchBuf, DeltaSize) &&
              NetSendToServer(Channel, GPacketBuf, Size);
  ServerStepTick(Server);
  ResetInbox(Inbox);
  DrainServerPackets(Channel, Inbox);
  bool Ok = Sent &&
            Server->DroppedClientPackets - DroppedBefore ==
                EXPECTED_DROPPED_PACKETS &&
            Inbox->LastState.LastProcessedClientTick == *NextTick;
  (*NextTick)++;
  return Expect(Ok, "server_drops_malformed_and_unexpected_packets");
}

static int FindPlaceableSlot(const Player *PlayerVal) {
  for (int Slot = 0; Slot < HOTBAR_SIZE; Slot++) {
    if (PlayerVal->Hotbar[Slot] != 0U) {
      return Slot;
    }
  }
  return -1;
}

static bool OnlyDeltaIs(const ClientInbox *Inbox, Vec3 BlockPos,
                        BlockID BlockId) {
  if (Inbox->DeltaCount != 1) {
    return false;
  }
  const PktServerBlockDelta *Delta = &Inbox->Deltas[0];
  return Delta->BlockX == (int)floorf(BlockPos.x) &&
         Delta->BlockY == (int)floorf(BlockPos.y) &&
         Delta->BlockZ == (int)floorf(BlockPos.z) && Delta->BlockId == BlockId;
}

// Aims straight down from a hovering (noclip) player so the break and the
// following place resolve to the same block, whatever the terrain under spawn.
static bool TestServerBreakAndPlace(ServerState *Server, NetLoopback *Channel,
                                    ClientInbox *Inbox, uint64_t *NextTick,
                                    Vec3 *OutBlockPos) {
  int Slot = FindPlaceableSlot(&Server->PlayerVal);
  if (!Expect(Slot >= 0, "server_hotbar_has_placeable_block")) {
    return false;
  }
  Server->PlayerVal.Noclip = true;

  PlayerInput Aim = IdleInput();
  Aim.HotbarSelect = Slot;
  bool Sent = SendInput(Channel, (*NextTick)++, Aim, LOOK_DOWN);
  ServerStepTick(Server);
  ResetInbox(Inbox);
  DrainServerPackets(Channel, Inbox);

  RaycastResult Target = Server->PlayerVal.TargetBlock;
  Vec3 BlockPos = Target.BlockPos;
  int IdBefore = GetBlockIDFromWorld(Server->WorldVal, BlockPos);
  LogInfo("PROTOCOL_TEST: target block (%d, %d, %d) id=%d",
          (int)floorf(BlockPos.x), (int)floorf(BlockPos.y),
          (int)floorf(BlockPos.z), IdBefore);
  if (!Expect(Sent && Target.Hit && IdBefore != 0 && Inbox->DeltaCount == 0 &&
                  Inbox->LastState.SelectedHotbarSlot == Slot,
              "server_hotbar_select_and_target_below_player")) {
    return false;
  }

  PlayerInput Break = IdleInput();
  Break.Break = true;
  Sent = SendInput(Channel, (*NextTick)++, Break, LOOK_DOWN);
  ServerStepTick(Server);
  ResetInbox(Inbox);
  DrainServerPackets(Channel, Inbox);
  if (!Expect(Sent && OnlyDeltaIs(Inbox, BlockPos, 0U) &&
                  GetBlockIDFromWorld(Server->WorldVal, BlockPos) == 0,
              "server_break_emits_block_delta")) {
    return false;
  }

  BlockID HandId = Server->PlayerVal.Hotbar[Slot];
  PlayerInput Place = IdleInput();
  Place.Place = true;
  Sent = SendInput(Channel, (*NextTick)++, Place, LOOK_DOWN);
  ServerStepTick(Server);
  ResetInbox(Inbox);
  DrainServerPackets(Channel, Inbox);
  *OutBlockPos = BlockPos;
  return Expect(Sent && OnlyDeltaIs(Inbox, BlockPos, HandId) &&
                    GetBlockIDFromWorld(Server->WorldVal, BlockPos) ==
                        (int)HandId,
                "server_place_emits_block_delta");
}

static bool TestServerChunkReplication(ServerState *Server,
                                       NetLoopback *Channel, ClientInbox *Inbox,
                                       Vec3 EditedBlock) {
  ResetInbox(Inbox);
  Inbox->WantChunkX = BlockToChunk((int)floorf(EditedBlock.x));
  Inbox->WantChunkY = BlockToChunk((int)floorf(EditedBlock.y));
  Inbox->WantChunkZ = BlockToChunk((int)floorf(EditedBlock.z));
  bool Sent = ServerSendChunkToClient(Server, Inbox->WantChunkX,
                                      Inbox->WantChunkY, Inbox->WantChunkZ);
  DrainServerPackets(Channel, Inbox);
  return Expect(Sent && Inbox->ChunkCount == 1 &&
                    ClientChunkMatchesServer(Inbox, Server),
                "server_chunk_replication_after_edit");
}

// Edits more blocks in one chunk than fit in a tick's delta list; the client
// must then receive the whole chunk instead of any delta for it.
static bool TestServerDeltaOverflow(ServerState *Server, NetLoopback *Channel,
                                    ClientInbox *Inbox, BlockID FillId) {
  int BaseX = Inbox->WantChunkX * CHUNK_SIZE;
  int BaseY = Inbox->WantChunkY * CHUNK_SIZE;
  int BaseZ = Inbox->WantChunkZ * CHUNK_SIZE;
  int Changed = 0;
  for (int Y = 0; Y < OVERFLOW_LAYERS; Y++) {
    for (int X = 0; X < CHUNK_SIZE; X++) {
      for (int Z = 0; Z < CHUNK_SIZE; Z++) {
        Vec3 Pos = {(float)(BaseX + X), (float)(BaseY + Y), (float)(BaseZ + Z)};
        int Current = GetBlockIDFromWorld(Server->WorldVal, Pos);
        BlockID NewId = (Current == (int)FillId) ? BLOCK_ID_AIR : FillId;
        SetBlockInWorld(Server->WorldVal, Pos, NewId);
        Changed++;
      }
    }
  }

  ServerStepTick(Server);
  int ChunkX = Inbox->WantChunkX;
  int ChunkY = Inbox->WantChunkY;
  int ChunkZ = Inbox->WantChunkZ;
  ResetInbox(Inbox);
  Inbox->WantChunkX = ChunkX;
  Inbox->WantChunkY = ChunkY;
  Inbox->WantChunkZ = ChunkZ;
  DrainServerPackets(Channel, Inbox);
  return Expect(Changed > SERVER_MAX_DELTAS_PER_TICK && Inbox->DeltaCount == 0 &&
                    Inbox->ChunkCount == 1 &&
                    ClientChunkMatchesServer(Inbox, Server) &&
                    Server->PendingResendCount == 0,
                "server_delta_overflow_resends_chunk");
}

// A requested hotbar reaches the server through the input packet and is
// validated per slot: a loaded block is kept, an id no block uses becomes air.
static bool TestServerHotbarRequest(ServerState *Server, NetLoopback *Channel,
                                    uint64_t *NextTick) {
  const BlockID UNKNOWN_BLOCK_ID = 4000;
  PlayerInput Request = IdleInput();
  Request.HotbarSetRequested = true;
  Request.HotbarSet[0] = UNKNOWN_BLOCK_ID;
  Request.HotbarSet[1] = (BlockID)STONE_BLOCK_ID;
  bool Sent = SendInput(Channel, (*NextTick)++, Request, LOOK_EAST);
  ServerStepTick(Server);

  const BlockID *Hotbar = Server->PlayerVal.Hotbar;
  bool Ok = Sent && Hotbar[0] == BLOCK_ID_AIR &&
            Hotbar[1] == (BlockID)STONE_BLOCK_ID &&
            !Server->PendingInput.HotbarSetRequested;
  return Expect(Ok, "server_applies_and_validates_hotbar_request");
}

bool RunProtocolServerSelfTest(ServerState *Server) {
  NetLoopback *Channel = NetLoopbackCreate();
  Chunk *ClientChunk = (Chunk *)calloc(1, sizeof(Chunk));
  if (!Expect(Channel != (NetLoopback *)0 && ClientChunk != (Chunk *)0,
              "server_test_allocation")) {
    NetLoopbackDestroy(Channel);
    free(ClientChunk);
    return false;
  }

  ServerBindLoopback(Server, Channel);
  ClientInbox Inbox = {0};
  Inbox.ClientChunk = ClientChunk;
  uint64_t NextTick = 1;
  Vec3 EditedBlock = {0};

  bool Ok = TestServerLanding(Server, Channel, &Inbox) &&
            TestServerWalk(Server, Channel, &Inbox, &NextTick) &&
            TestServerMalformedInput(Server, Channel, &Inbox, &NextTick) &&
            TestServerBreakAndPlace(Server, Channel, &Inbox, &NextTick,
                                    &EditedBlock) &&
            TestServerChunkReplication(Server, Channel, &Inbox, EditedBlock) &&
            TestServerDeltaOverflow(
                Server, Channel, &Inbox,
                Server->PlayerVal.Hotbar[Server->PlayerVal.SelectedHotbarSlot]) &&
            TestServerHotbarRequest(Server, Channel, &NextTick);

  ServerBindLoopback(Server, (NetLoopback *)0);
  NetLoopbackDestroy(Channel);
  free(ClientChunk);
  return Ok;
}
