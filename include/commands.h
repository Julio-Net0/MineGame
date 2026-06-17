#ifndef COMMANDS_H
#define COMMANDS_H

#include "block_system.h"
#include "chat.h"
#include "raylib.h"
#include "player.h"
#include "world.h"

typedef struct {
  ChatState *Chat;
  Camera3D *Camera;
  BlockType *BlockRegistry;
  int BlockCount;
  Player *Player;
  World *World;
} CommandContext;

typedef void (*CommandFunc)(const char *Args, CommandContext *Ctx);

typedef struct {
  const char *Name;
  const char *Use;
  const char *Description;
  CommandFunc Function;
} CommandInfo;

typedef void (*DebugFunc)(CommandContext *Ctx, bool State);

typedef struct {
  const char *Name;
  const char *Description;
  DebugFunc Func;
} DebugToggle;

void CommandHandler(char *Command, ChatState *Chat, Camera3D *Camera, Player *Player, World *World);

#endif
