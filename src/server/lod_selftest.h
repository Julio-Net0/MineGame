#ifndef SERVER_LOD_SELFTEST_H
#define SERVER_LOD_SELFTEST_H

#include <stdbool.h>

// Checks the LOD hierarchy in src/world/lod against hand-built grids and real
// generated chunks: voxel packing, block classes, the 2x2x2 rule (class
// priority, thin surface layers), all four storage forms, a fixed content hash,
// independence from ingestion order, edit propagation to the top level and
// back to uniform air, real-over-synthetic precedence, and clipmap eviction.
//
// Runs on the calling thread after the registries are loaded. Logs one
// "LOD_TEST: ok ..." line per check and stops at the first failure with
// "LOD_TEST: FAIL ...".
bool RunLodSelfTest(void);

#endif
