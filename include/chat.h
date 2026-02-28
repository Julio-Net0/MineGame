#ifndef CHAT_H
#define CHAT_H

#include "raylib.h"
#include "world.h"
#include "player.h"
#include <stdbool.h>

#define CHAT_WIDTH (GetScreenWidth() - (CHAT_Y_MARGIN * 2))
#define CHAT_HEIGHT (GetScreenHeight() / 25)
#define CHAT_X_MARGIN (GetScreenWidth() / 200)
#define CHAT_Y_MARGIN (GetScreenWidth() / 200)
#define CHAT_POSITION_X CHAT_X_MARGIN
#define CHAT_POSITION_Y (GetScreenHeight() - CHAT_HEIGHT - CHAT_Y_MARGIN)
#define CHAT_MAX_INPUT_CHARS 256 // Recommended limit is 256 to prevent Stack Overflow on Windows (1MB stack)
#define CHAT_MAX_HISTORY 256
#define CHAT_DISPLAY_TIME 3.0F
#define CHAT_FADE_TIME 1.0F
#define CHAT_FONT_SIZE ((int)(CHAT_HEIGHT * 0.7F))
#define CHAT_TEXT_PADDING 10
#define CHAT_HISTORY_WIDTH ((GetScreenWidth() / 1.2) - CHAT_Y_MARGIN)
#define CHAT_HISTORY_LIMIT_Y (GetScreenHeight() / 2)
#define CHAT_BG_ALPHA 0.5F
#define CHAT_HISTORY_BG_ALPHA 0.3F


typedef struct {
  char text[CHAT_MAX_INPUT_CHARS];
  double timeCreated;
} ChatMessage;

typedef struct{
  ChatMessage history[CHAT_MAX_HISTORY];

  char inputText[CHAT_MAX_INPUT_CHARS + 1];

  int letterCount;
  int historyCount;
  int scrollOffset;

  bool isActive;
} ChatState;

void InitChat(ChatState *chat);
void UpdateChat(ChatState *chat, Camera3D *camera, Player *player, World* world);
void DrawChat(ChatState *chat);
void AddChatHistory(ChatState *chat, const char *format, ...);

#endif
