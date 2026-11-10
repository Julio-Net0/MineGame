#include "net/loopback.h"

#include "net/protocol.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

enum {
  FRAME_HEADER_BYTES = 2,
  BITS_PER_BYTE = 8,
  BYTE_MASK = 0xFF,
};

_Static_assert(NET_MAX_PACKET_BYTES <= UINT16_MAX,
               "packet length must fit the u16 frame header");
_Static_assert(NET_MAX_PACKET_BYTES + FRAME_HEADER_BYTES <=
                   NET_LOOPBACK_RING_BYTES,
               "ring must hold at least one maximum-size frame");

// Head is the read offset; Used counts queued bytes, so an empty ring and a
// full ring are distinguishable without a spare slot.
typedef struct {
  uint8_t Bytes[NET_LOOPBACK_RING_BYTES];
  int Head;
  int Used;
} NetRing;

struct NetLoopback {
  NetRing ClientToServer;
  NetRing ServerToClient;
};

static void RingCopyIn(NetRing *Ring, int Offset, const uint8_t *Src,
                       int Count) {
  int Start = Offset % NET_LOOPBACK_RING_BYTES;
  int FirstPart = NET_LOOPBACK_RING_BYTES - Start;
  if (FirstPart > Count) {
    FirstPart = Count;
  }
  memcpy(Ring->Bytes + Start, Src, (size_t)FirstPart);
  if (Count > FirstPart) {
    memcpy(Ring->Bytes, Src + FirstPart, (size_t)(Count - FirstPart));
  }
}

static void RingCopyOut(const NetRing *Ring, int Offset, uint8_t *Dst,
                        int Count) {
  int Start = Offset % NET_LOOPBACK_RING_BYTES;
  int FirstPart = NET_LOOPBACK_RING_BYTES - Start;
  if (FirstPart > Count) {
    FirstPart = Count;
  }
  memcpy(Dst, Ring->Bytes + Start, (size_t)FirstPart);
  if (Count > FirstPart) {
    memcpy(Dst + FirstPart, Ring->Bytes, (size_t)(Count - FirstPart));
  }
}

static bool RingPush(NetRing *Ring, const uint8_t *Data, int Size) {
  if (Data == (const uint8_t *)0 || Size < 1 || Size > NET_MAX_PACKET_BYTES) {
    return false;
  }
  int FrameBytes = FRAME_HEADER_BYTES + Size;
  if (Ring->Used > NET_LOOPBACK_RING_BYTES - FrameBytes) {
    return false;
  }

  uint8_t Header[FRAME_HEADER_BYTES] = {
      (uint8_t)((unsigned)Size & BYTE_MASK),
      (uint8_t)(((unsigned)Size >> BITS_PER_BYTE) & BYTE_MASK),
  };
  int Tail = Ring->Head + Ring->Used;
  RingCopyIn(Ring, Tail, Header, FRAME_HEADER_BYTES);
  RingCopyIn(Ring, Tail + FRAME_HEADER_BYTES, Data, Size);
  Ring->Used += FrameBytes;
  return true;
}

static int RingPop(NetRing *Ring, uint8_t *OutBuf, int Capacity) {
  if (Ring->Used == 0) {
    return 0;
  }

  uint8_t Header[FRAME_HEADER_BYTES] = {0};
  RingCopyOut(Ring, Ring->Head, Header, FRAME_HEADER_BYTES);
  int Size = (int)((unsigned)Header[0] | ((unsigned)Header[1] << BITS_PER_BYTE));
  if (OutBuf == (uint8_t *)0 || Capacity < Size) {
    return -1;
  }

  RingCopyOut(Ring, Ring->Head + FRAME_HEADER_BYTES, OutBuf, Size);
  Ring->Head = (Ring->Head + FRAME_HEADER_BYTES + Size) % NET_LOOPBACK_RING_BYTES;
  Ring->Used -= FRAME_HEADER_BYTES + Size;
  return Size;
}

NetLoopback *NetLoopbackCreate(void) {
  NetLoopback *Channel = (NetLoopback *)calloc(1, sizeof(NetLoopback));
  return Channel;
}

void NetLoopbackDestroy(NetLoopback *Channel) { free(Channel); }

bool NetSendToServer(NetLoopback *Channel, const uint8_t *Data, int Size) {
  return Channel != (NetLoopback *)0 &&
         RingPush(&Channel->ClientToServer, Data, Size);
}

bool NetSendToClient(NetLoopback *Channel, const uint8_t *Data, int Size) {
  return Channel != (NetLoopback *)0 &&
         RingPush(&Channel->ServerToClient, Data, Size);
}

int NetRecvFromClient(NetLoopback *Channel, uint8_t *OutBuf, int Capacity) {
  if (Channel == (NetLoopback *)0) {
    return 0;
  }
  return RingPop(&Channel->ClientToServer, OutBuf, Capacity);
}

int NetRecvFromServer(NetLoopback *Channel, uint8_t *OutBuf, int Capacity) {
  if (Channel == (NetLoopback *)0) {
    return 0;
  }
  return RingPop(&Channel->ServerToClient, OutBuf, Capacity);
}
