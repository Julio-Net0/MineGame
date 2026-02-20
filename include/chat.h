#ifndef CHAT_H
#define CHAT_H

#include <stdbool.h>

#define CHAT_WIDTH GetScreenWidth() - CHAT_Y_MARGIN * 2
#define CHAT_HEIGHT GetScreenHeight() / 25
#define CHAT_X_MARGIN GetScreenWidth() / 200
#define CHAT_Y_MARGIN GetScreenWidth() / 200
#define CHAT_POSITION_X CHAT_X_MARGIN
#define CHAT_POSITION_Y GetScreenHeight() - CHAT_HEIGHT - CHAT_Y_MARGIN

#define CHAT_MAX_INPUT_CHARS 50


typedef struct{
  char inputText[CHAT_MAX_INPUT_CHARS + 1];
  int letterCount;
  bool isActive;
} ChatState;

void InitChat(ChatState *chat);
void UpdateChat(ChatState *chat);
void DrawChat(ChatState *chat);

#endif
