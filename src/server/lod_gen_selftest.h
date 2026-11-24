#ifndef SERVER_LOD_GEN_SELFTEST_H
#define SERVER_LOD_GEN_SELFTEST_H

#include <stdbool.h>

// Checks the synthetic LOD generator (src/world/lod/lod_gen.c): the lattice
// terrain-top probe against the general one, determinism of generated columns,
// agreement with the downsampled real chunks at levels 1 to 3 against fixed
// minimums, parent/child consistency of the present cells, the store's
// real-over-synthetic precedence for whole synthetic sections, and the
// generator's cost per section.
//
// Part of --test-lod; runs after RunLodSelfTest's checks, on the calling thread.
bool RunLodGenSelfTest(void);

#endif
