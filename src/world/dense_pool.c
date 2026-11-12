#include "world/dense_pool.h"
#include "core/log.h"
#include <stdlib.h>

static void LockPool(DenseBlockPool *Pool) {
  while (atomic_flag_test_and_set_explicit(&Pool->Lock, memory_order_acquire)) {
  }
}

static void UnlockPool(DenseBlockPool *Pool) {
  atomic_flag_clear_explicit(&Pool->Lock, memory_order_release);
}

void DensePoolInit(DenseBlockPool *Pool) {
  Pool->SlabCount = 0;
  Pool->FreeCount = 0;
  Pool->Leased = 0;
  atomic_flag_clear(&Pool->Lock);
}

void DensePoolDestroy(DenseBlockPool *Pool) {
  for (int Idx = 0; Idx < Pool->SlabCount; Idx++) {
    free(Pool->Slabs[Idx]);
    Pool->Slabs[Idx] = (DenseBlockBuffer *)0;
  }
  Pool->SlabCount = 0;
  Pool->FreeCount = 0;
  Pool->Leased = 0;
}

// Caller holds Lock. Pushed in reverse so the slab's first buffer pops first.
static bool GrowPool(DenseBlockPool *Pool) {
  if (Pool->SlabCount >= DENSE_MAX_SLABS) {
    return false;
  }
  DenseBlockBuffer *Slab =
      (DenseBlockBuffer *)malloc(sizeof(DenseBlockBuffer) * DENSE_SLAB_CAPACITY);
  if (Slab == (DenseBlockBuffer *)0) {
    LogError("DENSE_POOL: out of memory allocating slab %d", Pool->SlabCount);
    return false;
  }
  Pool->Slabs[Pool->SlabCount++] = Slab;
  for (int Idx = DENSE_SLAB_CAPACITY - 1; Idx >= 0; Idx--) {
    Pool->FreeList[Pool->FreeCount++] = &Slab[Idx];
  }
  return true;
}

DenseBlockBuffer *DensePoolAcquire(DenseBlockPool *Pool) {
  DenseBlockBuffer *Buffer = (DenseBlockBuffer *)0;
  LockPool(Pool);
  if (Pool->FreeCount > 0 || GrowPool(Pool)) {
    Buffer = Pool->FreeList[--Pool->FreeCount];
    Pool->Leased++;
  }
  UnlockPool(Pool);
  return Buffer;
}

void DensePoolRelease(DenseBlockPool *Pool, DenseBlockBuffer *Buffer) {
  if (Buffer == (DenseBlockBuffer *)0) {
    return;
  }
  LockPool(Pool);
  if (Pool->FreeCount < Pool->SlabCount * DENSE_SLAB_CAPACITY) {
    Pool->FreeList[Pool->FreeCount++] = Buffer;
    Pool->Leased--;
  } else {
    LogError("DENSE_POOL: release of %p with every buffer already free",
             (void *)Buffer);
  }
  UnlockPool(Pool);
}

void DensePoolGetStats(DenseBlockPool *Pool, DensePoolStats *Out) {
  LockPool(Pool);
  Out->Leased = Pool->Leased;
  Out->Slabs = Pool->SlabCount;
  Out->Capacity = Pool->SlabCount * DENSE_SLAB_CAPACITY;
  UnlockPool(Pool);
}
