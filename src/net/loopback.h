#ifndef NET_LOOPBACK_H
#define NET_LOOPBACK_H

#include <stdbool.h>
#include <stdint.h>

// In-memory client<->server transport. Each direction is a FIFO byte ring of
// length-framed packets, so every message crosses the same encode/decode path
// a socket backend will use. Not thread-safe: both endpoints must run on the
// same thread.

enum { NET_LOOPBACK_RING_BYTES = 256 * 1024 };

typedef struct NetLoopback NetLoopback;

// Returns NULL on allocation failure.
NetLoopback *NetLoopbackCreate(void);
void NetLoopbackDestroy(NetLoopback *Channel);

// Send functions enqueue one packet and return false, writing nothing, when
// Size is outside 1..NET_MAX_PACKET_BYTES or the ring lacks room for it.
bool NetSendToServer(NetLoopback *Channel, const uint8_t *Data, int Size);
bool NetSendToClient(NetLoopback *Channel, const uint8_t *Data, int Size);

// Receive functions dequeue one packet into OutBuf and return its length, 0
// when the queue is empty, or -1 when Capacity is smaller than the next packet
// (which then stays queued).
int NetRecvFromClient(NetLoopback *Channel, uint8_t *OutBuf, int Capacity);
int NetRecvFromServer(NetLoopback *Channel, uint8_t *OutBuf, int Capacity);

#endif
