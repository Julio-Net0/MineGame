#include "ui/commands.h"
#include "world/block_system.h"
#include "world/biome.h"
#include "world/chunk.h"
#include "world/prefab.h"
#include "world/prefab_capture.h"
#include "ui/chat.h"
#include "ui/debug.h"
#include "core/log.h"
#include "core/utils.h"
#include "world/world.h"
#include "persistence/world_save.h"
#include <stdint.h>
#include <stdio.h>

enum {
  DEBUG_HELP_LINE_SIZE = 128,
  DEBUG_STATUS_MSG_SIZE = 64,
  MSG_BUFFER_SIZE = 256
};

static void CommandHelp(const char *Args, CommandContext *Ctx);
static void CommandTP(const char *Args, CommandContext *Ctx);
static void CommandPos(const char *Args, CommandContext *Ctx);
static void CommandList(const char *Args, CommandContext *Ctx);
static void CommandNoclip(const char *Args, CommandContext *Ctx);
static void CommandDebug(const char *Args, CommandContext *Ctx);
static void CommandSeed(const char *Args, CommandContext *Ctx);
static void CommandSave(const char *Args, CommandContext *Ctx);
static void CommandPrefab(const char *Args, CommandContext *Ctx);
static void CommandBiome(const char *Args, CommandContext *Ctx);

static const CommandInfo AVAILABLECOMMANDS[] = {
    {"/help", "Use: /help",
     "List all the commands available and how to use them", CommandHelp},
    {"/tp", "Use: /tp <x> <Y> <z>", "Teleports to coordinates x y z",
     CommandTP},
    {"/pos", "Use: /pos", "Returns your current position", CommandPos},
    {"/list", "Use: /list", "Returns all loaded block assets", CommandList},
    {"/noclip", "Use: /noclip <1/0>", "Activate or deactivate noclip flight",
     CommandNoclip},
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
};

static const int AVAILABLECOMMANDSCOUNT =
    (int)(sizeof(AVAILABLECOMMANDS) / sizeof(AVAILABLECOMMANDS[0]));

static void DebugAABB(CommandContext *Ctx, bool State);
static void DebugWireframe(CommandContext *Ctx, bool State);
static void DebugChunkBorders(CommandContext *Ctx, bool State);
static void DebugFreeCam(CommandContext *Ctx, bool State);

static const DebugToggle AVAILABLEDEBUGS[] = {
    {"aabb", "Show collision boxes", DebugAABB},
    {"wireframe", "Show lines view", DebugWireframe},
    {"chunkborders", "Show chunk borders", DebugChunkBorders},
    {"freecam", "Enable freecam", DebugFreeCam},
};

static const int AVAILABLEDEBUGSCOUNT =
    (int)(sizeof(AVAILABLEDEBUGS) / sizeof(AVAILABLEDEBUGS[0]));

static char *TokenizeSpace(char **Context) {
  if (Context == (char **)0 || *Context == (char *)0) {
    return (void*)0;
  }
  char *Orig = *Context;
  int StartIndex = 0;
  #pragma unroll 4
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
  #pragma unroll 4
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

static void ReturnCommand(ChatState *Chat, int LogLevel, const char *Message) {
  AddChatHistory(Chat, Message);
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
    char *EndX = (void*)0;
    char *EndY = (void*)0;
    char *EndZ = (void*)0;

    float ValX = ParseFloat(XStr, &EndX);
    float ValY = ParseFloat(YStr, &EndY);
    float ValZ = ParseFloat(ZStr, &EndZ);

    if (EndX == XStr || *EndX != '\0' || EndY == YStr || *EndY != '\0' ||
        EndZ == ZStr || *EndZ != '\0') {
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

  #pragma unroll
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
  // The Depth axis is measured against the terrain surface, so the height must
  // come from the same field generation used, not from the player's own Y.
  int TerrainHeight = GetTerrainHeightAt(PosX, PosZ, SeedVal);

  BiomeClimate Climate =
      SampleBiomeClimate(PosX, PosY, PosZ, TerrainHeight, SeedVal);
  int BiomeId = ResolveBiome(Climate);
  const BiomeType *Biome = GetBiomeDef(BiomeId);

  char Msg[MSG_BUFFER_SIZE];
  if (Biome->Id < 0) {
    snprintf(Msg, sizeof(Msg),
             "No biomes loaded | T:%.3f H:%.3f D:%.0f (surface Y:%d)",
             (double)Climate.Temperature, (double)Climate.Humidity,
             (double)Climate.Depth, TerrainHeight);
    ReturnCommand(Ctx->Chat, LOG_LEVEL_WARN, Msg);
    return;
  }

  snprintf(Msg, sizeof(Msg),
           "Biome: %s [%d] | T:%.3f H:%.3f D:%.0f (surface Y:%d)", Biome->Name,
           BiomeId, (double)Climate.Temperature, (double)Climate.Humidity,
           (double)Climate.Depth, TerrainHeight);
  ReturnCommand(Ctx->Chat, LOG_LEVEL_INFO, Msg);
}

static void CommandSave(const char *Args, CommandContext *Ctx) {
  (void)Args;
  int SavedCount = 0;
  int WorldChunkCount = Ctx->World->ChunkCount;
  int TargetCount = WorldChunkCount < MAX_ACTIVE_CHUNKS ? WorldChunkCount : MAX_ACTIVE_CHUNKS;
  #pragma unroll 4
  for (int Index = 0; Index < MAX_ACTIVE_CHUNKS; Index++) {
    if (Index >= TargetCount) {
      break;
    }
    Chunk *CurrentChunk = &Ctx->World->Chunks[Index];
    if (CurrentChunk->IsModified) {
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

  #pragma unroll 4
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
  #pragma unroll 4
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
    ReturnCommand(Ctx->Chat, LOG_LEVEL_ERROR, "Incorrect Format. try: /noclip <1/0>");
    return;
  }

  char ArgsCopy[CHAT_MAX_INPUT_CHARS];
  SafeStrncpy(ArgsCopy, Args, (int)sizeof(ArgsCopy));

  char *SavePtr = ArgsCopy;
  char *OptStr = TokenizeSpace(&SavePtr);

  if (OptStr) {
    int Opt = ParseInt(OptStr, (char **)0);
    if (Opt == 1) {
      Ctx->Player->Noclip = true;
      ReturnCommand(Ctx->Chat, LOG_LEVEL_INFO, "Noclip activated");
    } else {
      Ctx->Player->Noclip = false;
      ReturnCommand(Ctx->Chat, LOG_LEVEL_INFO, "Noclip deactivated");
    }
  } else {
    ReturnCommand(Ctx->Chat, LOG_LEVEL_ERROR, "Incorrect Format. try: /noclip <1/0>");
  }
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
    #pragma unroll
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

  #pragma unroll
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

void CommandHandler(char *Command, ChatState *Chat, GameCamera *Camera,
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
    return;
  }

  char *Args = GetRemainingString(&SavePtr);

  #pragma unroll
  for (int Index = 0; Index < AVAILABLECOMMANDSCOUNT; Index++) {
    if (CompareString(CommandName, AVAILABLECOMMANDS[Index].Name) == 0) {
      AVAILABLECOMMANDS[Index].Function(Args, &Ctx);
      return;
    }
  }
  char Msg[MSG_BUFFER_SIZE];
  snprintf(Msg, sizeof(Msg), "Unknown command: %s. Type /help", CommandName);
  ReturnCommand(Chat, LOG_LEVEL_WARN, Msg);
}
