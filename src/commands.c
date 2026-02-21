#include "commands.h"
#include "raylib.h"
#include "chat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

CommandInfo availableCommands[] = {
  {"/tp", "Use: /tp <x> <Y> <z>", "Teleports to coordinates x y z"},
};

void ReturnCommand(ChatState *chat, int logLevel, const char *format, ...){

  char message[CHAT_MAX_INPUT_CHARS];

  va_list args;
  va_start(args, format);

  vsnprintf(message, CHAT_MAX_INPUT_CHARS, format, args);
  va_end(args);

  AddChatHistory(chat, "%s", message);
  TraceLog(logLevel, "%s", message);
}

void CommandHelp(ChatState *chat){
  for(int i = 0; i < sizeof(availableCommands) / sizeof(CommandInfo); i++){
    AddChatHistory(chat, "%s | %s | %s", availableCommands[i].name, availableCommands[i].use, availableCommands[i].description);
  }
}

void CommandTP(char *args, ChatState *chat, Camera3D *camera){
  if(args == NULL){
    ReturnCommand(chat, LOG_ERROR, "Incorrect Format. try: /tp <x> <y> <z>");
    return;
  }

  char *x_str = strtok(args, " ");
  char *y_str = strtok(NULL, " ");
  char *z_str = strtok(NULL, " ");

  if(x_str && y_str && z_str){
    float x = atof(x_str);
    float y = atof(y_str);
    float z = atof(z_str);

    camera->position = (Vector3){x, y, z};
    camera->target = (Vector3){x, y, z + 1.0F};
    
    ReturnCommand(chat, LOG_INFO, "Tp to X:%.1f Y:%.1f Z:%.1f", x, y, z);

  }else{
    ReturnCommand(chat, LOG_ERROR, "Incorrect Format. try: /tp <x> <y> <z>");
  }
}



void CommandHandler(char *command, ChatState *chat, Camera3D *camera){
  char buffer[CHAT_MAX_INPUT_CHARS];

  strcpy(buffer, command);
  buffer[CHAT_MAX_INPUT_CHARS-1] = '\0';

  char *commandName = strtok(buffer, " ");

  if(commandName == NULL){ 
    ReturnCommand(chat, LOG_WARNING, "No command found");
    return;
  }

  char *args = strtok(NULL, "");

  if(strcmp(commandName, "/help") == 0){
    CommandHelp(chat);
  }else if(strcmp(commandName, "/tp") == 0){
    CommandTP(args, chat, camera);
  }else{
    ReturnCommand(chat, LOG_WARNING, "No command found");
  }
}
