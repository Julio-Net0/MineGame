#include "ui/commands.h"
#include "world/block_system.h"
#include "world/biome.h"
#include "world/chunk.h"
#include "world/feature.h"
#include "world/prefab.h"
#include "world/prefab_capture.h"
#include "world/surface.h"
#include "ui/chat.h"
#include "ui/debug.h"
#include "ui/ui.h"
#include "core/log.h"
#include "core/utils.h"
#include "core/profiler.h"
#include "world/world.h"
#include "persistence/world_save.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
  FILLCHAT_DEFAULT_LINES = 20,
  DEBUG_HELP_LINE_SIZE = 128,
  DEBUG_STATUS_MSG_SIZE = 64,
  MSG_BUFFER_SIZE = 256,
  MAX_AUTOCOMPLETE_CANDIDATES = 64,
  MAX_AUTOCOMPLETE_TOKEN_LENGTH = 64
};

static void CommandHelp(const char *Args, CommandContext *Ctx);
static void CommandTP(const char *Args, CommandContext *Ctx);
static void CommandPos(const char *Args, CommandContext *Ctx);
static void CommandList(const char *Args, CommandContext *Ctx);
static void CommandNoclip(const char *Args, CommandContext *Ctx);
static void CommandSpeed(const char *Args, CommandContext *Ctx);
static void CommandDebug(const char *Args, CommandContext *Ctx);
static void CommandSeed(const char *Args, CommandContext *Ctx);
static void CommandSave(const char *Args, CommandContext *Ctx);
static void CommandPrefab(const char *Args, CommandContext *Ctx);
static void CommandBiome(const char *Args, CommandContext *Ctx);
static void CommandLocate(const char *Args, CommandContext *Ctx);
static void CommandGuiScale(const char *Args, CommandContext *Ctx);
static void CommandFillChat(const char *Args, CommandContext *Ctx);
#ifdef MINEGAME_PROFILE
static void CommandProfile(const char *Args, CommandContext *Ctx);
#endif

static const CommandInfo AVAILABLECOMMANDS[] = {
    {"/help", "Use: /help",
     "List all the commands available and how to use them", CommandHelp},
    {"/tp", "Use: /tp <x> <y> <z> (~ = relative)", "Teleports to coordinates x y z",
     CommandTP},
    {"/pos", "Use: /pos", "Returns your current position", CommandPos},
    {"/list", "Use: /list", "Returns all loaded block assets", CommandList},
    {"/noclip", "Use: /noclip <1/0 | speed <val>>",
     "Activate/deactivate noclip flight or adjust speed", CommandNoclip},
    {"/speed", "Use: /speed [val]", "Sets or queries noclip flight speed",
     CommandSpeed},
    {"/debug", "Use: /debug <debug/help> <1/0>",
     "Activate or deactivate debugs overlay", CommandDebug},
    {"/seed", "Use: /seed", "Returns the world seed", CommandSeed},
    {"/save", "Use: /save", "Saves all modified chunks to disk", CommandSave},
    {"/prefab",
     "Use: /prefab <list | stamp <name> | pos1 | pos2 | offset | save <name>>",
     "Lists/stamps prefabs, sets selection corners/offset, or saves a selection",
     CommandPrefab},
    {"/biome", "Use: /biome",
     "Returns the biome and its raw climate values at your position",
     CommandBiome},
    {"/locate", "Use: /locate <biome | prefab> <name>",
     "Locates the nearest biome or prefab structure", CommandLocate},
    {"/guiscale", "Use: /guiscale <0-4>",
     "Sets the GUI scale; 0 picks the largest that fits the window",
     CommandGuiScale},
    {"/fillchat", "Use: /fillchat [lines]",
     "Fills the chat history with identical test lines (20 by default)",
     CommandFillChat},
#ifdef MINEGAME_PROFILE
    {"/profile", "Use: /profile <start [seconds] | stop | dump | reset>",
     "Streams per-frame timings to CSV, dumps the window, or resets it",
     CommandProfile},
#endif
};

static const int AVAILABLECOMMANDSCOUNT =
    (int)(sizeof(AVAILABLECOMMANDS) / sizeof(AVAILABLECOMMANDS[0]));

static void DebugAABB(CommandContext *Ctx, bool State);
static void DebugWireframe(CommandContext *Ctx, bool State);
static void DebugChunkBorders(CommandContext *Ctx, bool State);
static void DebugFreeCam(CommandContext *Ctx, bool State);
static void DebugForceCutout(CommandContext *Ctx, bool State);
static void DebugSortChunks(CommandContext *Ctx, bool State);
static void DebugOcclusion(CommandContext *Ctx, bool State);

static const DebugToggle AVAILABLEDEBUGS[] = {
    {"aabb", "Show collision boxes", DebugAABB},
    {"wireframe", "Show lines view", DebugWireframe},
    {"chunkborders", "Show chunk borders", DebugChunkBorders},
    {"freecam", "Enable freecam", DebugFreeCam},
    {"forcecutout", "Draw solid blocks with the cutout shader (measurement)",
     DebugForceCutout},
    {"sortchunks", "Draw solid chunks nearest first (on by default)",
     DebugSortChunks},
    {"occlusion", "Skip chunks hidden behind terrain (on by default)",
     DebugOcclusion},
};

static const int AVAILABLEDEBUGSCOUNT =
    (int)(sizeof(AVAILABLEDEBUGS) / sizeof(AVAILABLEDEBUGS[0]));

static char *TokenizeSpace(char **Context) {
  if (Context == (char **)0 || *Context == (char *)0) {
    return (void*)0;
  }
  char *Orig = *Context;
  int StartIndex = 0;
  for (int Index = 0; Index < CHAT_MAX_INPUT_CHARS; Index++) {
    if (Orig[StartIndex] == ' ') {
      StartIndex++;
    } else {
      break;
    }
  }

  if (Orig[StartIndex] == '\0') {
    *Context = (char *)0;
    return (void*)0;
  }
  int EndIndex = StartIndex;
  for (int Index = 0; Index < CHAT_MAX_INPUT_CHARS; Index++) {
    char CharVal = Orig[EndIndex];
    if (CharVal != '\0' && CharVal != ' ') {
      EndIndex++;
    } else {
      break;
    }
  }

  char *Token = Orig + StartIndex;
  if (Orig[EndIndex] == ' ') {
    Orig[EndIndex] = '\0';
    *Context = Orig + EndIndex + 1;
  } else {
    *Context = (char *)0;
  }
  return Token;
}

static char *GetRemainingString(char **Context) {
  if (Context == (char **)0 || *Context == (char *)0) {
    return (void*)0;
  }
  char *Start = *Context;
  if (*Start == '\0') {
    *Context = (char *)0;
    return (void*)0;
  }
  *Context = (char *)0;
  return Start;
}

static CommandOutputSink OutputSink = (CommandOutputSink)0;

void SetCommandOutputSink(CommandOutputSink Sink) { OutputSink = Sink; }

static void ReturnCommand(ChatState *Chat, int LogLevel, const char *Message) {
  AddChatHistory(Chat, Message);
  if (OutputSink != (CommandOutputSink)0) {
    OutputSink(Message);
  }
  switch (LogLevel) {
  case LOG_LEVEL_WARN:
    LogWarn("%s", Message);
    break;
  case LOG_LEVEL_ERROR:
    LogError("%s", Message);
    break;
  default:
    LogInfo("%s", Message);
    break;
  }
}

// A coordinate is absolute ("12.5") or, with a leading '~', relative to the
// player's current value ("~", "~40", "~-3"). Returns false on anything else.
static bool ParseTeleportCoordinate(const char *Str, float Current,
                                    float *Out) {
  bool Relative = (Str[0] == '~');
  const char *Number = Relative ? Str + 1 : Str;
  if (Relative && *Number == '\0') {
    *Out = Current;
    return true;
  }

  char *End = (void *)0;
  float Value = ParseFloat(Number, &End);
  if (End == Number || *End != '\0') {
    return false;
  }
  *Out = Relative ? Current + Value : Value;
  return true;
}

static void CommandTP(const char *Args, CommandContext *Ctx) {
  if (Args == (void*)0) {
    ReturnCommand(Ctx->Chat, LOG_LEVEL_ERROR,
                  "Incorrect Format. try: /tp <x> <y> <z>");
    return;
  }

  char ArgsCopy[CHAT_MAX_INPUT_CHARS];
  SafeStrncpy(ArgsCopy, Args, (int)sizeof(ArgsCopy));

  char *SavePtr = ArgsCopy;
  char *XStr = TokenizeSpace(&SavePtr);
  char *YStr = TokenizeSpace(&SavePtr);
  char *ZStr = TokenizeSpace(&SavePtr);

  if (XStr && YStr && ZStr) {
    Vec3 Current = Ctx->Player->Position;
    float ValX = 0.0F;
    float ValY = 0.0F;
    float ValZ = 0.0F;

    if (!ParseTeleportCoordinate(XStr, Current.x, &ValX) ||
        !ParseTeleportCoordinate(YStr, Current.y, &ValY) ||
        !ParseTeleportCoordinate(ZStr, Current.z, &ValZ)) {
      ReturnCommand(Ctx->Chat, LOG_LEVEL_ERROR,
                    "Invalid coordinates. Must be numeric values.");
      return;
    }

    SetPlayerPosition(Ctx->Player, (Vec3){ValX, ValY, ValZ});
    Ctx->Player->Velocity = (Vec3){0.0F, 0.0F, 0.0F};

    char Msg[MSG_BUFFER_SIZE];
    snprintf(Msg, sizeof(Msg), "Tp to X:%.1f Y:%.1f Z:%.1f", (double)ValX,
             (double)ValY, (double)ValZ);
    ReturnCommand(Ctx->Chat, LOG_LEVEL_INFO, Msg);

  } else {
    ReturnCommand(Ctx->Chat, LOG_LEVEL_ERROR,
                  "Incorrect Format. try: /tp <x> <y> <z>");
  }
}

static void CommandHelp(const char *Args, CommandContext *Ctx) {
  (void)Args;

  for (int Index = 0; Index < AVAILABLECOMMANDSCOUNT; Index++) {
    char Msg[MSG_BUFFER_SIZE];
    snprintf(Msg, sizeof(Msg), "%s | %s | %s", AVAILABLECOMMANDS[Index].Name,
             AVAILABLECOMMANDS[Index].Use, AVAILABLECOMMANDS[Index].Description);
    AddChatHistory(Ctx->Chat, Msg);
  }
}

static void CommandPos(const char *Args, CommandContext *Ctx) {
  (void)Args;

  float PosX = Ctx->Player->Position.x;
  float PosY = Ctx->Player->Position.y;
  float PosZ = Ctx->Player->Position.z;

  char Msg[MSG_BUFFER_SIZE];
  snprintf(Msg, sizeof(Msg), "X:%.1f Y:%.1f Z:%.1f", (double)PosX, (double)PosY,
           (double)PosZ);
  ReturnCommand(Ctx->Chat, LOG_LEVEL_INFO, Msg);
}

#ifdef MINEGAME_PROFILE
// A capture longer than this is almost certainly a typo in the duration.
#define PROFILE_MAX_TIMED_SECONDS 600.0F

static void ProfileDump(CommandContext *Ctx) {
  char Path[PROFILER_DUMP_PATH_CAPACITY];
  char Msg[MSG_BUFFER_SIZE];

  switch (ProfilerDumpCsv(Path, sizeof(Path))) {
  case PROFILER_DUMP_OK:
    snprintf(Msg, sizeof(Msg), "Profile written to %s", Path);
    ReturnCommand(Ctx->Chat, LOG_LEVEL_INFO, Msg);
    break;
  case PROFILER_DUMP_NO_SAMPLES:
    ReturnCommand(Ctx->Chat, LOG_LEVEL_WARN,
                  "No frames recorded yet, nothing to export");
    break;
  case PROFILER_DUMP_WRITE_FAILED:
  default:
    ReturnCommand(Ctx->Chat, LOG_LEVEL_ERROR, "Could not write the profile file");
    break;
  }
}

// Kept so the message announcing that a timed capture finished can name the
// file it went to.
static char *GetLastCapturePath(void) {
  static char Path[PROFILER_DUMP_PATH_CAPACITY];
  return Path;
}

static void ProfileStart(CommandContext *Ctx, const char *DurationArg) {
  char *Path = GetLastCapturePath();
  char Msg[MSG_BUFFER_SIZE];

  if (ProfilerIsCapturing()) {
    ReturnCommand(Ctx->Chat, LOG_LEVEL_WARN,
                  "Already capturing. Use /profile stop first");
    return;
  }

  if (DurationArg == (void *)0) {
    if (!ProfilerStartCapture(Path, PROFILER_DUMP_PATH_CAPACITY)) {
      ReturnCommand(Ctx->Chat, LOG_LEVEL_ERROR, "Could not start the capture");
      return;
    }
    snprintf(Msg, sizeof(Msg),
             "Capturing every frame to %s. /profile stop to end", Path);
    ReturnCommand(Ctx->Chat, LOG_LEVEL_INFO, Msg);
    return;
  }

  char *End = (char *)0;
  float Seconds = ParseFloat(DurationArg, &End);
  if (End == DurationArg || Seconds <= 0.0F ||
      Seconds > PROFILE_MAX_TIMED_SECONDS) {
    snprintf(Msg, sizeof(Msg),
             "Duration must be a number of seconds between 0 and %d",
             (int)PROFILE_MAX_TIMED_SECONDS);
    ReturnCommand(Ctx->Chat, LOG_LEVEL_ERROR, Msg);
    return;
  }

  if (!ProfilerStartTimedCapture(Path, PROFILER_DUMP_PATH_CAPACITY,
                                 (double)Seconds)) {
    ReturnCommand(Ctx->Chat, LOG_LEVEL_ERROR, "Could not start the capture");
    return;
  }
  snprintf(Msg, sizeof(Msg), "Capturing %.1f s to %s", (double)Seconds, Path);
  ReturnCommand(Ctx->Chat, LOG_LEVEL_INFO, Msg);
}

static void ProfileStop(CommandContext *Ctx) {
  if (!ProfilerIsCapturing()) {
    ReturnCommand(Ctx->Chat, LOG_LEVEL_WARN, "Not capturing");
    return;
  }
  ProfilerStopCapture();
  ReturnCommand(Ctx->Chat, LOG_LEVEL_INFO, "Capture stopped and written");
}

static void CommandProfile(const char *Args, CommandContext *Ctx) {
  if (Args == (void *)0) {
    ReturnCommand(
        Ctx->Chat, LOG_LEVEL_ERROR,
        "Incorrect Format. try: /profile <start [seconds] | stop | dump | reset>");
    return;
  }

  char ArgsCopy[CHAT_MAX_INPUT_CHARS];
  SafeStrncpy(ArgsCopy, Args, (int)sizeof(ArgsCopy));

  char *SavePtr = ArgsCopy;
  char *SubCmd = TokenizeSpace(&SavePtr);

  if (SubCmd != (void *)0 && CompareString(SubCmd, "dump") == 0) {
    ProfileDump(Ctx);
    return;
  }

  if (SubCmd != (void *)0 && CompareString(SubCmd, "start") == 0) {
    ProfileStart(Ctx, TokenizeSpace(&SavePtr));
    return;
  }

  if (SubCmd != (void *)0 && CompareString(SubCmd, "stop") == 0) {
    ProfileStop(Ctx);
    return;
  }

  if (SubCmd != (void *)0 && CompareString(SubCmd, "reset") == 0) {
    ProfilerResetWindow();
    ReturnCommand(Ctx->Chat, LOG_LEVEL_INFO, "Profiler window reset");
    return;
  }

  ReturnCommand(Ctx->Chat, LOG_LEVEL_ERROR,
                "Incorrect Format. try: /profile <start | stop | dump | reset>");
}
#endif

static void CommandSeed(const char *Args, CommandContext *Ctx) {
  (void)Args;
  char Msg[MSG_BUFFER_SIZE];
  snprintf(Msg, sizeof(Msg), "World Seed: %llu",
           (unsigned long long)GetWorldSeed());
  ReturnCommand(Ctx->Chat, LOG_LEVEL_INFO, Msg);
}

static void CommandBiome(const char *Args, CommandContext *Ctx) {
  (void)Args;

  int PosX = (int)__builtin_floorf(Ctx->Player->Position.x);
  int PosY = (int)__builtin_floorf(Ctx->Player->Position.y);
  int PosZ = (int)__builtin_floorf(Ctx->Player->Position.z);

  uint64_t SeedVal = GetWorldSeed();
  BiomeClimate Climate;
  int BiomeId = SampleBiomeAt(PosX, PosY, PosZ, SeedVal, &Climate);
  const BiomeType *Biome = GetBiomeDef(BiomeId);
  const float *Axes = Climate.Axes;

  char Msg[MSG_BUFFER_SIZE];
  snprintf(Msg, sizeof(Msg),
           "Biome: %s [%d] | T:%.3f H:%.3f C:%.3f E:%.3f W:%.3f PV:%.3f "
           "D:%.0f",
           (Biome->Id < 0) ? "(none loaded)" : Biome->Name, BiomeId,
           (double)Axes[CLIMATE_TEMPERATURE], (double)Axes[CLIMATE_HUMIDITY],
           (double)Axes[CLIMATE_CONTINENTALNESS], (double)Axes[CLIMATE_EROSION],
           (double)Axes[CLIMATE_WEIRDNESS], (double)Axes[CLIMATE_PEAKS_VALLEYS],
           (double)Climate.Depth);
  ReturnCommand(Ctx->Chat, (Biome->Id < 0) ? LOG_LEVEL_WARN : LOG_LEVEL_INFO,
                Msg);
}

static int FindBiomeByMatch(const char *Name) {
  int BiomeTotal = GetLoadedBiomesCount();
  for (int Idx = 0; Idx < BiomeTotal; Idx++) {
    const BiomeType *B = GetBiomeDef(Idx);
    if (B != (void *)0 && B->Name[0] != '\0' && NameMatchesExact(B->Name, Name)) {
      return Idx;
    }
  }
  for (int Idx = 0; Idx < BiomeTotal; Idx++) {
    const BiomeType *B = GetBiomeDef(Idx);
    if (B != (void *)0 && B->Name[0] != '\0' && NameMatchesPrefix(B->Name, Name)) {
      return Idx;
    }
  }
  return -1;
}

static int FindPrefabByMatch(const char *Name) {
  int PrefabTotal = GetLoadedPrefabsCount();
  for (int Idx = 0; Idx < PrefabTotal; Idx++) {
    const Prefab *P = GetPrefabByIndex(Idx);
    if (P != (void *)0 && P->Name[0] != '\0' && NameMatchesExact(P->Name, Name)) {
      return Idx;
    }
  }
  for (int Idx = 0; Idx < PrefabTotal; Idx++) {
    const Prefab *P = GetPrefabByIndex(Idx);
    if (P != (void *)0 && P->Name[0] != '\0' && NameMatchesPrefix(P->Name, Name)) {
      return Idx;
    }
  }
  return -1;
}

static void CommandLocate(const char *Args, CommandContext *Ctx) {
  if (Args == (void *)0) {
    ReturnCommand(Ctx->Chat, LOG_LEVEL_ERROR,
                  "Use: /locate <biome | prefab> <name>");
    return;
  }

  char ArgsCopy[CHAT_MAX_INPUT_CHARS];
  SafeStrncpy(ArgsCopy, Args, (int)sizeof(ArgsCopy));

  char *SavePtr = ArgsCopy;
  char *Subcmd = TokenizeSpace(&SavePtr);
  if (Subcmd == (void *)0 || SavePtr == (void *)0) {
    ReturnCommand(Ctx->Chat, LOG_LEVEL_ERROR,
                  "Use: /locate <biome | prefab> <name>");
    return;
  }

  while (*SavePtr == ' ') {
    SavePtr++;
  }
  if (*SavePtr == '"') {
    SavePtr++;
    char *EndQuote = strrchr(SavePtr, '"');
    if (EndQuote != (void *)0) {
      *EndQuote = '\0';
    }
  }
  int NameLen = (int)strlen(SavePtr);
  while (NameLen > 0 && SavePtr[NameLen - 1] == ' ') {
    SavePtr[--NameLen] = '\0';
  }

  if (SavePtr[0] == '\0') {
    ReturnCommand(Ctx->Chat, LOG_LEVEL_ERROR,
                  "Missing name. Use: /locate <biome | prefab> <name>");
    return;
  }

  int PlayerX = (int)__builtin_floorf(Ctx->Player->Position.x);
  int PlayerZ = (int)__builtin_floorf(Ctx->Player->Position.z);
  uint64_t Seed = GetWorldSeed();
  const int MAX_SEARCH_RADIUS = 3000;

  if (CompareString(Subcmd, "biome") == 0) {
    int BiomeId = FindBiomeByMatch(SavePtr);
    if (BiomeId < 0) {
      char Msg[MSG_BUFFER_SIZE];
      snprintf(Msg, sizeof(Msg),
               "Unknown biome '%s'. Use Tab for suggestions.", SavePtr);
      ReturnCommand(Ctx->Chat, LOG_LEVEL_WARN, Msg);
      return;
    }
    const BiomeType *Def = GetBiomeDef(BiomeId);
    int FoundX = 0;
    int FoundZ = 0;
    float Distance = 0.0F;
    if (FindBiomeLocation(BiomeId, PlayerX, PlayerZ, MAX_SEARCH_RADIUS, Seed,
                          &FoundX, &FoundZ, &Distance)) {
      char Msg[MSG_BUFFER_SIZE];
      snprintf(Msg, sizeof(Msg),
               "Nearest %s is at [%d, ~, %d] (%.1f blocks away)",
               Def->Name, FoundX, FoundZ, (double)Distance);
      ReturnCommand(Ctx->Chat, LOG_LEVEL_INFO, Msg);
    } else {
      char Msg[MSG_BUFFER_SIZE];
      snprintf(Msg, sizeof(Msg),
               "Could not find biome '%s' within %d blocks.", Def->Name,
               MAX_SEARCH_RADIUS);
      ReturnCommand(Ctx->Chat, LOG_LEVEL_WARN, Msg);
    }
  } else if (CompareString(Subcmd, "prefab") == 0 ||
             CompareString(Subcmd, "structure") == 0) {
    int PrefabIdx = FindPrefabByMatch(SavePtr);
    if (PrefabIdx < 0) {
      char Msg[MSG_BUFFER_SIZE];
      snprintf(Msg, sizeof(Msg),
               "Unknown prefab '%s'. Use Tab for suggestions.", SavePtr);
      ReturnCommand(Ctx->Chat, LOG_LEVEL_WARN, Msg);
      return;
    }
    const Prefab *P = GetPrefabByIndex(PrefabIdx);
    int FoundX = 0;
    int FoundY = 0;
    int FoundZ = 0;
    float Distance = 0.0F;
    if (FindStructureByName(P->Name, PlayerX, PlayerZ, MAX_SEARCH_RADIUS,
                            Seed, &FoundX, &FoundY, &FoundZ, &Distance)) {
      char Msg[MSG_BUFFER_SIZE];
      snprintf(Msg, sizeof(Msg),
               "Nearest %s is at [%d, %d, %d] (%.1f blocks away)",
               P->Name, FoundX, FoundY, FoundZ, (double)Distance);
      ReturnCommand(Ctx->Chat, LOG_LEVEL_INFO, Msg);
    } else {
      bool Spawns = false;
      int BiomeCount = GetLoadedBiomesCount();
      for (int BIdx = 0; BIdx < BiomeCount; BIdx++) {
        const BiomeType *B = GetBiomeDef(BIdx);
        if (B == (void *)0) {
          continue;
        }
        for (int SIdx = 0; SIdx < B->StructureCount; SIdx++) {
          if (B->Structures[SIdx].PrefabIndex == PrefabIdx) {
            Spawns = true;
            break;
          }
        }
        if (Spawns) {
          break;
        }
      }
      char Msg[MSG_BUFFER_SIZE];
      if (!Spawns) {
        snprintf(Msg, sizeof(Msg),
                 "Prefab '%s' is not configured to spawn naturally in any biome.",
                 P->Name);
      } else {
        snprintf(Msg, sizeof(Msg),
                 "Could not find prefab '%s' within %d blocks.", P->Name,
                 MAX_SEARCH_RADIUS);
      }
      ReturnCommand(Ctx->Chat, LOG_LEVEL_WARN, Msg);
    }
  } else {
    char Msg[MSG_BUFFER_SIZE];
    snprintf(Msg, sizeof(Msg),
             "Invalid locate category '%s'. Use: /locate <biome | prefab> <name>",
             Subcmd);
    ReturnCommand(Ctx->Chat, LOG_LEVEL_ERROR, Msg);
  }
}

static void CommandGuiScale(const char *Args, CommandContext *Ctx) {
  char *End = (char *)0;
  int Setting = ParseInt(Args, &End);
  // ParseInt leaves End just past the last digit, so a preceding digit is the
  // only proof that a number was actually read.
  bool HasNumber = (End != (char *)0) && (End != Args) && (End[-1] >= '0') &&
                   (End[-1] <= '9');
  if (!HasNumber || Setting < UI_GUI_SCALE_AUTO || Setting > UI_GUI_SCALE_MAX) {
    ReturnCommand(Ctx->Chat, LOG_LEVEL_ERROR,
                  "Incorrect Format. try: /guiscale <0-4>");
    return;
  }

  UiSetGuiScaleSetting(Setting);
  char Msg[MSG_BUFFER_SIZE];
  if (Setting == UI_GUI_SCALE_AUTO) {
    snprintf(Msg, sizeof(Msg), "GUI scale set to auto");
  } else {
    snprintf(Msg, sizeof(Msg), "GUI scale set to %d", Setting);
  }
  ReturnCommand(Ctx->Chat, LOG_LEVEL_INFO, Msg);
}

// Identical lines, accents included, so chat-cost captures always draw the
// same glyphs from the same sheets.
static void CommandFillChat(const char *Args, CommandContext *Ctx) {
  int Lines = FILLCHAT_DEFAULT_LINES;
  if (Args != (void *)0) {
    char *End = (char *)0;
    Lines = ParseInt(Args, &End);
    bool HasNumber = (End != (char *)0) && (End != Args) &&
                     (End[-1] >= '0') && (End[-1] <= '9');
    if (!HasNumber || Lines < 1 || Lines > CHAT_MAX_HISTORY) {
      ReturnCommand(Ctx->Chat, LOG_LEVEL_ERROR,
                    "Incorrect Format. try: /fillchat [1-256]");
      return;
    }
  }

  char Line[MSG_BUFFER_SIZE];
  for (int Idx = 0; Idx < Lines; Idx++) {
    snprintf(Line, sizeof(Line),
             "Fill %03d: The quick brown fox jumps over the lazy dog. "
             "A\xc3\xa7\xc3\xa3o, n\xc3\xa3o, \xc3\xa9 poss\xc3\xadvel.",
             Idx + 1);
    AddChatHistory(Ctx->Chat, Line);
  }
}

static void CommandSave(const char *Args, CommandContext *Ctx) {
  (void)Args;
  if (WorldSaveIsReadOnly()) {
    ReturnCommand(Ctx->Chat, LOG_LEVEL_WARN,
                  "World is read-only in this run; nothing was saved");
    return;
  }
  int SavedCount = 0;
  for (int Index = 0; Index < Ctx->World->ChunkCount; Index++) {
    Chunk *CurrentChunk = &Ctx->World->Chunks[Index];
    if (Ctx->World->SlotInUse[Index] && CurrentChunk->IsModified) {
      SaveChunkToDisk(CurrentChunk);
      SavedCount++;
    }
  }
  char Msg[MSG_BUFFER_SIZE];
  snprintf(Msg, sizeof(Msg), "World saved successfully! (%d chunks saved)",
           SavedCount);
  ReturnCommand(Ctx->Chat, LOG_LEVEL_INFO, Msg);
}

static void PrefabList(CommandContext *Ctx) {
  int Count = GetLoadedPrefabsCount();
  if (Count == 0) {
    ReturnCommand(Ctx->Chat, LOG_LEVEL_WARN, "No prefab loaded");
    return;
  }

  for (int Index = 0; Index < Count; Index++) {
    const Prefab *Entry = GetPrefabByIndex(Index);
    if (Entry == (void*)0) {
      continue;
    }
    char Msg[MSG_BUFFER_SIZE];
    snprintf(Msg, sizeof(Msg), "%s (%dx%dx%d, %d cells)", Entry->Name,
             Entry->SizeX, Entry->SizeY, Entry->SizeZ, Entry->CellCount);
    AddChatHistory(Ctx->Chat, Msg);
  }

  char Msg[MSG_BUFFER_SIZE];
  snprintf(Msg, sizeof(Msg), "Listed %d prefabs", Count);
  ReturnCommand(Ctx->Chat, LOG_LEVEL_INFO, Msg);
}

static void PrefabStamp(const char *Name, CommandContext *Ctx) {
  if (Name == (void*)0) {
    ReturnCommand(Ctx->Chat, LOG_LEVEL_ERROR,
                  "Missing prefab name. try: /prefab stamp <name>");
    return;
  }

  const Prefab *Entry = GetPrefab(Name);
  if (Entry == (void*)0) {
    char Msg[MSG_BUFFER_SIZE];
    snprintf(Msg, sizeof(Msg), "Prefab not found: %s", Name);
    ReturnCommand(Ctx->Chat, LOG_LEVEL_ERROR, Msg);
    return;
  }

  RaycastResult Target = Ctx->Player->TargetBlock;
  if (Target.Hit == false) {
    ReturnCommand(Ctx->Chat, LOG_LEVEL_WARN, "No block targeted");
    return;
  }

  // Placement cell: the block adjacent to the targeted face.
  Vec3 Placement = {Target.BlockPos.x + Target.Normal.x,
                    Target.BlockPos.y + Target.Normal.y,
                    Target.BlockPos.z + Target.Normal.z};

  Vec3 Origin;
  if (Entry->HasOrigin) {
    // Anchor the prefab so its origin cell lands on the placement cell.
    Origin = (Vec3){Placement.x - (float)Entry->OriginX,
                    Placement.y - (float)Entry->OriginY,
                    Placement.z - (float)Entry->OriginZ};
  } else {
    // Default: center horizontally on the target and rest the base on the face.
    int HalfX = Entry->SizeX / 2;
    int HalfZ = Entry->SizeZ / 2;
    Origin = (Vec3){Placement.x - (float)HalfX, Placement.y,
                    Placement.z - (float)HalfZ};
  }
  StampPrefab(Ctx->World, Entry, Origin);

  char Msg[MSG_BUFFER_SIZE];
  snprintf(Msg, sizeof(Msg), "Stamped '%s' at X:%.0f Y:%.0f Z:%.0f", Entry->Name,
           (double)Origin.x, (double)Origin.y, (double)Origin.z);
  ReturnCommand(Ctx->Chat, LOG_LEVEL_INFO, Msg);
}

// True when both corners are set and Point falls inside the inclusive selection
// box.
static bool IsPointInSelection(const Player *PlayerVal, Vec3 Point) {
  if (PlayerVal->HasSelectionA == false || PlayerVal->HasSelectionB == false) {
    return false;
  }
  Vec3 CornerA = PlayerVal->SelectionA;
  Vec3 CornerB = PlayerVal->SelectionB;
  float MinX = CornerA.x < CornerB.x ? CornerA.x : CornerB.x;
  float MinY = CornerA.y < CornerB.y ? CornerA.y : CornerB.y;
  float MinZ = CornerA.z < CornerB.z ? CornerA.z : CornerB.z;
  float MaxX = CornerA.x > CornerB.x ? CornerA.x : CornerB.x;
  float MaxY = CornerA.y > CornerB.y ? CornerA.y : CornerB.y;
  float MaxZ = CornerA.z > CornerB.z ? CornerA.z : CornerB.z;
  return Point.x >= MinX && Point.x <= MaxX && Point.y >= MinY &&
         Point.y <= MaxY && Point.z >= MinZ && Point.z <= MaxZ;
}

static void PrefabSetCorner(CommandContext *Ctx, bool IsFirst) {
  RaycastResult Target = Ctx->Player->TargetBlock;
  if (Target.Hit == false) {
    ReturnCommand(Ctx->Chat, LOG_LEVEL_WARN, "No block targeted");
    return;
  }

  if (IsFirst) {
    Ctx->Player->SelectionA = Target.BlockPos;
    Ctx->Player->HasSelectionA = true;
  } else {
    Ctx->Player->SelectionB = Target.BlockPos;
    Ctx->Player->HasSelectionB = true;
  }

  // Drop a stale anchor that no longer lies within the redefined selection.
  if (Ctx->Player->HasSelectionOffset &&
      IsPointInSelection(Ctx->Player, Ctx->Player->SelectionOffset) == false) {
    Ctx->Player->HasSelectionOffset = false;
  }

  char Msg[MSG_BUFFER_SIZE];
  snprintf(Msg, sizeof(Msg), "Set pos%d to X:%.0f Y:%.0f Z:%.0f",
           IsFirst ? 1 : 2, (double)Target.BlockPos.x, (double)Target.BlockPos.y,
           (double)Target.BlockPos.z);
  ReturnCommand(Ctx->Chat, LOG_LEVEL_INFO, Msg);
}

static void PrefabOffset(CommandContext *Ctx) {
  if (Ctx->Player->HasSelectionA == false ||
      Ctx->Player->HasSelectionB == false) {
    ReturnCommand(Ctx->Chat, LOG_LEVEL_WARN,
                  "Selection incomplete. Set pos1 and pos2 first");
    return;
  }

  RaycastResult Target = Ctx->Player->TargetBlock;
  if (Target.Hit == false) {
    ReturnCommand(Ctx->Chat, LOG_LEVEL_WARN, "No block targeted");
    return;
  }

  if (IsPointInSelection(Ctx->Player, Target.BlockPos) == false) {
    ReturnCommand(Ctx->Chat, LOG_LEVEL_WARN,
                  "Target is outside the selection box");
    return;
  }

  Ctx->Player->SelectionOffset = Target.BlockPos;
  Ctx->Player->HasSelectionOffset = true;

  char Msg[MSG_BUFFER_SIZE];
  snprintf(Msg, sizeof(Msg), "Set offset to X:%.0f Y:%.0f Z:%.0f",
           (double)Target.BlockPos.x, (double)Target.BlockPos.y,
           (double)Target.BlockPos.z);
  ReturnCommand(Ctx->Chat, LOG_LEVEL_INFO, Msg);
}

static void PrefabSave(const char *Name, CommandContext *Ctx) {
  if (Name == (void*)0) {
    ReturnCommand(Ctx->Chat, LOG_LEVEL_ERROR,
                  "Missing prefab name. try: /prefab save <name>");
    return;
  }

  if (Ctx->Player->HasSelectionA == false ||
      Ctx->Player->HasSelectionB == false) {
    ReturnCommand(Ctx->Chat, LOG_LEVEL_WARN,
                  "Selection incomplete. Set pos1 and pos2 first");
    return;
  }

  char ErrorBuf[MSG_BUFFER_SIZE];
  ErrorBuf[0] = '\0';
  const Vec3 *Offset =
      Ctx->Player->HasSelectionOffset ? &Ctx->Player->SelectionOffset : NULL;
  bool Ok = CaptureSelectionToPrefab(Ctx->World, Ctx->Player->SelectionA,
                                     Ctx->Player->SelectionB, Offset, Name,
                                     ErrorBuf, (int)sizeof(ErrorBuf));
  if (Ok == false) {
    char Msg[MSG_BUFFER_SIZE];
    snprintf(Msg, sizeof(Msg), "Save failed: %s", ErrorBuf);
    ReturnCommand(Ctx->Chat, LOG_LEVEL_ERROR, Msg);
    return;
  }

  char Msg[MSG_BUFFER_SIZE];
  snprintf(Msg, sizeof(Msg), "Saved prefab '%s'", Name);
  ReturnCommand(Ctx->Chat, LOG_LEVEL_INFO, Msg);
}

static void CommandPrefab(const char *Args, CommandContext *Ctx) {
  if (Args == (void*)0) {
    ReturnCommand(
        Ctx->Chat, LOG_LEVEL_ERROR,
        "Incorrect Format. try: /prefab <list | stamp <name> | pos1 | pos2 | "
        "offset | save <name>>");
    return;
  }

  char ArgsCopy[CHAT_MAX_INPUT_CHARS];
  SafeStrncpy(ArgsCopy, Args, (int)sizeof(ArgsCopy));

  char *SavePtr = ArgsCopy;
  char *SubCmd = TokenizeSpace(&SavePtr);

  if (SubCmd != (void*)0 && CompareString(SubCmd, "list") == 0) {
    PrefabList(Ctx);
    return;
  }

  if (SubCmd != (void*)0 && CompareString(SubCmd, "stamp") == 0) {
    char *Name = TokenizeSpace(&SavePtr);
    PrefabStamp(Name, Ctx);
    return;
  }

  if (SubCmd != (void*)0 && CompareString(SubCmd, "pos1") == 0) {
    PrefabSetCorner(Ctx, true);
    return;
  }

  if (SubCmd != (void*)0 && CompareString(SubCmd, "pos2") == 0) {
    PrefabSetCorner(Ctx, false);
    return;
  }

  if (SubCmd != (void*)0 && CompareString(SubCmd, "offset") == 0) {
    PrefabOffset(Ctx);
    return;
  }

  if (SubCmd != (void*)0 && CompareString(SubCmd, "save") == 0) {
    char *Name = TokenizeSpace(&SavePtr);
    PrefabSave(Name, Ctx);
    return;
  }

  ReturnCommand(
      Ctx->Chat, LOG_LEVEL_ERROR,
      "Incorrect Format. try: /prefab <list | stamp <name> | pos1 | pos2 | "
      "save <name>>");
}

static void CommandList(const char *Args, CommandContext *Ctx) {
  (void)Args;

  if (Ctx->BlockRegistry == (void*)0 || Ctx->BlockCount == 0) {
    ReturnCommand(Ctx->Chat, LOG_LEVEL_WARN, "No block asset loaded");
    return;
  }

  int Printed = 0;
  for (int Index = 0; Index < BLOCK_REGISTRY_SIZE; Index++) {
    if (Ctx->BlockRegistry[Index].Id != -1) {
      char Msg[MSG_BUFFER_SIZE];
      snprintf(Msg, sizeof(Msg), "[ID: %d] %s", Ctx->BlockRegistry[Index].Id,
               Ctx->BlockRegistry[Index].Name);
      AddChatHistory(Ctx->Chat, Msg);
      Printed++;
    }
  }
  char Msg[MSG_BUFFER_SIZE];
  snprintf(Msg, sizeof(Msg), "Listed %d blocks", Printed);
  ReturnCommand(Ctx->Chat, LOG_LEVEL_INFO, Msg);
}

static void CommandNoclip(const char *Args, CommandContext *Ctx) {
  if (Args == (void*)0) {
    ReturnCommand(Ctx->Chat, LOG_LEVEL_ERROR,
                  "Incorrect Format. try: /noclip <1/0 | speed <val>>");
    return;
  }

  char ArgsCopy[CHAT_MAX_INPUT_CHARS];
  SafeStrncpy(ArgsCopy, Args, (int)sizeof(ArgsCopy));

  char *SavePtr = ArgsCopy;
  char *OptStr = TokenizeSpace(&SavePtr);
  char *SpeedStr = TokenizeSpace(&SavePtr);

  if (OptStr) {
    if (CompareString(OptStr, "speed") == 0) {
      if (SpeedStr != (void*)0) {
        float SpeedVal = ParseFloat(SpeedStr, (char **)0);
        if (SpeedVal > 0.0F) {
          Ctx->Player->NoclipSpeed = SpeedVal;
          char Msg[MSG_BUFFER_SIZE];
          snprintf(Msg, sizeof(Msg), "Noclip speed set to %.1f", (double)SpeedVal);
          ReturnCommand(Ctx->Chat, LOG_LEVEL_INFO, Msg);
          return;
        }
      }
      char Msg[MSG_BUFFER_SIZE];
      snprintf(Msg, sizeof(Msg), "Current noclip speed: %.1f",
               (double)Ctx->Player->NoclipSpeed);
      ReturnCommand(Ctx->Chat, LOG_LEVEL_INFO, Msg);
      return;
    }

    int Opt = ParseInt(OptStr, (char **)0);
    if (Opt == 1) {
      Ctx->Player->Noclip = true;
      if (SpeedStr != (void*)0) {
        float SpeedVal = ParseFloat(SpeedStr, (char **)0);
        if (SpeedVal > 0.0F) {
          Ctx->Player->NoclipSpeed = SpeedVal;
        }
      } else {
        Ctx->Player->NoclipSpeed = PLAYER_DEFAULT_NOCLIP_SPEED;
      }
      char Msg[MSG_BUFFER_SIZE];
      snprintf(Msg, sizeof(Msg), "Noclip activated (speed: %.1f)",
               (double)Ctx->Player->NoclipSpeed);
      ReturnCommand(Ctx->Chat, LOG_LEVEL_INFO, Msg);
    } else {
      Ctx->Player->Noclip = false;
      ReturnCommand(Ctx->Chat, LOG_LEVEL_INFO, "Noclip deactivated");
    }
  } else {
    ReturnCommand(Ctx->Chat, LOG_LEVEL_ERROR,
                  "Incorrect Format. try: /noclip <1/0 | speed <val>>");
  }
}

static void CommandSpeed(const char *Args, CommandContext *Ctx) {
  if (Args == (void*)0) {
    char Msg[MSG_BUFFER_SIZE];
    snprintf(Msg, sizeof(Msg), "Current noclip speed: %.1f",
             (double)Ctx->Player->NoclipSpeed);
    ReturnCommand(Ctx->Chat, LOG_LEVEL_INFO, Msg);
    return;
  }

  char ArgsCopy[CHAT_MAX_INPUT_CHARS];
  SafeStrncpy(ArgsCopy, Args, (int)sizeof(ArgsCopy));

  char *SavePtr = ArgsCopy;
  char *SpeedStr = TokenizeSpace(&SavePtr);

  if (SpeedStr != (void*)0) {
    float SpeedVal = ParseFloat(SpeedStr, (char **)0);
    if (SpeedVal > 0.0F) {
      Ctx->Player->NoclipSpeed = SpeedVal;
      char Msg[MSG_BUFFER_SIZE];
      snprintf(Msg, sizeof(Msg), "Noclip speed set to %.1f", (double)SpeedVal);
      ReturnCommand(Ctx->Chat, LOG_LEVEL_INFO, Msg);
      return;
    }
  }
  ReturnCommand(Ctx->Chat, LOG_LEVEL_ERROR,
                "Incorrect Format. try: /speed <val>");
}

static void DebugAABB(CommandContext *Ctx, bool State) {
  (void)Ctx;
  GetDebugState()->Aabb = State;
}

static void DebugWireframe(CommandContext *Ctx, bool State) {
  (void)Ctx;
  GetDebugState()->Wireframe = State;
}

static void DebugChunkBorders(CommandContext *Ctx, bool State) {
  (void)Ctx;
  GetDebugState()->ChunkBorders = State;
}

static void DebugFreeCam(CommandContext *Ctx, bool State) {
  (void)Ctx;
  GetDebugState()->Freecam = State;
}

static void DebugForceCutout(CommandContext *Ctx, bool State) {
  (void)Ctx;
  GetDebugState()->ForceCutoutShader = State;
}

static void DebugSortChunks(CommandContext *Ctx, bool State) {
  (void)Ctx;
  GetDebugState()->SortChunks = State;
}

static void DebugOcclusion(CommandContext *Ctx, bool State) {
  (void)Ctx;
  GetDebugState()->Occlusion = State;
}

static void CommandDebug(const char *Args, CommandContext *Ctx) {
  if (Args == (void*)0) {
    ReturnCommand(
        Ctx->Chat, LOG_LEVEL_ERROR,
        "Incorrect Format. try: /debug <command> <1/0> or /debug help");
    return;
  }

  char ArgsCopy[CHAT_MAX_INPUT_CHARS];
  SafeStrncpy(ArgsCopy, Args, (int)sizeof(ArgsCopy));

  char *SavePtr = ArgsCopy;
  char *DebugStr = TokenizeSpace(&SavePtr);
  char *OptStr = TokenizeSpace(&SavePtr);

  if (DebugStr == (void*)0) {
    ReturnCommand(
        Ctx->Chat, LOG_LEVEL_ERROR,
        "Incorrect Format. try: /debug <command> <1/0> or /debug help");
    return;
  }

  if (CompareString(DebugStr, "help") == 0) {
    ReturnCommand(Ctx->Chat, LOG_LEVEL_INFO, "===DEBUG COMMANDS===");
    for (int Index = 0; Index < AVAILABLEDEBUGSCOUNT; Index++) {
      char Msg[MSG_BUFFER_SIZE];
      snprintf(Msg, sizeof(Msg), "/debug %s %s", AVAILABLEDEBUGS[Index].Name,
               AVAILABLEDEBUGS[Index].Description);
      ReturnCommand(Ctx->Chat, LOG_LEVEL_INFO, Msg);
    }
    return;
  }

  if (OptStr == (void*)0) {
    ReturnCommand(Ctx->Chat, LOG_LEVEL_ERROR,
                  "Missing state (1 or 0). Ex: /debug wireframe 1");
    return;
  }

  bool State = (ParseInt(OptStr, (char **)0) == 1);

  for (int Index = 0; Index < AVAILABLEDEBUGSCOUNT; Index++) {
    if (CompareString(DebugStr, AVAILABLEDEBUGS[Index].Name) == 0) {
      AVAILABLEDEBUGS[Index].Func(Ctx, State);
      char Msg[MSG_BUFFER_SIZE];
      snprintf(Msg, sizeof(Msg), "Debug %s %s", DebugStr,
               (int)State == 1 ? "activated" : "deactivated");
      ReturnCommand(Ctx->Chat, LOG_LEVEL_INFO, Msg);
      return;
    }
  }

  ReturnCommand(Ctx->Chat, LOG_LEVEL_ERROR,
                "Incorrect Format. try: /debug <command> <1/0> or /debug help");
}

bool CommandHandler(char *Command, ChatState *Chat, GameCamera *Camera,
                    Player *Player, World *World) {
  CommandContext Ctx = {
      .Chat = Chat,
      .Camera = Camera,
      .BlockRegistry = GetBlockRegistry(),
      .BlockCount = GetLoadedBlocksCount(),
      .Player = Player,
      .World = World,
  };

  char Buffer[CHAT_MAX_INPUT_CHARS];
  SafeStrncpy(Buffer, Command, (int)sizeof(Buffer));

  char *SavePtr = Buffer;
  char *CommandName = TokenizeSpace(&SavePtr);

  if (CommandName == (void*)0) {
    return false;
  }

  char *Args = GetRemainingString(&SavePtr);

  for (int Index = 0; Index < AVAILABLECOMMANDSCOUNT; Index++) {
    if (CompareString(CommandName, AVAILABLECOMMANDS[Index].Name) == 0) {
      AVAILABLECOMMANDS[Index].Function(Args, &Ctx);
      return true;
    }
  }
  char Msg[MSG_BUFFER_SIZE];
  snprintf(Msg, sizeof(Msg), "Unknown command: %s. Type /help", CommandName);
  ReturnCommand(Chat, LOG_LEVEL_WARN, Msg);
  return false;
}

void PollCommandEvents(ChatState *Chat) {
#ifdef MINEGAME_PROFILE
  int Frames = 0;
  if (ProfilerConsumeTimedStop(&Frames)) {
    char Msg[MSG_BUFFER_SIZE];
    snprintf(Msg, sizeof(Msg), "Timed capture finished: %d frames written to %s",
             Frames, GetLastCapturePath());
    ReturnCommand(Chat, LOG_LEVEL_INFO, Msg);
  }
#else
  (void)Chat;
#endif
}

int CommandAutocomplete(const char *Buffer, int CursorPos, char *OutCompleted,
                        int Capacity, int *OutNewCursor, ChatState *Chat,
                        int CycleIndex) {
  if (Buffer == NULL || OutCompleted == NULL || OutNewCursor == NULL ||
      Capacity <= 0 || Buffer[0] != '/' || CursorPos <= 0) {
    return 0;
  }

  int TokenStart = CursorPos;
  while (TokenStart > 0 && Buffer[TokenStart - 1] != ' ') {
    TokenStart--;
  }

  char CurrentToken[MAX_AUTOCOMPLETE_TOKEN_LENGTH];
  int TokenLen = CursorPos - TokenStart;
  if (TokenLen >= MAX_AUTOCOMPLETE_TOKEN_LENGTH) {
    TokenLen = MAX_AUTOCOMPLETE_TOKEN_LENGTH - 1;
  }
  if (TokenLen > 0) {
    memcpy(CurrentToken, &Buffer[TokenStart], (size_t)TokenLen);
  }
  CurrentToken[TokenLen] = '\0';

  int TokenIndex = 0;
  char FirstToken[MAX_AUTOCOMPLETE_TOKEN_LENGTH] = {0};
  char SecondToken[MAX_AUTOCOMPLETE_TOKEN_LENGTH] = {0};

  {
    int Scan = 0;
    while (Scan < TokenStart && Buffer[Scan] == ' ') {
      Scan++;
    }
    int WordIdx = 0;
    while (Scan < TokenStart) {
      int WStart = Scan;
      while (Scan < TokenStart && Buffer[Scan] != ' ') {
        Scan++;
      }
      int WLen = Scan - WStart;
      if (WordIdx == 0) {
        int CopyLen = WLen < MAX_AUTOCOMPLETE_TOKEN_LENGTH - 1
                          ? WLen
                          : MAX_AUTOCOMPLETE_TOKEN_LENGTH - 1;
        memcpy(FirstToken, &Buffer[WStart], (size_t)CopyLen);
        FirstToken[CopyLen] = '\0';
      } else if (WordIdx == 1) {
        int CopyLen = WLen < MAX_AUTOCOMPLETE_TOKEN_LENGTH - 1
                          ? WLen
                          : MAX_AUTOCOMPLETE_TOKEN_LENGTH - 1;
        memcpy(SecondToken, &Buffer[WStart], (size_t)CopyLen);
        SecondToken[CopyLen] = '\0';
      }
      WordIdx++;
      while (Scan < TokenStart && Buffer[Scan] == ' ') {
        Scan++;
      }
    }
    TokenIndex = WordIdx;
  }

  const char *Candidates[MAX_AUTOCOMPLETE_CANDIDATES];
  int CandidateCount = 0;

  if (TokenIndex == 0) {
    for (int Idx = 0;
         Idx < AVAILABLECOMMANDSCOUNT && CandidateCount < MAX_AUTOCOMPLETE_CANDIDATES;
         Idx++) {
      Candidates[CandidateCount++] = AVAILABLECOMMANDS[Idx].Name;
    }
  } else if (TokenIndex == 1) {
    if (CompareString(FirstToken, "/debug") == 0) {
      for (int Idx = 0;
           Idx < AVAILABLEDEBUGSCOUNT && CandidateCount < MAX_AUTOCOMPLETE_CANDIDATES;
           Idx++) {
        Candidates[CandidateCount++] = AVAILABLEDEBUGS[Idx].Name;
      }
      if (CandidateCount < MAX_AUTOCOMPLETE_CANDIDATES) {
        Candidates[CandidateCount++] = "help";
      }
    } else if (CompareString(FirstToken, "/prefab") == 0) {
      static const char *PrefabSubcommands[] = {"list", "stamp", "pos1",
                                                "pos2", "offset", "save"};
      int SubCount = (int)(sizeof(PrefabSubcommands) / sizeof(PrefabSubcommands[0]));
      for (int Idx = 0;
           Idx < SubCount && CandidateCount < MAX_AUTOCOMPLETE_CANDIDATES;
           Idx++) {
        Candidates[CandidateCount++] = PrefabSubcommands[Idx];
      }
    } else if (CompareString(FirstToken, "/noclip") == 0) {
      Candidates[CandidateCount++] = "1";
      Candidates[CandidateCount++] = "0";
      Candidates[CandidateCount++] = "speed";
    } else if (CompareString(FirstToken, "/speed") == 0) {
      static const char *SpeedVals[] = {"10", "15", "25", "50"};
      int SpeedCount = (int)(sizeof(SpeedVals) / sizeof(SpeedVals[0]));
      for (int Idx = 0;
           Idx < SpeedCount && CandidateCount < MAX_AUTOCOMPLETE_CANDIDATES;
           Idx++) {
        Candidates[CandidateCount++] = SpeedVals[Idx];
      }
    } else if (CompareString(FirstToken, "/guiscale") == 0) {
      static const char *GuiScales[] = {"0", "1", "2", "3", "4"};
      int ScaleCount = (int)(sizeof(GuiScales) / sizeof(GuiScales[0]));
      for (int Idx = 0;
           Idx < ScaleCount && CandidateCount < MAX_AUTOCOMPLETE_CANDIDATES;
           Idx++) {
        Candidates[CandidateCount++] = GuiScales[Idx];
      }
#ifdef MINEGAME_PROFILE
    } else if (CompareString(FirstToken, "/profile") == 0) {
      static const char *ProfileSubcommands[] = {"start", "stop", "dump",
                                                 "reset"};
      int SubCount =
          (int)(sizeof(ProfileSubcommands) / sizeof(ProfileSubcommands[0]));
      for (int Idx = 0;
           Idx < SubCount && CandidateCount < MAX_AUTOCOMPLETE_CANDIDATES;
           Idx++) {
        Candidates[CandidateCount++] = ProfileSubcommands[Idx];
      }
#endif
    } else if (CompareString(FirstToken, "/help") == 0) {
      for (int Idx = 0;
           Idx < AVAILABLECOMMANDSCOUNT && CandidateCount < MAX_AUTOCOMPLETE_CANDIDATES;
           Idx++) {
        Candidates[CandidateCount++] = AVAILABLECOMMANDS[Idx].Name + 1;
      }
    } else if (CompareString(FirstToken, "/locate") == 0) {
      Candidates[CandidateCount++] = "biome";
      Candidates[CandidateCount++] = "prefab";
      Candidates[CandidateCount++] = "structure";
    }
  } else if (TokenIndex == 2) {
    if (CompareString(FirstToken, "/prefab") == 0 &&
        CompareString(SecondToken, "stamp") == 0) {
      int PrefabTotal = GetLoadedPrefabsCount();
      for (int Idx = 0;
           Idx < PrefabTotal && CandidateCount < MAX_AUTOCOMPLETE_CANDIDATES;
           Idx++) {
        const Prefab *P = GetPrefabByIndex(Idx);
        if (P != NULL && P->Name[0] != '\0') {
          Candidates[CandidateCount++] = P->Name;
        }
      }
    } else if (CompareString(FirstToken, "/debug") == 0) {
      Candidates[CandidateCount++] = "1";
      Candidates[CandidateCount++] = "0";
    } else if (CompareString(FirstToken, "/noclip") == 0) {
      static const char *SpeedVals[] = {"10", "15", "25", "50"};
      int SpeedCount = (int)(sizeof(SpeedVals) / sizeof(SpeedVals[0]));
      for (int Idx = 0;
           Idx < SpeedCount && CandidateCount < MAX_AUTOCOMPLETE_CANDIDATES;
           Idx++) {
        Candidates[CandidateCount++] = SpeedVals[Idx];
      }
    } else if (CompareString(FirstToken, "/locate") == 0 &&
               CompareString(SecondToken, "biome") == 0) {
      int BiomeTotal = GetLoadedBiomesCount();
      for (int Idx = 0;
           Idx < BiomeTotal && CandidateCount < MAX_AUTOCOMPLETE_CANDIDATES;
           Idx++) {
        const BiomeType *B = GetBiomeDef(Idx);
        if (B != NULL && B->Name[0] != '\0') {
          Candidates[CandidateCount++] = B->Name;
        }
      }
    } else if (CompareString(FirstToken, "/locate") == 0 &&
               (CompareString(SecondToken, "prefab") == 0 ||
                CompareString(SecondToken, "structure") == 0)) {
      int PrefabTotal = GetLoadedPrefabsCount();
      for (int Idx = 0;
           Idx < PrefabTotal && CandidateCount < MAX_AUTOCOMPLETE_CANDIDATES;
           Idx++) {
        const Prefab *P = GetPrefabByIndex(Idx);
        if (P != NULL && P->Name[0] != '\0') {
          Candidates[CandidateCount++] = P->Name;
        }
      }
    }
  }

  const char *Matches[MAX_AUTOCOMPLETE_CANDIDATES];
  int MatchCount = 0;
  for (int Idx = 0; Idx < CandidateCount; Idx++) {
    if (TokenLen == 0 ||
        PrefixMatchesNoCase(Candidates[Idx], CurrentToken, (SizeType)TokenLen)) {
      Matches[MatchCount++] = Candidates[Idx];
    }
  }

  if (MatchCount == 0) {
    return 0;
  }

  if (MatchCount == 1) {
    const char *Match = Matches[0];
    int MatchLen = (int)strlen(Match);
    int SuffixLen = (int)strlen(&Buffer[CursorPos]);
    int Needed = TokenStart + MatchLen + 1 + SuffixLen + 1;
    if (Needed > Capacity) {
      return 0;
    }
    memcpy(OutCompleted, Buffer, (size_t)TokenStart);
    memcpy(&OutCompleted[TokenStart], Match, (size_t)MatchLen);
    OutCompleted[TokenStart + MatchLen] = ' ';
    memcpy(&OutCompleted[TokenStart + MatchLen + 1], &Buffer[CursorPos],
           (size_t)SuffixLen + 1U);
    *OutNewCursor = TokenStart + MatchLen + 1;
    return 1;
  }

  int LcpLen = (int)strlen(Matches[0]);
  for (int Idx = 1; Idx < MatchCount; Idx++) {
    int CharIdx = 0;
    while (CharIdx < LcpLen && Matches[0][CharIdx] == Matches[Idx][CharIdx]) {
      CharIdx++;
    }
    LcpLen = CharIdx;
  }

  if (LcpLen > TokenLen) {
    int SuffixLen = (int)strlen(&Buffer[CursorPos]);
    int Needed = TokenStart + LcpLen + SuffixLen + 1;
    if (Needed > Capacity) {
      return 0;
    }
    memcpy(OutCompleted, Buffer, (size_t)TokenStart);
    memcpy(&OutCompleted[TokenStart], Matches[0], (size_t)LcpLen);
    memcpy(&OutCompleted[TokenStart + LcpLen], &Buffer[CursorPos],
           (size_t)SuffixLen + 1U);
    *OutNewCursor = TokenStart + LcpLen;
    return MatchCount;
  }

  int Picked = CycleIndex % MatchCount;
  if (Picked < 0) {
    Picked += MatchCount;
  }
  const char *Choice = Matches[Picked];
  int ChoiceLen = (int)strlen(Choice);
  int SuffixLen = (int)strlen(&Buffer[CursorPos]);
  int Needed = TokenStart + ChoiceLen + SuffixLen + 1;
  if (Needed > Capacity) {
    return 0;
  }

  if (CycleIndex == 0 && Chat != NULL) {
    char Summary[MSG_BUFFER_SIZE];
    int Pos = snprintf(Summary, sizeof(Summary), "Matches: ");
    for (int Idx = 0; Idx < MatchCount; Idx++) {
      int Written = snprintf(&Summary[Pos], sizeof(Summary) - (size_t)Pos,
                             "%s%s", (Idx > 0 ? ", " : ""), Matches[Idx]);
      if (Written > 0 && (size_t)Pos + (size_t)Written < sizeof(Summary)) {
        Pos += Written;
      } else {
        break;
      }
    }
    AddChatHistory(Chat, Summary);
  }

  memcpy(OutCompleted, Buffer, (size_t)TokenStart);
  memcpy(&OutCompleted[TokenStart], Choice, (size_t)ChoiceLen);
  memcpy(&OutCompleted[TokenStart + ChoiceLen], &Buffer[CursorPos],
         (size_t)SuffixLen + 1U);
  *OutNewCursor = TokenStart + ChoiceLen;
  return MatchCount;
}
