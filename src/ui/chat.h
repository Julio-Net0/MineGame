#ifndef CHAT_H
#define CHAT_H

#include "player/player.h"
#include "core/camera.h"
#include "platform/platform.h"
#include "world/world.h"
#include <stdbool.h>

#define CHAT_Y_MARGIN_F ((float)PlatformGetScreenWidth() / 200.0F)
#define CHAT_Y_MARGIN ((int)CHAT_Y_MARGIN_F)
#define CHAT_HEIGHT_F ((float)PlatformGetScreenHeight() / 25.0F)
#define CHAT_HEIGHT ((int)CHAT_HEIGHT_F)

#define CHAT_WIDTH (PlatformGetScreenWidth() - (CHAT_Y_MARGIN * 2))
#define CHAT_X_MARGIN CHAT_Y_MARGIN
#define CHAT_POSITION_X CHAT_X_MARGIN
#define CHAT_POSITION_Y (PlatformGetScreenHeight() - CHAT_HEIGHT - CHAT_Y_MARGIN)
#define CHAT_MAX_INPUT_CHARS 256
#define CHAT_MAX_HISTORY 256
#define CHAT_DISPLAY_TIME 3.0F
#define CHAT_FADE_TIME 1.0F
#define CHAT_FONT_SIZE ((int)(CHAT_HEIGHT_F * 0.7F))
#define CHAT_TEXT_PADDING 10
#define CHAT_HISTORY_WIDTH ((PlatformGetScreenWidth() / 1.2) - CHAT_Y_MARGIN_F)
#define CHAT_HISTORY_LIMIT_Y (PlatformGetScreenHeight() / 2)
#define CHAT_BG_ALPHA 0.5F
#define CHAT_HISTORY_BG_ALPHA 0.3F

typedef struct {
  char Text[CHAT_MAX_INPUT_CHARS];
  double TimeCreated;
} ChatMessage;

typedef struct {
  ChatMessage History[CHAT_MAX_HISTORY];

  char InputText[CHAT_MAX_INPUT_CHARS + 1];

  int LetterCount;
  int HistoryCount;
  int ScrollOffset;

  bool IsActive;
} ChatState;

void InitChat(ChatState *Chat);
void UpdateChat(ChatState *Chat, GameCamera *Camera, Player *Player,
                World *World);
void DrawChat(ChatState *Chat);
void AddChatHistory(ChatState *Chat, const char *Message);

#endif
