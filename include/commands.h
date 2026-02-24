#ifndef COMMANDS_H
#define COMMANDS_H

#include "block_system.h"
#include "chat.h"
#include "raylib.h"
#include "player.h"

typedef struct{
  ChatState *chat;
  Camera3D *camera;
  BlockType *blockRegistry;
  int blockCount;
  Player *player;
} CommandContext;

typedef void (*CommandFunc)(char *args, CommandContext *ctx);

typedef struct{
  const char* name;
  const char* use;
  const char* description;
  CommandFunc function;
} CommandInfo;


void CommandHandler(char *command, ChatState *chat, Camera3D *camera, Player *player);

#endif
