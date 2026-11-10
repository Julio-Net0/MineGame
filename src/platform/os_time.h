#ifndef PLATFORM_OS_TIME_H
#define PLATFORM_OS_TIME_H

#include <stdbool.h>

// Monotonic elapsed time in seconds since the first call in this process.
// Renderer-free so simulation, profiling, and the headless server share one
// high-resolution clock without initializing a window or graphics context.
double OsGetTimeSeconds(void);

// Suspend the calling thread for at least Millis milliseconds.
void OsSleepMillis(unsigned int Millis);

// Change the process working directory to the directory containing the running
// executable so relative asset paths resolve identically regardless of where
// the binary was launched from.
bool OsAnchorWorkingDirectoryToExecutable(void);

#endif
