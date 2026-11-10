#include "net/protocol.h"

#include "persistence/chunk_serial.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

enum {
  BITS_PER_BYTE = 8,
  BYTE_MASK = 0xFF,
  U8_BYTES = 1,
  U16_BYTES = 2,
  U32_BYTES = 4,
  U64_BYTES = 8,
  INT8_MAX_VALUE = 127,
  INT8_MIN_VALUE = -128,
};

static const float ROUND_HALF = 0.5F;

enum {
  INPUT_FLAG_JUMP = 1 << 0,
  INPUT_FLAG_ASCEND = 1 << 1,
  INPUT_FLAG_DESCEND = 1 << 2,
  INPUT_FLAG_BREAK = 1 << 3,
  INPUT_FLAG_PLACE = 1 << 4,
  INPUT_FLAG_HOTBAR_SET = 1 << 5,
  INPUT_FLAG_MASK = INPUT_FLAG_JUMP | INPUT_FLAG_ASCEND | INPUT_FLAG_DESCEND |
                    INPUT_FLAG_BREAK | INPUT_FLAG_PLACE | INPUT_FLAG_HOTBAR_SET,
};

enum {
  STATE_FLAG_GROUNDED = 1 << 0,
  STATE_FLAG_NOCLIP = 1 << 1,
  STATE_FLAG_MASK = STATE_FLAG_GROUNDED | STATE_FLAG_NOCLIP,
};

_Static_assert(sizeof(((Chunk *)0)->BiomeMap) == (size_t)NET_BIOME_MAP_BYTES,
               "BiomeMap size must match its wire size");
_Static_assert(CHUNK_SERIAL_MAX_BYTES <= UINT16_MAX,
               "chunk payload size must fit the u16 DataSize field");

typedef struct {
  uint8_t *Buf;
  int Capacity;
  int Pos;
  bool Ok;
} NetWriter;

typedef struct {
  const uint8_t *Buf;
  int Size;
  int Pos;
  bool Ok;
} NetReader;

static NetWriter MakeWriter(uint8_t *Buf, int Capacity) {
  return (NetWriter){.Buf = Buf, .Capacity = Capacity, .Pos = 0, .Ok = true};
}

static NetReader MakeReader(const uint8_t *Buf, int Size) {
  return (NetReader){.Buf = Buf, .Size = Size, .Pos = 0, .Ok = true};
}

static void WriteBytesLe(NetWriter *Writer, uint64_t Value, int ByteCount) {
  if (!Writer->Ok || Writer->Pos > Writer->Capacity - ByteCount) {
    Writer->Ok = false;
    return;
  }
  for (int Idx = 0; Idx < ByteCount; Idx++) {
    unsigned Shift = (unsigned)(Idx * BITS_PER_BYTE);
    Writer->Buf[Writer->Pos + Idx] = (uint8_t)((Value >> Shift) & BYTE_MASK);
  }
  Writer->Pos += ByteCount;
}

static void WriteU8(NetWriter *Writer, uint8_t Value) {
  WriteBytesLe(Writer, Value, U8_BYTES);
}

static void WriteI8(NetWriter *Writer, int8_t Value) {
  WriteBytesLe(Writer, (uint8_t)Value, U8_BYTES);
}

static void WriteU16Le(NetWriter *Writer, uint16_t Value) {
  WriteBytesLe(Writer, Value, U16_BYTES);
}

static void WriteI32Le(NetWriter *Writer, int32_t Value) {
  WriteBytesLe(Writer, (uint32_t)Value, U32_BYTES);
}

static void WriteU64Le(NetWriter *Writer, uint64_t Value) {
  WriteBytesLe(Writer, Value, U64_BYTES);
}

static void WriteF32Le(NetWriter *Writer, float Value) {
  uint32_t Bits = 0;
  memcpy(&Bits, &Value, sizeof(Bits));
  WriteBytesLe(Writer, Bits, U32_BYTES);
}

static void WriteVec3(NetWriter *Writer, Vec3 Value) {
  WriteF32Le(Writer, Value.x);
  WriteF32Le(Writer, Value.y);
  WriteF32Le(Writer, Value.z);
}

static void WriteRaw(NetWriter *Writer, const uint8_t *Src, int Count) {
  if (!Writer->Ok || Count < 0 || Writer->Pos > Writer->Capacity - Count) {
    Writer->Ok = false;
    return;
  }
  memcpy(Writer->Buf + Writer->Pos, Src, (size_t)Count);
  Writer->Pos += Count;
}

static uint64_t ReadBytesLe(NetReader *Reader, int ByteCount) {
  if (!Reader->Ok || Reader->Pos > Reader->Size - ByteCount) {
    Reader->Ok = false;
    return 0;
  }
  uint64_t Value = 0;
  for (int Idx = 0; Idx < ByteCount; Idx++) {
    unsigned Shift = (unsigned)(Idx * BITS_PER_BYTE);
    Value |= (uint64_t)Reader->Buf[Reader->Pos + Idx] << Shift;
  }
  Reader->Pos += ByteCount;
  return Value;
}

static uint8_t ReadU8(NetReader *Reader) {
  return (uint8_t)ReadBytesLe(Reader, U8_BYTES);
}

static int8_t ReadI8(NetReader *Reader) {
  return (int8_t)(uint8_t)ReadBytesLe(Reader, U8_BYTES);
}

static uint16_t ReadU16Le(NetReader *Reader) {
  return (uint16_t)ReadBytesLe(Reader, U16_BYTES);
}

static int32_t ReadI32Le(NetReader *Reader) {
  return (int32_t)(uint32_t)ReadBytesLe(Reader, U32_BYTES);
}

static uint64_t ReadU64Le(NetReader *Reader) {
  return ReadBytesLe(Reader, U64_BYTES);
}

static float ReadF32Le(NetReader *Reader) {
  uint32_t Bits = (uint32_t)ReadBytesLe(Reader, U32_BYTES);
  float Value = 0.0F;
  memcpy(&Value, &Bits, sizeof(Value));
  return Value;
}

static Vec3 ReadVec3(NetReader *Reader) {
  Vec3 Value = {0};
  Value.x = ReadF32Le(Reader);
  Value.y = ReadF32Le(Reader);
  Value.z = ReadF32Le(Reader);
  return Value;
}

// Returns a pointer to Count bytes inside the buffer and advances past them,
// or NULL when fewer than Count bytes remain.
static const uint8_t *ReadRaw(NetReader *Reader, int Count) {
  if (!Reader->Ok || Count < 0 || Reader->Pos > Reader->Size - Count) {
    Reader->Ok = false;
    return (const uint8_t *)0;
  }
  const uint8_t *Start = Reader->Buf + Reader->Pos;
  Reader->Pos += Count;
  return Start;
}

static bool IsVec3Finite(Vec3 Value) {
  return isfinite(Value.x) && isfinite(Value.y) && isfinite(Value.z);
}

static int8_t ClampToInt8(int Value) {
  if (Value > INT8_MAX_VALUE) {
    return INT8_MAX_VALUE;
  }
  if (Value < INT8_MIN_VALUE) {
    return INT8_MIN_VALUE;
  }
  return (int8_t)Value;
}

static int8_t QuantizeAxis(float Value) {
  if (!isfinite(Value)) {
    return 0;
  }
  float Clamped = Value;
  if (Clamped > 1.0F) {
    Clamped = 1.0F;
  } else if (Clamped < -1.0F) {
    Clamped = -1.0F;
  }
  float Scaled = Clamped * NET_AXIS_QUANT_SCALE;
  float Rounded = (Scaled >= 0.0F) ? (Scaled + ROUND_HALF) : (Scaled - ROUND_HALF);
  return (int8_t)Rounded;
}

static float DequantizeAxis(int8_t Value) {
  return (float)Value / NET_AXIS_QUANT_SCALE;
}

static bool IsValidHotbarSelect(int Value) {
  return Value >= -1 && Value < HOTBAR_SIZE;
}

PacketType NetPeekPacketType(const uint8_t *Buf, int Size) {
  if (Buf == (const uint8_t *)0 || Size < 1) {
    return PKT_INVALID;
  }
  switch (Buf[0]) {
  case PKT_CLIENT_INPUT:
    return PKT_CLIENT_INPUT;
  case PKT_SERVER_PLAYER_STATE:
    return PKT_SERVER_PLAYER_STATE;
  case PKT_SERVER_CHUNK_DATA:
    return PKT_SERVER_CHUNK_DATA;
  case PKT_SERVER_BLOCK_DELTA:
    return PKT_SERVER_BLOCK_DELTA;
  default:
    return PKT_INVALID;
  }
}

static bool HasExactPacket(const uint8_t *Buf, int Size, PacketType Type,
                           int ExpectedSize) {
  return Size == ExpectedSize && NetPeekPacketType(Buf, Size) == Type;
}

int NetEncodeClientInput(const PktClientInput *Pkt, uint8_t *OutBuf,
                         int Capacity) {
  if (Pkt == (const PktClientInput *)0 || OutBuf == (uint8_t *)0) {
    return 0;
  }
  const PlayerInput *Input = &Pkt->Input;
  int PacketSize = NET_CLIENT_INPUT_BYTES +
                   (Input->HotbarSetRequested ? NET_HOTBAR_TAIL_BYTES : 0);
  if (Capacity < PacketSize) {
    return 0;
  }

  unsigned Flags = 0U;
  Flags |= Input->Jump ? (unsigned)INPUT_FLAG_JUMP : 0U;
  Flags |= Input->AscendHeld ? (unsigned)INPUT_FLAG_ASCEND : 0U;
  Flags |= Input->DescendHeld ? (unsigned)INPUT_FLAG_DESCEND : 0U;
  Flags |= Input->Break ? (unsigned)INPUT_FLAG_BREAK : 0U;
  Flags |= Input->Place ? (unsigned)INPUT_FLAG_PLACE : 0U;
  Flags |= Input->HotbarSetRequested ? (unsigned)INPUT_FLAG_HOTBAR_SET : 0U;

  int HotbarSelect =
      IsValidHotbarSelect(Input->HotbarSelect) ? Input->HotbarSelect : -1;

  NetWriter Writer = MakeWriter(OutBuf, Capacity);
  WriteU8(&Writer, (uint8_t)PKT_CLIENT_INPUT);
  WriteU64Le(&Writer, Pkt->ClientTick);
  WriteI8(&Writer, QuantizeAxis(Input->MoveX));
  WriteI8(&Writer, QuantizeAxis(Input->MoveForward));
  WriteU8(&Writer, (uint8_t)Flags);
  WriteI8(&Writer, (int8_t)HotbarSelect);
  WriteI8(&Writer, ClampToInt8(Input->HotbarScroll));
  WriteVec3(&Writer, Pkt->Forward);
  if (Input->HotbarSetRequested) {
    for (int Slot = 0; Slot < HOTBAR_SIZE; Slot++) {
      WriteU16Le(&Writer, Input->HotbarSet[Slot]);
    }
  }
  return Writer.Ok ? Writer.Pos : 0;
}

bool NetDecodeClientInput(PktClientInput *OutPkt, const uint8_t *Buf,
                          int Size) {
  // Two layouts share the tag; the hotbar flag decides which length is exact,
  // so it is checked once the flags byte has been read.
  bool BaseLayout =
      HasExactPacket(Buf, Size, PKT_CLIENT_INPUT, NET_CLIENT_INPUT_BYTES);
  bool TailLayout = HasExactPacket(Buf, Size, PKT_CLIENT_INPUT,
                                   NET_CLIENT_INPUT_BYTES + NET_HOTBAR_TAIL_BYTES);
  if (OutPkt == (PktClientInput *)0 || (!BaseLayout && !TailLayout)) {
    return false;
  }

  NetReader Reader = MakeReader(Buf, Size);
  (void)ReadU8(&Reader);
  uint64_t ClientTick = ReadU64Le(&Reader);
  int8_t MoveX = ReadI8(&Reader);
  int8_t MoveForward = ReadI8(&Reader);
  unsigned Flags = ReadU8(&Reader);
  int8_t HotbarSelect = ReadI8(&Reader);
  int8_t HotbarScroll = ReadI8(&Reader);
  Vec3 Forward = ReadVec3(&Reader);
  bool HotbarSetRequested = (Flags & (unsigned)INPUT_FLAG_HOTBAR_SET) != 0U;
  BlockID HotbarSet[HOTBAR_SIZE] = {0};
  if (HotbarSetRequested && TailLayout) {
    for (int Slot = 0; Slot < HOTBAR_SIZE; Slot++) {
      HotbarSet[Slot] = ReadU16Le(&Reader);
    }
  }

  // -128 has no positive counterpart, so a conforming encoder never emits it.
  if (!Reader.Ok || MoveX == INT8_MIN_VALUE || MoveForward == INT8_MIN_VALUE ||
      (Flags & ~(unsigned)INPUT_FLAG_MASK) != 0U ||
      HotbarSetRequested != TailLayout ||
      !IsValidHotbarSelect(HotbarSelect) || !IsVec3Finite(Forward)) {
    return false;
  }

  *OutPkt = (PktClientInput){
      .ClientTick = ClientTick,
      .Input =
          {
              .MoveX = DequantizeAxis(MoveX),
              .MoveForward = DequantizeAxis(MoveForward),
              .Jump = (Flags & (unsigned)INPUT_FLAG_JUMP) != 0U,
              .AscendHeld = (Flags & (unsigned)INPUT_FLAG_ASCEND) != 0U,
              .DescendHeld = (Flags & (unsigned)INPUT_FLAG_DESCEND) != 0U,
              .HotbarSelect = HotbarSelect,
              .HotbarScroll = HotbarScroll,
              .Break = (Flags & (unsigned)INPUT_FLAG_BREAK) != 0U,
              .Place = (Flags & (unsigned)INPUT_FLAG_PLACE) != 0U,
              .HotbarSetRequested = HotbarSetRequested,
          },
      .Forward = Forward,
  };
  for (int Slot = 0; Slot < HOTBAR_SIZE; Slot++) {
    OutPkt->Input.HotbarSet[Slot] = HotbarSet[Slot];
  }
  return true;
}

int NetEncodeServerPlayerState(const PktServerPlayerState *Pkt, uint8_t *OutBuf,
                               int Capacity) {
  if (Pkt == (const PktServerPlayerState *)0 || OutBuf == (uint8_t *)0 ||
      Capacity < NET_SERVER_PLAYER_STATE_BYTES) {
    return 0;
  }

  unsigned Flags = 0U;
  Flags |= Pkt->IsGrounded ? (unsigned)STATE_FLAG_GROUNDED : 0U;
  Flags |= Pkt->Noclip ? (unsigned)STATE_FLAG_NOCLIP : 0U;

  NetWriter Writer = MakeWriter(OutBuf, Capacity);
  WriteU8(&Writer, (uint8_t)PKT_SERVER_PLAYER_STATE);
  WriteU64Le(&Writer, Pkt->ServerTick);
  WriteU64Le(&Writer, Pkt->LastProcessedClientTick);
  WriteVec3(&Writer, Pkt->Position);
  WriteVec3(&Writer, Pkt->Velocity);
  WriteU8(&Writer, (uint8_t)Flags);
  WriteI8(&Writer, ClampToInt8(Pkt->SelectedHotbarSlot));
  return Writer.Ok ? Writer.Pos : 0;
}

bool NetDecodeServerPlayerState(PktServerPlayerState *OutPkt,
                                const uint8_t *Buf, int Size) {
  if (OutPkt == (PktServerPlayerState *)0 ||
      !HasExactPacket(Buf, Size, PKT_SERVER_PLAYER_STATE,
                      NET_SERVER_PLAYER_STATE_BYTES)) {
    return false;
  }

  NetReader Reader = MakeReader(Buf, Size);
  (void)ReadU8(&Reader);
  uint64_t ServerTick = ReadU64Le(&Reader);
  uint64_t LastProcessedClientTick = ReadU64Le(&Reader);
  Vec3 Position = ReadVec3(&Reader);
  Vec3 Velocity = ReadVec3(&Reader);
  unsigned Flags = ReadU8(&Reader);
  int8_t Slot = ReadI8(&Reader);

  if (!Reader.Ok || (Flags & ~(unsigned)STATE_FLAG_MASK) != 0U || Slot < 0 ||
      Slot >= HOTBAR_SIZE || !IsVec3Finite(Position) ||
      !IsVec3Finite(Velocity)) {
    return false;
  }

  *OutPkt = (PktServerPlayerState){
      .ServerTick = ServerTick,
      .LastProcessedClientTick = LastProcessedClientTick,
      .Position = Position,
      .Velocity = Velocity,
      .IsGrounded = (Flags & (unsigned)STATE_FLAG_GROUNDED) != 0U,
      .Noclip = (Flags & (unsigned)STATE_FLAG_NOCLIP) != 0U,
      .SelectedHotbarSlot = Slot,
  };
  return true;
}

int NetEncodeChunkData(const Chunk *ChunkVal, uint8_t *OutBuf, int Capacity) {
  if (ChunkVal == (const Chunk *)0 || OutBuf == (uint8_t *)0) {
    return 0;
  }

  uint8_t Payload[CHUNK_SERIAL_MAX_BYTES];
  bool IsRaw = false;
  int DataSize = ChunkSerialize(ChunkVal, Payload, &IsRaw);
  if (DataSize <= 0 || DataSize > CHUNK_SERIAL_MAX_BYTES ||
      Capacity < NET_CHUNK_HEADER_BYTES + DataSize + NET_BIOME_MAP_BYTES) {
    return 0;
  }

  NetWriter Writer = MakeWriter(OutBuf, Capacity);
  WriteU8(&Writer, (uint8_t)PKT_SERVER_CHUNK_DATA);
  WriteI32Le(&Writer, ChunkVal->ChunkX);
  WriteI32Le(&Writer, ChunkVal->ChunkY);
  WriteI32Le(&Writer, ChunkVal->ChunkZ);
  WriteU8(&Writer, IsRaw ? 1U : 0U);
  WriteU16Le(&Writer, (uint16_t)DataSize);
  WriteRaw(&Writer, Payload, DataSize);
  WriteRaw(&Writer, (const uint8_t *)ChunkVal->BiomeMap, NET_BIOME_MAP_BYTES);
  return Writer.Ok ? Writer.Pos : 0;
}

typedef struct {
  PktServerChunkData Header;
  const uint8_t *Payload;
  const uint8_t *BiomeMap;
} ParsedChunkPacket;

static bool ParseChunkPacket(const uint8_t *Buf, int Size,
                             ParsedChunkPacket *Out) {
  if (Size < NET_CHUNK_HEADER_BYTES ||
      NetPeekPacketType(Buf, Size) != PKT_SERVER_CHUNK_DATA) {
    return false;
  }

  NetReader Reader = MakeReader(Buf, Size);
  (void)ReadU8(&Reader);
  PktServerChunkData Header = {0};
  Header.ChunkX = ReadI32Le(&Reader);
  Header.ChunkY = ReadI32Le(&Reader);
  Header.ChunkZ = ReadI32Le(&Reader);
  uint8_t RawFlag = ReadU8(&Reader);
  Header.DataSize = ReadU16Le(&Reader);
  Header.IsRaw = (RawFlag == 1U);

  if (!Reader.Ok || RawFlag > 1U || Header.DataSize == 0U ||
      (int)Header.DataSize > CHUNK_SERIAL_MAX_BYTES ||
      (Header.IsRaw && (int)Header.DataSize != CHUNK_SERIAL_MAX_BYTES) ||
      Size != NET_CHUNK_HEADER_BYTES + (int)Header.DataSize +
                  NET_BIOME_MAP_BYTES) {
    return false;
  }

  const uint8_t *Payload = ReadRaw(&Reader, (int)Header.DataSize);
  const uint8_t *BiomeMap = ReadRaw(&Reader, NET_BIOME_MAP_BYTES);
  if (!Reader.Ok) {
    return false;
  }

  *Out = (ParsedChunkPacket){
      .Header = Header, .Payload = Payload, .BiomeMap = BiomeMap};
  return true;
}

bool NetDecodeChunkHeader(PktServerChunkData *OutHeader, const uint8_t *Buf,
                          int Size) {
  ParsedChunkPacket Parsed = {0};
  if (OutHeader == (PktServerChunkData *)0 ||
      !ParseChunkPacket(Buf, Size, &Parsed)) {
    return false;
  }
  *OutHeader = Parsed.Header;
  return true;
}

bool NetDecodeChunkData(Chunk *OutChunk, const uint8_t *Buf, int Size) {
  ParsedChunkPacket Parsed = {0};
  if (OutChunk == (Chunk *)0 || !ParseChunkPacket(Buf, Size, &Parsed)) {
    return false;
  }

  if (!ChunkDeserialize(OutChunk, Parsed.Payload, (int)Parsed.Header.DataSize,
                        Parsed.Header.IsRaw)) {
    return false;
  }

  OutChunk->ChunkX = Parsed.Header.ChunkX;
  OutChunk->ChunkY = Parsed.Header.ChunkY;
  OutChunk->ChunkZ = Parsed.Header.ChunkZ;
  memcpy(OutChunk->BiomeMap, Parsed.BiomeMap, NET_BIOME_MAP_BYTES);
  OutChunk->IsGenerated = true;
  OutChunk->IsDirty = true;
  return true;
}

int NetEncodeBlockDelta(const PktServerBlockDelta *Pkt, uint8_t *OutBuf,
                        int Capacity) {
  if (Pkt == (const PktServerBlockDelta *)0 || OutBuf == (uint8_t *)0 ||
      Capacity < NET_BLOCK_DELTA_BYTES) {
    return 0;
  }

  NetWriter Writer = MakeWriter(OutBuf, Capacity);
  WriteU8(&Writer, (uint8_t)PKT_SERVER_BLOCK_DELTA);
  WriteI32Le(&Writer, Pkt->BlockX);
  WriteI32Le(&Writer, Pkt->BlockY);
  WriteI32Le(&Writer, Pkt->BlockZ);
  WriteU16Le(&Writer, Pkt->BlockId);
  return Writer.Ok ? Writer.Pos : 0;
}

bool NetDecodeBlockDelta(PktServerBlockDelta *OutPkt, const uint8_t *Buf,
                         int Size) {
  if (OutPkt == (PktServerBlockDelta *)0 ||
      !HasExactPacket(Buf, Size, PKT_SERVER_BLOCK_DELTA,
                      NET_BLOCK_DELTA_BYTES)) {
    return false;
  }

  NetReader Reader = MakeReader(Buf, Size);
  (void)ReadU8(&Reader);
  PktServerBlockDelta Pkt = {0};
  Pkt.BlockX = ReadI32Le(&Reader);
  Pkt.BlockY = ReadI32Le(&Reader);
  Pkt.BlockZ = ReadI32Le(&Reader);
  Pkt.BlockId = ReadU16Le(&Reader);
  if (!Reader.Ok) {
    return false;
  }

  *OutPkt = Pkt;
  return true;
}
