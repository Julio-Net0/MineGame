#include "core/log.h"
#include "core/profiler.h"
#include "persistence/world_save.h"
#include "platform/os_limits.h"
#include "platform/os_time.h"
#include "server/protocol_selftest.h"
#include "server/server.h"
#include "world/chunk_worker.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define BYTES_PER_MEGABYTE (1024ULL * 1024ULL)
#define DECIMAL_BASE 10
#define MIN_CPU_SLOWDOWN 1.0
#define MAX_CPU_SLOWDOWN 16.0
#define MAX_RAM_LIMIT_MB 1048576L
#define MILLIS_PER_SECOND_D 1000.0
#define SERVER_DEFAULT_SETTLE_TIMEOUT_SECONDS 60.0

typedef struct {
  ServerConfig Config;
  long MaxTicks;
  bool HasMaxTicks;
  bool PaceRealtime;
  bool TestProtocol;
  int CoreCount;
  double CpuSlowdown;
  size_t RamLimitMb;
} ServerLaunchOptions;

static volatile sig_atomic_t GStopRequested = 0;

static void HandleStopSignal(int SignalValue) {
  (void)SignalValue;
  GStopRequested = 1;
}

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

static ServerLaunchOptions ParseServerOptions(int Argc, char **Argv) {
  ServerLaunchOptions Options = {
      .Config =
          {
              .WorkerCount = CHUNK_WORKER_MAX_THREADS,
              .RenderDistance = MAX_RENDER_DISTANCE,
              .HasSeed = false,
              .Seed = 0,
              .ReadOnly = false,
              .SettleTimeoutSeconds = SERVER_DEFAULT_SETTLE_TIMEOUT_SECONDS,
          },
      .MaxTicks = 0,
      .HasMaxTicks = false,
      .PaceRealtime = false,
      .TestProtocol = false,
      .CoreCount = 0,
      .CpuSlowdown = 1.0,
      .RamLimitMb = 0,
  };

  for (int Idx = 1; Idx < Argc; Idx++) {
    const char *Arg = Argv[Idx];
    if (strcmp(Arg, "--read-only") == 0) {
      Options.Config.ReadOnly = true;
      continue;
    }
    if (strcmp(Arg, "--pace-realtime") == 0) {
      Options.PaceRealtime = true;
      continue;
    }
    if (strcmp(Arg, "--test-protocol") == 0) {
      Options.TestProtocol = true;
      continue;
    }

    bool NeedsValue = (strcmp(Arg, "--ticks") == 0) ||
                      (strcmp(Arg, "--seed") == 0) ||
                      (strcmp(Arg, "--workers") == 0) ||
                      (strcmp(Arg, "--cores") == 0) ||
                      (strcmp(Arg, "--cpu-slowdown") == 0) ||
                      (strcmp(Arg, "--ram-limit") == 0);
    if (!NeedsValue) {
      LogWarn("SERVER: unknown launch flag \"%s\"; ignoring", Arg);
      continue;
    }

    if (Idx + 1 >= Argc || IsFlagToken(Argv[Idx + 1])) {
      LogWarn("SERVER: launch flag \"%s\" is missing a value; ignoring", Arg);
      continue;
    }

    const char *ValueStr = Argv[++Idx];
    if (strcmp(Arg, "--ticks") == 0) {
      long Value = 0;
      if (ParseStrictLong(ValueStr, &Value) && Value >= 0) {
        Options.MaxTicks = Value;
        Options.HasMaxTicks = true;
      } else {
        LogWarn("SERVER: --ticks must be a non-negative integer; ignoring \"%s\"",
                ValueStr);
      }
    } else if (strcmp(Arg, "--seed") == 0) {
      uint64_t Value = 0;
      if (ParseStrictUint64(ValueStr, &Value)) {
        Options.Config.HasSeed = true;
        Options.Config.Seed = Value;
      } else {
        LogWarn("SERVER: --seed must be an unsigned 64-bit integer; ignoring \"%s\"",
                ValueStr);
      }
    } else if (strcmp(Arg, "--workers") == 0) {
      long Value = 0;
      if (ParseStrictLong(ValueStr, &Value) && Value >= 1 &&
          Value <= CHUNK_WORKER_MAX_THREADS) {
        Options.Config.WorkerCount = (int)Value;
      } else {
        LogWarn("SERVER: --workers must be in [1, %d]; ignoring \"%s\"",
                CHUNK_WORKER_MAX_THREADS, ValueStr);
      }
    } else if (strcmp(Arg, "--cores") == 0) {
      long Value = 0;
      if (ParseStrictLong(ValueStr, &Value) && Value >= 1 && Value <= INT_MAX) {
        Options.CoreCount = (int)Value;
      } else {
        LogWarn("SERVER: --cores must be a positive integer; ignoring \"%s\"",
                ValueStr);
      }
    } else if (strcmp(Arg, "--cpu-slowdown") == 0) {
      double Value = 0.0;
      if (ParseStrictDouble(ValueStr, &Value) && Value >= MIN_CPU_SLOWDOWN &&
          Value <= MAX_CPU_SLOWDOWN) {
        Options.CpuSlowdown = Value;
      } else {
        LogWarn("SERVER: --cpu-slowdown must be in [%.1f, %.1f]; ignoring \"%s\"",
                MIN_CPU_SLOWDOWN, MAX_CPU_SLOWDOWN, ValueStr);
      }
    } else if (strcmp(Arg, "--ram-limit") == 0) {
      long Value = 0;
      if (ParseStrictLong(ValueStr, &Value) && Value >= 1 &&
          Value <= MAX_RAM_LIMIT_MB) {
        Options.RamLimitMb = (size_t)Value;
      } else {
        LogWarn("SERVER: --ram-limit must be a positive integer (MB); ignoring \"%s\"",
                ValueStr);
      }
    }
  }

  return Options;
}

static void ApplyProcessLimits(const ServerLaunchOptions *Options) {
  if (Options->CoreCount > 0) {
    int AppliedCores = 0;
    if (OsLimitPhysicalCores(Options->CoreCount, &AppliedCores)) {
      LogInfo("SERVER: simulation --cores %d", AppliedCores);
    }
  }
  if (Options->RamLimitMb > 0) {
    LogInfo("SERVER: simulation --ram-limit %zu MB", Options->RamLimitMb);
    (void)OsLimitProcessMemory(Options->RamLimitMb * BYTES_PER_MEGABYTE);
  }
#ifdef MINEGAME_PROFILE
  if (Options->CpuSlowdown > 1.0) {
    ProfilerSetSlowdown(Options->CpuSlowdown);
    LogInfo("SERVER: simulation --cpu-slowdown %.2f", Options->CpuSlowdown);
  }
#endif
}

int main(int Argc, char **Argv) {
  (void)signal(SIGINT, HandleStopSignal);
  (void)signal(SIGTERM, HandleStopSignal);

  ServerLaunchOptions Options = ParseServerOptions(Argc, Argv);
  ApplyProcessLimits(&Options);

  if (Options.TestProtocol && !RunProtocolCodecSelfTest()) {
    return EXIT_FAILURE;
  }

  ServerState Server = {0};
  if (!ServerInit(&Server, &Options.Config)) {
    return EXIT_FAILURE;
  }

  if (Options.TestProtocol) {
    bool Passed = RunProtocolServerSelfTest(&Server);
    ServerShutdown(&Server);
    if (!Passed) {
      return EXIT_FAILURE;
    }
    LogInfo("PROTOCOL_TEST: PASS");
    return EXIT_SUCCESS;
  }

  LogInfo("SERVER: started headless 20 TPS loop (seed=%llu)",
          (unsigned long long)GetWorldSeed());

  bool Paced = !Options.HasMaxTicks || Options.PaceRealtime;
  while (GStopRequested == 0) {
    if (Options.HasMaxTicks && Server.TickCount >= (uint64_t)Options.MaxTicks) {
      break;
    }

    double TickStart = OsGetTimeSeconds();
    ServerStepTick(&Server);
    PROFILE_FRAME_END();

    if (Paced) {
      double ElapsedMs =
          (OsGetTimeSeconds() - TickStart) * MILLIS_PER_SECOND_D;
      if (ElapsedMs < (double)SERVER_TICK_MS) {
        unsigned int RemainingMs =
            (unsigned int)((double)SERVER_TICK_MS - ElapsedMs);
        if (RemainingMs > 0U) {
          OsSleepMillis(RemainingMs);
        }
      }
    }
  }

  int Generated = 0;
  int Generating = 0;
  int Dirty = 0;
  int Meshing = 0;
  WorldGetLoadCounts(Server.WorldVal, &Generated, &Generating, &Dirty,
                     &Meshing);
  bool Settled = WorldIsGenerationSettled(
      Server.WorldVal, Server.PlayerVal.Position, Server.RenderDistance);

  LogInfo("SERVER: summary ticks=%llu seed=%llu settled=%s active=%d "
          "generated=%d generating=%d meshing=%d pos=(%.1f, %.1f, %.1f) "
          "vel=(%.1f, %.1f, %.1f) grounded=%s",
          (unsigned long long)Server.TickCount,
          (unsigned long long)GetWorldSeed(), Settled ? "true" : "false",
          Server.WorldVal->ChunkCount, Generated, Generating, Meshing,
          (double)Server.PlayerVal.Position.x,
          (double)Server.PlayerVal.Position.y,
          (double)Server.PlayerVal.Position.z,
          (double)Server.PlayerVal.Velocity.x,
          (double)Server.PlayerVal.Velocity.y,
          (double)Server.PlayerVal.Velocity.z,
          Server.PlayerVal.IsGrounded ? "true" : "false");

  ServerShutdown(&Server);

  size_t PeakBytes = OsGetPeakProcessMemory();
  if (PeakBytes > 0) {
    double PeakMb = (double)PeakBytes / (double)BYTES_PER_MEGABYTE;
    LogInfo("SERVER: peak process memory (%s): %.1f MB",
            OsGetPeakProcessMemoryKind(), PeakMb);
  }

  return EXIT_SUCCESS;
}
