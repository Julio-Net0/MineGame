#ifndef COMMANDS_H
#define COMMANDS_H

#include "chat.h"
#include "raylib.h"

typedef struct{
  const char* name;
  const char* use;
  const char* description;
} CommandInfo;

void CommandHandler(char *command, ChatState *chat, Camera3D *camera);

#endif
