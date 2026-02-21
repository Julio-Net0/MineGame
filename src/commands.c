#include "commands.h"
#include "raylib.h"
#include "chat.h"
#include <stdlib.h>
#include <string.h>

void CommandTP(char *args, ChatState *chat, Camera3D *camera){
  if(args == NULL){
    TraceLog(LOG_WARNING, "Incorrect Format. try: /tp <x> <y> <z>");
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

    AddChatHistory(chat, "Tp complete - Mov to X:%.1f Y:%.1f Z:%.1f", x, y, z);
    TraceLog(LOG_INFO, "Tp complete - Mov to X:%.1f Y:%.1f Z:%.1f", x, y, z);

  }else{
    AddChatHistory(chat, "Incorrect Format. try: /tp <x> <y> <z>");
    TraceLog(LOG_WARNING, "Incorrect Format. try: /tp <x> <y> <z>");
  }
}

void CommandHandler(char *command, ChatState *chat, Camera3D *camera){
  char buffer[CHAT_MAX_INPUT_CHARS];

  strcpy(buffer, command);
  buffer[CHAT_MAX_INPUT_CHARS-1] = '\0';

  char *commandName = strtok(buffer, " ");

  if(commandName == NULL){ 
    TraceLog(LOG_ERROR, "No command found");
    AddChatHistory(chat, "No command found");
    return;
  }

  char *args = strtok(NULL, "");

  if(strcmp(commandName, "/tp") == 0){
    CommandTP(args, chat, camera);
  }else{
    TraceLog(LOG_ERROR, "No command found");
    AddChatHistory(chat, "No command found");
    return;
  }
}
