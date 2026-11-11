#ifndef CORE_HASH_H
#define CORE_HASH_H

#include <stdint.h>

// SplitMix64: one multiply-xor-shift round per stage, enough to turn adjacent
// seeds, salts, and cell coordinates into uncorrelated bits. Every subsystem that
// derives sub-seeds or hashes positions goes through this one definition, so
// they cannot drift into subtly different mixers.
static inline uint64_t SplitMix64(uint64_t Value) {
  const uint64_t GOLDEN_GAMMA = 0x9E3779B97F4A7C15ULL;
  const uint64_t MIX_A = 0xBF58476D1CE4E5B9ULL;
  const uint64_t MIX_B = 0x94D049BB133111EBULL;
  const unsigned int SHIFT_A = 30U;
  const unsigned int SHIFT_B = 27U;
  const unsigned int SHIFT_C = 31U;

  Value += GOLDEN_GAMMA;
  Value = (Value ^ (Value >> SHIFT_A)) * MIX_A;
  Value = (Value ^ (Value >> SHIFT_B)) * MIX_B;
  return Value ^ (Value >> SHIFT_C);
}

#endif
