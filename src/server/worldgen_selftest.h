#ifndef SERVER_WORLDGEN_SELFTEST_H
#define SERVER_WORLDGEN_SELFTEST_H

#include <stdbool.h>

// Proves the generation accelerators are transparent. A fixed set of cubes
// (above the ceiling, around the surface, deep stone, the bedrock band, the
// reserved void) is generated three times: with classification and the column
// cache, with classification disabled, and with an uncached column entry. Block
// data, solid counts, and biome maps must match byte for byte, every cache
// entry must equal the pure samplers, and every cube class must be exercised.
//
// Runs on the calling thread with the main-thread cache slot, after the
// registries are loaded. Logs one "WORLDGEN_TEST: ok ..." line per check and
// stops at the first failure with "WORLDGEN_TEST: FAIL ...".
bool RunWorldgenSelfTest(void);

#endif
