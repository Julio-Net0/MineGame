#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "platform/os_time.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>

enum {
  WIN_EXE_PATH_CAPACITY = 4096
};

double OsGetTimeSeconds(void) {
  static LARGE_INTEGER Frequency = {0};
  static LARGE_INTEGER StartCounter = {0};
  static bool Initialized = false;

  if (!Initialized) {
    (void)QueryPerformanceFrequency(&Frequency);
    (void)QueryPerformanceCounter(&StartCounter);
    Initialized = true;
  }

  if (Frequency.QuadPart <= 0) {
    return 0.0;
  }

  LARGE_INTEGER Now = {0};
  (void)QueryPerformanceCounter(&Now);
  LONGLONG Delta = Now.QuadPart - StartCounter.QuadPart;
  if (Delta < 0) {
    return 0.0;
  }
  return (double)Delta / (double)Frequency.QuadPart;
}

void OsSleepMillis(unsigned int Millis) { Sleep((DWORD)Millis); }

bool OsAnchorWorkingDirectoryToExecutable(void) {
  char ExePath[WIN_EXE_PATH_CAPACITY];
  DWORD Len = GetModuleFileNameA((HMODULE)0, ExePath,
                                 (DWORD)(sizeof(ExePath) - 1U));
  if (Len == 0 || Len >= (DWORD)(sizeof(ExePath) - 1U)) {
    return false;
  }
  ExePath[Len] = '\0';

  int LastSep = -1;
  for (DWORD Idx = 0; Idx < Len; Idx++) {
    if (ExePath[Idx] == '\\' || ExePath[Idx] == '/') {
      LastSep = (int)Idx;
    }
  }
  if (LastSep <= 0) {
    return false;
  }
  ExePath[LastSep] = '\0';
  return _chdir(ExePath) == 0;
}

#else

#include <time.h>
#include <unistd.h>

enum {
  POSIX_EXE_PATH_CAPACITY = 4096,
  MILLIS_PER_SECOND = 1000U,
  NANOS_PER_MILLI = 1000000L
};

#define NANOS_PER_SECOND_F 1.0e-9

double OsGetTimeSeconds(void) {
  static struct timespec StartTime = {0, 0};
  static bool Initialized = false;

  struct timespec Now = {0, 0};
  if (clock_gettime(CLOCK_MONOTONIC, &Now) != 0) {
    return 0.0;
  }

  if (!Initialized) {
    StartTime = Now;
    Initialized = true;
  }

  double SecDiff = (double)(Now.tv_sec - StartTime.tv_sec);
  double NanoDiff = (double)(Now.tv_nsec - StartTime.tv_nsec) * NANOS_PER_SECOND_F;
  double Total = SecDiff + NanoDiff;
  return (Total >= 0.0) ? Total : 0.0;
}

void OsSleepMillis(unsigned int Millis) {
  struct timespec Req = {
      .tv_sec = (time_t)(Millis / MILLIS_PER_SECOND),
      .tv_nsec = (long)(Millis % MILLIS_PER_SECOND) * NANOS_PER_MILLI,
  };
  (void)nanosleep(&Req, (struct timespec *)0);
}

bool OsAnchorWorkingDirectoryToExecutable(void) {
  char ExePath[POSIX_EXE_PATH_CAPACITY];
  ssize_t Len = readlink("/proc/self/exe", ExePath, sizeof(ExePath) - 1U);
  if (Len <= 0 || (size_t)Len >= sizeof(ExePath) - 1U) {
    return false;
  }
  ExePath[Len] = '\0';

  ssize_t LastSep = -1;
  for (ssize_t Idx = 0; Idx < Len; Idx++) {
    if (ExePath[Idx] == '/') {
      LastSep = Idx;
    }
  }
  if (LastSep <= 0) {
    return false;
  }
  ExePath[LastSep] = '\0';
  return chdir(ExePath) == 0;
}

#endif
