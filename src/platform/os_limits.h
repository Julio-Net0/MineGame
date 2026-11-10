#ifndef PLATFORM_OS_LIMITS_H
#define PLATFORM_OS_LIMITS_H

#include <stdbool.h>
#include <stddef.h>

// Restrict the process to one logical processor on each of the first CoreCount
// physical cores. Clamps CoreCount to the available physical cores and writes
// the applied count to OutApplied (when non-NULL) on success.
bool OsLimitPhysicalCores(int CoreCount, int *OutApplied);

// Cap the process's memory at Bytes (committed memory on Windows via a Job
// Object, RLIMIT_DATA on Linux).
bool OsLimitProcessMemory(size_t Bytes);

// Peak process memory in bytes since startup (committed on Windows, resident on
// Linux), or 0 if unavailable.
size_t OsGetPeakProcessMemory(void);

// Human-readable name for the quantity returned by OsGetPeakProcessMemory
// ("committed" on Windows, "resident" on Linux).
const char *OsGetPeakProcessMemoryKind(void);

#endif
