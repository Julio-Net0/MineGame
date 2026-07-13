#ifndef COMMANDS_H
#define COMMANDS_H

#include "world/block_system.h"
#include "ui/chat.h"
#include "core/camera.h"
#include "player/player.h"
#include "world/world.h"

typedef struct {
  ChatState *Chat;
  GameCamera *Camera;
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

void CommandHandler(char *Command, ChatState *Chat, GameCamera *Camera, Player *Player, World *World);

#endif
