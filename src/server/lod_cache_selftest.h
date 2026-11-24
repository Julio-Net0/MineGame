#ifndef SERVER_LOD_CACHE_SELFTEST_H
#define SERVER_LOD_CACHE_SELFTEST_H

#include <stdbool.h>

// Checks the LOD disk cache (src/world/lod/lod_cache.c) in a scratch
// directory it creates and removes: bit-exact round trips of every storage mode
// through both the run-length and the raw encodings, discarding the cache when
// the seed, generator version or format version changes, surviving truncated
// and corrupted files, keeping real-over-synthetic precedence across a save and
// a reload into a new store, and creating nothing when not writable.
//
// Part of --test-lod; runs after the generator checks, on the calling thread.
bool RunLodCacheSelfTest(void);

#endif
