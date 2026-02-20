#include "commands.h"
#include "raylib.h"
#include "chat.h"
#include <stdlib.h>
#include <string.h>

void CommandTP(char *args){
  if(args == NULL){
    TraceLog(LOG_WARNING, "Incorrect Format. try: /tp <x> <y> <z>");
    return;
  }

  char *x_str = strtok(args, " ");
  char *y_str = strtok(NULL, " ");
  char *z_str = strtok(NULL, " ");


  TraceLog(LOG_INFO, "X: %s", x_str ? x_str : "NULL");
  TraceLog(LOG_INFO, "Y: %s", y_str ? y_str : "NULL");
  TraceLog(LOG_INFO, "Z: %s", z_str ? z_str : "NULL");

  if(x_str && y_str && z_str){
    float x = atof(x_str);
    float y = atof(y_str);
    float z = atof(z_str);

    TraceLog(LOG_INFO, "Tp complete - Mov to X:%.1f Y:%.1f Z:%.1f", x, y, z);

  }else{
    TraceLog(LOG_WARNING, "Incorrect Format. try: /tp <x> <y> <z>");
  }
}

void CommandHandler(char *command){
  char buffer[CHAT_MAX_INPUT_CHARS];

  strcpy(buffer, command);
  buffer[CHAT_MAX_INPUT_CHARS-1] = '\0';

  char *commandName = strtok(buffer, " ");

  if(commandName == NULL){ 
    TraceLog(LOG_ERROR, "No command found");
    return;
  }

  char *args = strtok(NULL, "");

  if(strcmp(commandName, "/tp") == 0){
    CommandTP(args);
  }
}
