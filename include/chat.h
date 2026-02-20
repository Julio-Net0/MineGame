#ifndef CHAT_H
#define CHAT_H

#include <stdbool.h>

#define CHAT_WIDTH (GetScreenWidth() - (CHAT_Y_MARGIN * 2))
#define CHAT_HEIGHT (GetScreenHeight() / 25)
#define CHAT_X_MARGIN (GetScreenWidth() / 200)
#define CHAT_Y_MARGIN (GetScreenWidth() / 200)
#define CHAT_POSITION_X CHAT_X_MARGIN
#define CHAT_POSITION_Y (GetScreenHeight() - CHAT_HEIGHT - CHAT_Y_MARGIN)
#define CHAT_MAX_INPUT_CHARS 50
#define CHAT_MAX_HISTORY 256
#define CHAT_DISPLAY_TIME 3.0F
#define CHAT_FADE_TIME 1.0F
#define CHAT_FONT_SIZE 20
#define CHAT_TEXT_PADDING 10
#define CHAT_HISTORY_WIDTH (GetScreenWidth() / 2) - CHAT_Y_MARGIN
#define CHAT_HISTORY_LIMIT_Y (GetScreenHeight() / 2)
#define CHAT_BG_ALPHA 0.5F
#define CHAT_HISTORY_BG_ALPHA 0.3F


typedef struct {
  char text[CHAT_MAX_INPUT_CHARS];
  double timeCreated;
} ChatMessage;

typedef struct{
  char inputText[CHAT_MAX_INPUT_CHARS + 1];
  int letterCount;
  bool isActive;

  ChatMessage history[CHAT_MAX_HISTORY][CHAT_MAX_INPUT_CHARS];
  int historyCount;

  int scrollOffset;
} ChatState;

void InitChat(ChatState *chat);
void UpdateChat(ChatState *chat);
void DrawChat(ChatState *chat);

void AddChatHistory(ChatState *chat, const char *message);

#endif
