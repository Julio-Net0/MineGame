#ifndef DENSE_POOL_H
#define DENSE_POOL_H

#include "world/chunk.h"
#include "world/world_limits.h"
#include <stdatomic.h>

// Buffers per slab: 512 x 8 KB = 4 MB per dense allocation, 512 x ~2 KB = 1 MB
// per packed one.
#define DENSE_SLAB_CAPACITY 512
// Enough slabs for every active chunk to hold a buffer at once, so the world
// can never exhaust the pool; only a failed slab allocation can.
#define DENSE_MAX_SLABS \
  ((MAX_ACTIVE_CHUNKS + DENSE_SLAB_CAPACITY - 1) / DENSE_SLAB_CAPACITY)
#define DENSE_MAX_BUFFERS (DENSE_MAX_SLABS * DENSE_SLAB_CAPACITY)

// One size class of buffers: slabs allocated on demand and a free list over
// every buffer they hold.
typedef struct {
  void *Slabs[DENSE_MAX_SLABS];
  void *FreeList[DENSE_MAX_BUFFERS];
  int SlabCount;
  int FreeCount;
  int Leased;
} BlockBufferClass;

// Block buffers for chunks that are not uniform: dense buffers for chunks being
// filled or holding more than PACKED_PALETTE_CAPACITY ids, packed buffers for
// the rest. Slabs are kept until DensePoolDestroy, so steady-state acquire and
// release are a free-list pop and push. Generation workers lease and release
// buffers while the main thread evicts and edits chunks, so every operation on
// either class holds the one spinlock; each critical section is a pop or push,
// except the rare slab allocation.
typedef struct DenseBlockPool {
  BlockBufferClass Dense;
  BlockBufferClass Packed;
  atomic_flag Lock;
} DenseBlockPool;

typedef struct {
  int Leased;
  int Slabs;
  int Capacity;
  int PackedLeased;
  int PackedSlabs;
  int PackedCapacity;
} DensePoolStats;

// Allocates no slab; the first acquire does.
void DensePoolInit(DenseBlockPool *Pool);
void DensePoolDestroy(DenseBlockPool *Pool);

// Return NULL only when a slab allocation fails. Contents are unspecified.
DenseBlockBuffer *DensePoolAcquire(DenseBlockPool *Pool);
void DensePoolRelease(DenseBlockPool *Pool, DenseBlockBuffer *Buffer);
PackedBlockBuffer *PackedPoolAcquire(DenseBlockPool *Pool);
void PackedPoolRelease(DenseBlockPool *Pool, PackedBlockBuffer *Buffer);

void DensePoolGetStats(DenseBlockPool *Pool, DensePoolStats *Out);

#endif
