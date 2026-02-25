#include "commands.h"
#include "block_system.h"
#include "raylib.h"
#include "chat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

void CommandHelp(char *args, CommandContext *ctx);
void CommandTP(char *args, CommandContext *ctx);
void CommandPos(char *args, CommandContext *ctx);
void CommandList(char *args, CommandContext *ctx);
void CommandNoclip(char *args, CommandContext *ctx);
void CommandDebugAABB(char *args, CommandContext *ctx);

CommandInfo availableCommands[] = {
  {"/help", "Use: /help", "List all the commands available and how to use them", CommandHelp},
  {"/tp", "Use: /tp <x> <Y> <z>", "Teleports to coordinates x y z", CommandTP},
  {"/pos", "Use: /pos", "Returns your current position", CommandPos},
  {"/list", "Use: /list", "Returns all loaded block assets", CommandList},
  {"/noclip", "Use: /noclip <1/0>", "Activate or deactivate noclip flight", CommandNoclip},
  {"/debug_aabb", "Use: /debug_aabb <1/0>", "Activate or deactivate AABB debug overlay", CommandDebugAABB},
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

    ctx->player->position = (Vector3){x, y, z};
    ctx->player->velocity = (Vector3){0,0,0};
    
    ReturnCommand(ctx->chat, LOG_INFO, "Tp to X:%.1f Y:%.1f Z:%.1f", x, y, z);

  }else{
    ReturnCommand(ctx->chat, LOG_ERROR, "Incorrect Format. try: /tp <x> <y> <z>");
  }
}

void CommandHelp(char *args, CommandContext *ctx){
  for(int i = 0; i < AVAILABLECOMMANDSCOUNT; i++){
    AddChatHistory(ctx->chat, "%s | %s | %s", availableCommands[i].name, availableCommands[i].use, availableCommands[i].description);
  }
}

void CommandPos(char *args, CommandContext *ctx){

  float posX = ctx->player->position.x;
  float posY = ctx->player->position.y;
  float posZ = ctx->player->position.z;

  ReturnCommand(ctx->chat, LOG_INFO, "X:%.1f Y:%.1f Z:%.1f", posX, posY, posZ );
}

void CommandList(char *args, CommandContext *ctx){
  (void)args;

  if(ctx->blockRegistry == NULL || ctx->blockCount == 0){
    ReturnCommand(ctx->chat, LOG_WARNING, "No block asset loaded");
    return;
  }

  for(int i = 0; i < ctx->blockCount; i++){
    AddChatHistory(ctx->chat, "[ID: %d] %s",
        ctx->blockRegistry[i].id,
        ctx->blockRegistry[i].name);
  }
}

void CommandNoclip(char *args, CommandContext *ctx){

  if(args == NULL){
    ReturnCommand(ctx->chat, LOG_ERROR, "Incorrect Format. try: /noclip <1/0>");
    return;
  }

  char *opt_str = strtok(args, " ");

  if(opt_str){
    int opt = atoi(opt_str);
    if(opt == 1){
      ctx->player->noclip = true;
      ReturnCommand(ctx->chat, LOG_INFO, "Noclip activated");
    }else{
      ctx->player->noclip= false;
      ReturnCommand(ctx->chat, LOG_INFO, "Noclip deactivated");
    }
  }else{
    ReturnCommand(ctx->chat, LOG_ERROR, "Incorrect Format. try: /noclip <1/0>");
  }
}

void CommandDebugAABB(char *args, CommandContext *ctx){
  if(args == NULL){
    ReturnCommand(ctx->chat, LOG_ERROR, "Incorrect Format. try: /debug_aabb <1/0>");
    return;
  }

  char *opt_str = strtok(args, " ");

  if(opt_str){
    int opt = atoi(opt_str);
    if(opt == 1){
      ctx->player->debug_aabb = true;
      ReturnCommand(ctx->chat, LOG_INFO, "debug_aabb activated");
    }else{
      ctx->player->debug_aabb = false;
      ReturnCommand(ctx->chat, LOG_INFO, "debug_aabb deactivated");
    }
  }else{
    ReturnCommand(ctx->chat, LOG_ERROR, "Incorrect Format. try: /debug_aabb <1/0>");
  }
}

void CommandHandler(char *command, ChatState *chat, Camera3D *camera, Player *player, World *world){

  CommandContext ctx = {
    .chat = chat,
    .camera = camera,
    .blockRegistry = blockRegistry,
    .blockCount = GetLoadedBlocksCount(),
    .player = player,
    .world = world,
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
