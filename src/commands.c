#include "commands.h"
#include "block_system.h"
#include "raylib.h"
#include "chat.h"
#include "renderer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

void CommandHelp(char *args, CommandContext *ctx);
void CommandTP(char *args, CommandContext *ctx);
void CommandPos(char *args, CommandContext *ctx);
void CommandList(char *args, CommandContext *ctx);
void CommandNoclip(char *args, CommandContext *ctx);
void CommandDebug(char *args, CommandContext *ctx);

static CommandInfo AVAILABLECOMMANDS[] = {
  {"/help", "Use: /help", "List all the commands available and how to use them", CommandHelp},
  {"/tp", "Use: /tp <x> <Y> <z>", "Teleports to coordinates x y z", CommandTP},
  {"/pos", "Use: /pos", "Returns your current position", CommandPos},
  {"/list", "Use: /list", "Returns all loaded block assets", CommandList},
  {"/noclip", "Use: /noclip <1/0>", "Activate or deactivate noclip flight", CommandNoclip},
  {"/debug", "Use: /debug <debug/help> <1/0>", "Activate or deactivate debugs overlay", CommandDebug},
};

static const int AVAILABLECOMMANDSCOUNT = sizeof(AVAILABLECOMMANDS) / sizeof(AVAILABLECOMMANDS[0]);

void DebugAABB(CommandContext *ctx, bool state);
void DebugWireframe(CommandContext *ctx, bool state);

static DebugToggle AVAILABLEDEBUGS[] = {
  {"aabb", "Show collision boxes", DebugAABB},
  {"wireframe", "Show lines view", DebugWireframe},
};

static const int AVAILABLEDEBUGSCOUNT = sizeof(AVAILABLEDEBUGS) / sizeof(AVAILABLEDEBUGS[0]);

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
    float x = (float)atof(x_str);
    float y = (float)atof(y_str);
    float z = (float)atof(z_str);

    ctx->player->position = (Vector3){x, y, z};
    ctx->player->velocity = (Vector3){0,0,0};
    
    ReturnCommand(ctx->chat, LOG_INFO, "Tp to X:%.1f Y:%.1f Z:%.1f", x, y, z);

  }else{
    ReturnCommand(ctx->chat, LOG_ERROR, "Incorrect Format. try: /tp <x> <y> <z>");
  }
}

void CommandHelp(char *args, CommandContext *ctx){
  (void)args;

  for(int i = 0; i < AVAILABLECOMMANDSCOUNT; i++){
    AddChatHistory(ctx->chat, "%s | %s | %s", AVAILABLECOMMANDS[i].name, AVAILABLECOMMANDS[i].use, AVAILABLECOMMANDS[i].description);
  }
}

void CommandPos(char *args, CommandContext *ctx){
  (void)args;

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

  int printed = 0;
  for(int i = 0; i < BLOCK_REGISTRY_SIZE; i++){
    if(ctx->blockRegistry[i].id != -1){
      AddChatHistory(ctx->chat, "[ID: %d] %s", ctx->blockRegistry[i].id, ctx->blockRegistry[i].name);
      printed++;
    }
  }
  ReturnCommand(ctx->chat, LOG_INFO, "Listed %d blocks", printed);
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

void DebugAABB(CommandContext *ctx, bool state){
  ctx->player->debug_aabb = state;
}

void DebugWireframe(CommandContext *ctx, bool state){
  debugWireFrame = state;
}

void CommandDebug(char *args, CommandContext *ctx){
  if(args == NULL){
    ReturnCommand(ctx->chat, LOG_ERROR, "Incorrect Format. try: /debug <command> <1/0> or /debug help");
    return;
  }

  char *debug_str = strtok(args, " ");
  char *opt_str = strtok(NULL, " ");

  if(debug_str == NULL){
    ReturnCommand(ctx->chat, LOG_ERROR, "Incorrect Format. try: /debug <command> <1/0> or /debug help");
    return;
  }

  if(strcmp(debug_str, "help") == 0){
    ReturnCommand(ctx->chat, LOG_INFO, "===DEBUG COMMANDS===");
    for(int i = 0; i < AVAILABLEDEBUGSCOUNT; i++){
      char helpLine[128];
      snprintf(helpLine, sizeof(helpLine), "/debug %s %s", AVAILABLEDEBUGS[i].name, AVAILABLEDEBUGS[i].description);
      ReturnCommand(ctx->chat, LOG_INFO, helpLine);
    }
    return;
  }

  if(opt_str == NULL){
    ReturnCommand(ctx->chat, LOG_ERROR, "Missing state (1 or 0). Ex: /debug wireframe 1");
    return;
  }

  bool state = (atoi(opt_str) == 1);

  for(int i = 0; i < AVAILABLEDEBUGSCOUNT; i++){
    if(strcmp(debug_str, AVAILABLEDEBUGS[i].name) == 0){

      AVAILABLEDEBUGS[i].func(ctx, state);

      char msg[64];
      snprintf(msg, sizeof(msg), "Debug %s %s", debug_str, state ? "activated" : "deactivated");
      return;
    }
  }

  ReturnCommand(ctx->chat, LOG_ERROR, "Incorrect Format. try: /debug <command> <1/0> or /debug help");
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
    if(strcmp(commandName, AVAILABLECOMMANDS[i].name) == 0){
      AVAILABLECOMMANDS[i].function(args, &ctx);
      return;
    }
  }
  ReturnCommand(chat, LOG_WARNING, "Unknown command: %s. Type /help", commandName);
}
