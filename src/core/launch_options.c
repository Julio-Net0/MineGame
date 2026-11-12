#include "core/launch_options.h"

#include "core/log.h"
#include "world/chunk_worker.h"
#include "world/world.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

// Bounds on a framebuffer size given on the command line. The lower bound only
// has to keep the HUD from being degenerate; the upper one is 8K.
#define MIN_REQUESTED_DIMENSION 160
#define MAX_REQUESTED_DIMENSION 7680
#define DECIMAL_BASE 10

#define MIN_CPU_SLOWDOWN 1.0
#define MAX_CPU_SLOWDOWN 16.0
#define MAX_RAM_LIMIT_MB 1048576L

#define MIN_SCRIPT_TIMEOUT_SECONDS 1L
#define MAX_SCRIPT_TIMEOUT_SECONDS 3600L

enum {
  MAX_POSITIONAL_ARGS = 3
};

static LaunchOptions *GetMutableLaunchOptions(void) {
  static LaunchOptions Options = {
      .Window =
          {
              .Width = INITIAL_WIDTH,
              .Height = INITIAL_HEIGHT,
              .Fullscreen = true,
          },
      .WorkerCount = 0,
      .CoreCount = 0,
      .CpuSlowdown = 1.0,
      .RamLimitMb = 0,
      .RenderDistance = 0,
      .HasSeed = false,
      .Seed = 0,
      .ScriptPath = (const char *)0,
      .ScriptOutDir = SCRIPT_DEFAULT_OUT_DIR,
      .ScriptTimeoutSeconds = SCRIPT_DEFAULT_TIMEOUT_SECONDS,
  };
  return &Options;
}

const LaunchOptions *GetLaunchOptions(void) {
  return GetMutableLaunchOptions();
}

void SetAppliedCoreCount(int CoreCount) {
  GetMutableLaunchOptions()->CoreCount = (CoreCount > 0) ? CoreCount : 0;
}

void SetAppliedRamLimitMb(size_t RamLimitMb) {
  GetMutableLaunchOptions()->RamLimitMb = RamLimitMb;
}

// A token starting with '-' is treated as a flag unless its second character is
// a digit, so negative numbers passed as flag values ("--workers -1") reach the
// flag's own validation rather than being misread as a new flag.
static bool IsFlagToken(const char *Arg) {
  if (Arg == (const char *)0 || Arg[0] != '-' || Arg[1] == '\0') {
    return false;
  }
  if (Arg[1] == '-') {
    return true;
  }
  return (Arg[1] < '0') || (Arg[1] > '9');
}

static bool ParseStrictLong(const char *Str, long *OutValue) {
  if (Str == (const char *)0 || Str[0] == '\0') {
    return false;
  }
  errno = 0;
  char *End = (char *)0;
  long Value = strtol(Str, &End, DECIMAL_BASE);
  if (errno == ERANGE || End == Str || *End != '\0') {
    return false;
  }
  *OutValue = Value;
  return true;
}

static bool ParseStrictUint64(const char *Str, uint64_t *OutValue) {
  if (Str == (const char *)0 || Str[0] < '0' || Str[0] > '9') {
    return false;
  }
  errno = 0;
  char *End = (char *)0;
  unsigned long long Value = strtoull(Str, &End, DECIMAL_BASE);
  if (errno == ERANGE || End == Str || *End != '\0') {
    return false;
  }
  *OutValue = (uint64_t)Value;
  return true;
}

static bool ParseStrictDouble(const char *Str, double *OutValue) {
  if (Str == (const char *)0 || Str[0] == '\0') {
    return false;
  }
  errno = 0;
  char *End = (char *)0;
  double Value = strtod(Str, &End);
  if (errno == ERANGE || End == Str || *End != '\0' || !isfinite(Value)) {
    return false;
  }
  *OutValue = Value;
  return true;
}

static int CountFollowingNonFlags(int Argc, char **Argv, int FlagIdx) {
  int Count = 0;
  for (int Idx = FlagIdx + 1; Idx < Argc; Idx++) {
    if (IsFlagToken(Argv[Idx])) {
      break;
    }
    Count++;
  }
  return Count;
}

// Parses an optional "<width> <height>" pair from the positionals left after
// flags are consumed. Anything else -- no positionals, a wrong count, a
// non-number, a dimension out of range -- leaves the request fullscreen, so a
// malformed invocation starts the game normally.
static WindowRequest ParseWindowPositionals(int PositionalCount,
                                            const char *const *Positionals) {
  WindowRequest Request = {
      .Width = INITIAL_WIDTH,
      .Height = INITIAL_HEIGHT,
      .Fullscreen = true,
  };

  if (PositionalCount != 2) {
    if (PositionalCount > 0) {
      LogWarn("MAIN: expected \"<width> <height>\" or no arguments; starting "
              "fullscreen");
    }
    return Request;
  }

  long Width = 0;
  long Height = 0;
  bool Parsed = ParseStrictLong(Positionals[0], &Width) &&
                ParseStrictLong(Positionals[1], &Height);
  bool InRange = (Width >= MIN_REQUESTED_DIMENSION) &&
                 (Width <= MAX_REQUESTED_DIMENSION) &&
                 (Height >= MIN_REQUESTED_DIMENSION) &&
                 (Height <= MAX_REQUESTED_DIMENSION);

  if (!Parsed || !InRange) {
    LogWarn("MAIN: window size must be two integers in [%d, %d]; starting "
            "fullscreen",
            MIN_REQUESTED_DIMENSION, MAX_REQUESTED_DIMENSION);
    return Request;
  }

  Request.Width = (int)Width;
  Request.Height = (int)Height;
  Request.Fullscreen = false;
  LogInfo("MAIN: windowed at %dx%d", Request.Width, Request.Height);
  return Request;
}

static void ParseWorkersValue(const char *ValueStr, LaunchOptions *Options) {
  long Value = 0;
  if (!ParseStrictLong(ValueStr, &Value) || Value < 1 ||
      Value > CHUNK_WORKER_MAX_THREADS) {
    LogWarn("MAIN: --workers must be an integer in [1, %d]; ignoring \"%s\"",
            CHUNK_WORKER_MAX_THREADS, ValueStr);
    return;
  }
  Options->WorkerCount = (int)Value;
}

static void ParseCoresValue(const char *ValueStr, LaunchOptions *Options) {
  long Value = 0;
  if (!ParseStrictLong(ValueStr, &Value) || Value < 1 || Value > INT_MAX) {
    LogWarn("MAIN: --cores must be a positive integer; ignoring \"%s\"",
            ValueStr);
    return;
  }
  Options->CoreCount = (int)Value;
}

static void ParseCpuSlowdownValue(const char *ValueStr,
                                  LaunchOptions *Options) {
  double Value = 0.0;
  if (!ParseStrictDouble(ValueStr, &Value) || Value < MIN_CPU_SLOWDOWN ||
      Value > MAX_CPU_SLOWDOWN) {
    LogWarn(
        "MAIN: --cpu-slowdown must be a number in [%.1f, %.1f]; ignoring \"%s\"",
        MIN_CPU_SLOWDOWN, MAX_CPU_SLOWDOWN, ValueStr);
    return;
  }
#ifdef MINEGAME_PROFILE
  Options->CpuSlowdown = Value;
#else
  (void)Options;
  LogWarn("MAIN: --cpu-slowdown requires MINEGAME_PROFILE=ON; ignoring");
#endif
}

static void ParseScriptTimeoutValue(const char *ValueStr,
                                    LaunchOptions *Options) {
  long Value = 0;
  if (!ParseStrictLong(ValueStr, &Value) || Value < MIN_SCRIPT_TIMEOUT_SECONDS ||
      Value > MAX_SCRIPT_TIMEOUT_SECONDS) {
    LogWarn("MAIN: --script-timeout must be an integer in [%ld, %ld] seconds; "
            "ignoring \"%s\"",
            MIN_SCRIPT_TIMEOUT_SECONDS, MAX_SCRIPT_TIMEOUT_SECONDS, ValueStr);
    return;
  }
  Options->ScriptTimeoutSeconds = (double)Value;
}

static void ParseRamLimitValue(const char *ValueStr, LaunchOptions *Options) {
  long Value = 0;
  if (!ParseStrictLong(ValueStr, &Value) || Value < 1 ||
      Value > MAX_RAM_LIMIT_MB) {
    LogWarn("MAIN: --ram-limit must be a positive integer (MB); ignoring \"%s\"",
            ValueStr);
    return;
  }
  Options->RamLimitMb = (size_t)Value;
}

static void ParseRenderDistanceValue(const char *ValueStr,
                                     LaunchOptions *Options) {
  long Value = 0;
  if (!ParseStrictLong(ValueStr, &Value) || Value < MIN_RENDER_DISTANCE ||
      Value > MAX_RENDER_DISTANCE) {
    LogWarn("MAIN: --render-distance must be an integer in [%d, %d]; ignoring "
            "\"%s\"",
            MIN_RENDER_DISTANCE, MAX_RENDER_DISTANCE, ValueStr);
    return;
  }
  Options->RenderDistance = (int)Value;
}

static void ParseSeedValue(const char *ValueStr, LaunchOptions *Options) {
  uint64_t Value = 0;
  if (!ParseStrictUint64(ValueStr, &Value)) {
    LogWarn("MAIN: --seed must be an unsigned 64-bit integer; ignoring \"%s\"",
            ValueStr);
    return;
  }
  Options->HasSeed = true;
  Options->Seed = Value;
}

void ParseLaunchOptions(int Argc, char **Argv) {
  LaunchOptions *Options = GetMutableLaunchOptions();
  *Options = (LaunchOptions){
      .Window =
          {
              .Width = INITIAL_WIDTH,
              .Height = INITIAL_HEIGHT,
              .Fullscreen = true,
          },
      .WorkerCount = 0,
      .CoreCount = 0,
      .CpuSlowdown = 1.0,
      .RamLimitMb = 0,
      .RenderDistance = 0,
      .HasSeed = false,
      .Seed = 0,
      .ScriptPath = (const char *)0,
      .ScriptOutDir = SCRIPT_DEFAULT_OUT_DIR,
      .ScriptTimeoutSeconds = SCRIPT_DEFAULT_TIMEOUT_SECONDS,
  };

  if (Argc <= 1 || Argv == (char **)0) {
    return;
  }

  const char *Positionals[MAX_POSITIONAL_ARGS] = {0};
  int PositionalCount = 0;
  bool SawScriptOutDir = false;
  bool SawScriptTimeout = false;

  for (int Idx = 1; Idx < Argc; Idx++) {
    const char *Arg = Argv[Idx];
    if (!IsFlagToken(Arg)) {
      if (PositionalCount < MAX_POSITIONAL_ARGS) {
        Positionals[PositionalCount] = Arg;
      }
      PositionalCount++;
      continue;
    }

    bool KnownFlag = (strcmp(Arg, "--workers") == 0) ||
                     (strcmp(Arg, "--cores") == 0) ||
                     (strcmp(Arg, "--cpu-slowdown") == 0) ||
                     (strcmp(Arg, "--ram-limit") == 0) ||
                     (strcmp(Arg, "--seed") == 0) ||
                     (strcmp(Arg, "--render-distance") == 0) ||
                     (strcmp(Arg, "--script") == 0) ||
                     (strcmp(Arg, "--script-out") == 0) ||
                     (strcmp(Arg, "--script-timeout") == 0);
    if (!KnownFlag) {
      LogWarn("MAIN: unknown launch flag \"%s\"; ignoring", Arg);
      int Following = CountFollowingNonFlags(Argc, Argv, Idx);
      if (Following > 0 && Following != 2) {
        Idx++;
      }
      continue;
    }

    if (Idx + 1 >= Argc || IsFlagToken(Argv[Idx + 1])) {
      LogWarn("MAIN: launch flag \"%s\" is missing a value; ignoring", Arg);
      continue;
    }

    const char *ValueStr = Argv[++Idx];
    if (strcmp(Arg, "--workers") == 0) {
      ParseWorkersValue(ValueStr, Options);
    } else if (strcmp(Arg, "--cores") == 0) {
      ParseCoresValue(ValueStr, Options);
    } else if (strcmp(Arg, "--cpu-slowdown") == 0) {
      ParseCpuSlowdownValue(ValueStr, Options);
    } else if (strcmp(Arg, "--ram-limit") == 0) {
      ParseRamLimitValue(ValueStr, Options);
    } else if (strcmp(Arg, "--seed") == 0) {
      ParseSeedValue(ValueStr, Options);
    } else if (strcmp(Arg, "--render-distance") == 0) {
      ParseRenderDistanceValue(ValueStr, Options);
    } else if (strcmp(Arg, "--script") == 0) {
      Options->ScriptPath = ValueStr;
    } else if (strcmp(Arg, "--script-out") == 0) {
      SawScriptOutDir = true;
      Options->ScriptOutDir = ValueStr;
    } else if (strcmp(Arg, "--script-timeout") == 0) {
      SawScriptTimeout = true;
      ParseScriptTimeoutValue(ValueStr, Options);
    }
  }

  Options->Window = ParseWindowPositionals(PositionalCount, Positionals);

  if (Options->ScriptPath == (const char *)0) {
    if (SawScriptOutDir || SawScriptTimeout) {
      LogWarn("MAIN: --script-out and --script-timeout need --script; ignoring");
      Options->ScriptOutDir = SCRIPT_DEFAULT_OUT_DIR;
      Options->ScriptTimeoutSeconds = SCRIPT_DEFAULT_TIMEOUT_SECONDS;
    }
    return;
  }

  // A script's screenshots are only comparable between runs at a fixed size, so
  // script mode never inherits the fullscreen default. An explicit size on the
  // command line still wins; the fallback is the windowed default already in
  // WindowRequest.
  if (Options->Window.Fullscreen) {
    Options->Window.Fullscreen = false;
    LogInfo("MAIN: script mode is windowed at %dx%d", Options->Window.Width,
            Options->Window.Height);
  }
}
