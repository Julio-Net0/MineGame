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

static void ClassInit(BlockBufferClass *Class) {
  Class->SlabCount = 0;
  Class->FreeCount = 0;
  Class->Leased = 0;
}

static void ClassDestroy(BlockBufferClass *Class) {
  for (int Idx = 0; Idx < Class->SlabCount; Idx++) {
    free(Class->Slabs[Idx]);
    Class->Slabs[Idx] = NULL;
  }
  ClassInit(Class);
}

// Caller holds Lock. Pushed in reverse so the slab's first buffer pops first.
static bool GrowClass(BlockBufferClass *Class, size_t BufferBytes) {
  if (Class->SlabCount >= DENSE_MAX_SLABS) {
    return false;
  }
  unsigned char *Slab = (unsigned char *)malloc(BufferBytes * DENSE_SLAB_CAPACITY);
  if (Slab == NULL) {
    LogError("DENSE_POOL: out of memory allocating slab %d", Class->SlabCount);
    return false;
  }
  Class->Slabs[Class->SlabCount++] = Slab;
  for (int Idx = DENSE_SLAB_CAPACITY - 1; Idx >= 0; Idx--) {
    Class->FreeList[Class->FreeCount++] = &Slab[(size_t)Idx * BufferBytes];
  }
  return true;
}

static void *ClassAcquire(DenseBlockPool *Pool, BlockBufferClass *Class,
                          size_t BufferBytes) {
  void *Buffer = NULL;
  LockPool(Pool);
  if (Class->FreeCount > 0 || GrowClass(Class, BufferBytes)) {
    Buffer = Class->FreeList[--Class->FreeCount];
    Class->Leased++;
  }
  UnlockPool(Pool);
  return Buffer;
}

static void ClassRelease(DenseBlockPool *Pool, BlockBufferClass *Class,
                         void *Buffer) {
  if (Buffer == NULL) {
    return;
  }
  LockPool(Pool);
  if (Class->FreeCount < Class->SlabCount * DENSE_SLAB_CAPACITY) {
    Class->FreeList[Class->FreeCount++] = Buffer;
    Class->Leased--;
  } else {
    LogError("DENSE_POOL: release of %p with every buffer already free", Buffer);
  }
  UnlockPool(Pool);
}

void DensePoolInit(DenseBlockPool *Pool) {
  ClassInit(&Pool->Dense);
  ClassInit(&Pool->Packed);
  atomic_flag_clear(&Pool->Lock);
}

void DensePoolDestroy(DenseBlockPool *Pool) {
  ClassDestroy(&Pool->Dense);
  ClassDestroy(&Pool->Packed);
}

DenseBlockBuffer *DensePoolAcquire(DenseBlockPool *Pool) {
  return (DenseBlockBuffer *)ClassAcquire(Pool, &Pool->Dense,
                                          sizeof(DenseBlockBuffer));
}

void DensePoolRelease(DenseBlockPool *Pool, DenseBlockBuffer *Buffer) {
  ClassRelease(Pool, &Pool->Dense, Buffer);
}

PackedBlockBuffer *PackedPoolAcquire(DenseBlockPool *Pool) {
  return (PackedBlockBuffer *)ClassAcquire(Pool, &Pool->Packed,
                                           sizeof(PackedBlockBuffer));
}

void PackedPoolRelease(DenseBlockPool *Pool, PackedBlockBuffer *Buffer) {
  ClassRelease(Pool, &Pool->Packed, Buffer);
}

void DensePoolGetStats(DenseBlockPool *Pool, DensePoolStats *Out) {
  LockPool(Pool);
  Out->Leased = Pool->Dense.Leased;
  Out->Slabs = Pool->Dense.SlabCount;
  Out->Capacity = Pool->Dense.SlabCount * DENSE_SLAB_CAPACITY;
  Out->PackedLeased = Pool->Packed.Leased;
  Out->PackedSlabs = Pool->Packed.SlabCount;
  Out->PackedCapacity = Pool->Packed.SlabCount * DENSE_SLAB_CAPACITY;
  UnlockPool(Pool);
}
