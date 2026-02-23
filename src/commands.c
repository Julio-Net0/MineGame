#include "commands.h"
#include "raylib.h"
#include "chat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

void CommandHelp(char *args, CommandContext *ctx);
void CommandTP(char *args, CommandContext *ctx);
void CommandPos(char *args, CommandContext *ctx);

CommandInfo availableCommands[] = {
  {"/help", "Use: /help", "List all the commands available and how to use them", CommandHelp},
  {"/tp", "Use: /tp <x> <Y> <z>", "Teleports to coordinates x y z", CommandTP},
  {"/pos", "Use: /pos", "Returns your current position", CommandPos},
};

const int AVAILABLECOMMANDSCOUNT = sizeof(availableCommands) / sizeof(availableCommands[0]);

void ReturnCommand(ChatState *chat, int logLevel, const char *format, ...){

  char message[CHAT_MAX_INPUT_CHARS];

  va_list args;
  va_start(args, format);

  vsnprintf(message, CHAT_MAX_INPUT_CHARS, format, args);
  va_end(args);

  AddChatHistory(chat, "%s", message);
  TraceLog(logLevel, "%s", message);
}

void CommandTP(char *args, CommandContext *ctx){
  if(args == NULL){
    ReturnCommand(ctx->chat, LOG_ERROR, "Incorrect Format. try: /tp <x> <y> <z>");
    return;
  }

  char *x_str = strtok(args, " ");
  char *y_str = strtok(NULL, " ");
  char *z_str = strtok(NULL, " ");

  if(x_str && y_str && z_str){
    float x = atof(x_str);
    float y = atof(y_str);
    float z = atof(z_str);

    ctx->camera->position = (Vector3){x, y, z};
    ctx->camera->target = (Vector3){x, y, z + 1.0F};
    
    ReturnCommand(ctx->chat, LOG_INFO, "Tp to X:%.1f Y:%.1f Z:%.1f", x, y, z);

  }else{
    ReturnCommand(ctx->chat, LOG_ERROR, "Incorrect Format. try: /tp <x> <y> <z>");
  }
}

void CommandHelp(char *args, CommandContext *ctx){
  for(int i = 0; i < sizeof(availableCommands) / sizeof(CommandInfo); i++){
    AddChatHistory(ctx->chat, "%s | %s | %s", availableCommands[i].name, availableCommands[i].use, availableCommands[i].description);
  }
}

void CommandPos(char *args, CommandContext *ctx){

  float posX = ctx->camera->position.x;
  float posY = ctx->camera->position.y;
  float posZ = ctx->camera->position.z;

  ReturnCommand(ctx->chat, LOG_INFO, "X:%.1f Y:%.1f Z:%.1f", posX, posY, posZ );
}

void CommandHandler(char *command, ChatState *chat, Camera3D *camera){

  CommandContext ctx = {
    .chat = chat,
    .camera = camera,
  };

  char buffer[CHAT_MAX_INPUT_CHARS];

  strncpy(buffer, command, CHAT_MAX_INPUT_CHARS - 1);
  buffer[CHAT_MAX_INPUT_CHARS - 1]= '\0';

  char *commandName = strtok(buffer, " ");

  if(commandName == NULL){ return; }

  char *args = strtok(NULL, "");

  for(int i = 0; i < AVAILABLECOMMANDSCOUNT; i++){
    if(strcmp(commandName, availableCommands[i].name) == 0){
      availableCommands[i].function(args, &ctx);
      return;
    }
  }
  ReturnCommand(chat, LOG_WARNING, "Unknown command: %s. Type /help", commandName);
}
