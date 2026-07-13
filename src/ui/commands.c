#include "ui/commands.h"
#include "world/block_system.h"
#include "ui/chat.h"
#include "ui/debug.h"
#include "core/log.h"
#include "core/utils.h"
#include "world/world.h"
#include "persistence/world_save.h"
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
  case LOG_WARNING:
    LogWarn("%s", Message);
    break;
  case LOG_ERROR:
    LogError("%s", Message);
    break;
  default:
    LogInfo("%s", Message);
    break;
  }
}

static void CommandTP(const char *Args, CommandContext *Ctx) {
  if (Args == (void*)0) {
    ReturnCommand(Ctx->Chat, LOG_ERROR,
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
      ReturnCommand(Ctx->Chat, LOG_ERROR,
                    "Invalid coordinates. Must be numeric values.");
      return;
    }

    SetPlayerPosition(Ctx->Player, (Vec3){ValX, ValY, ValZ});
    Ctx->Player->Velocity = (Vec3){0.0F, 0.0F, 0.0F};

    char Msg[MSG_BUFFER_SIZE];
    snprintf(Msg, sizeof(Msg), "Tp to X:%.1f Y:%.1f Z:%.1f", (double)ValX,
             (double)ValY, (double)ValZ);
    ReturnCommand(Ctx->Chat, LOG_INFO, Msg);

  } else {
    ReturnCommand(Ctx->Chat, LOG_ERROR,
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
  ReturnCommand(Ctx->Chat, LOG_INFO, Msg);
}

static void CommandSeed(const char *Args, CommandContext *Ctx) {
  (void)Args;
  char Msg[MSG_BUFFER_SIZE];
  snprintf(Msg, sizeof(Msg), "World Seed: %llu",
           (unsigned long long)GetWorldSeed());
  ReturnCommand(Ctx->Chat, LOG_INFO, Msg);
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
  ReturnCommand(Ctx->Chat, LOG_INFO, Msg);
}

static void CommandList(const char *Args, CommandContext *Ctx) {
  (void)Args;

  if (Ctx->BlockRegistry == (void*)0 || Ctx->BlockCount == 0) {
    ReturnCommand(Ctx->Chat, LOG_WARNING, "No block asset loaded");
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
  ReturnCommand(Ctx->Chat, LOG_INFO, Msg);
}

static void CommandNoclip(const char *Args, CommandContext *Ctx) {
  if (Args == (void*)0) {
    ReturnCommand(Ctx->Chat, LOG_ERROR, "Incorrect Format. try: /noclip <1/0>");
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
      ReturnCommand(Ctx->Chat, LOG_INFO, "Noclip activated");
    } else {
      Ctx->Player->Noclip = false;
      ReturnCommand(Ctx->Chat, LOG_INFO, "Noclip deactivated");
    }
  } else {
    ReturnCommand(Ctx->Chat, LOG_ERROR, "Incorrect Format. try: /noclip <1/0>");
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
        Ctx->Chat, LOG_ERROR,
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
        Ctx->Chat, LOG_ERROR,
        "Incorrect Format. try: /debug <command> <1/0> or /debug help");
    return;
  }

  if (CompareString(DebugStr, "help") == 0) {
    ReturnCommand(Ctx->Chat, LOG_INFO, "===DEBUG COMMANDS===");
    #pragma unroll
    for (int Index = 0; Index < AVAILABLEDEBUGSCOUNT; Index++) {
      char Msg[MSG_BUFFER_SIZE];
      snprintf(Msg, sizeof(Msg), "/debug %s %s", AVAILABLEDEBUGS[Index].Name,
               AVAILABLEDEBUGS[Index].Description);
      ReturnCommand(Ctx->Chat, LOG_INFO, Msg);
    }
    return;
  }

  if (OptStr == (void*)0) {
    ReturnCommand(Ctx->Chat, LOG_ERROR,
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
      ReturnCommand(Ctx->Chat, LOG_INFO, Msg);
      return;
    }
  }

  ReturnCommand(Ctx->Chat, LOG_ERROR,
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
  ReturnCommand(Chat, LOG_WARNING, Msg);
}
