#ifndef COMMANDS_H
#define COMMANDS_H

#include "block_system.h"
#include "chat.h"
#include "raylib.h"
#include "player.h"
#include "world.h"

typedef struct{
  ChatState *chat;
  Camera3D *camera;
  BlockType *blockRegistry;
  int blockCount;
  Player *player;
  World *world;
} CommandContext;

typedef void (*CommandFunc)(char *args, CommandContext *ctx);

typedef struct{
  const char* name;
  const char* use;
  const char* description;
  CommandFunc function;
} CommandInfo;


void CommandHandler(char *command, ChatState *chat, Camera3D *camera, Player *player, World *world);

#endif
