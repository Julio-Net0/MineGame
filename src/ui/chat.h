#ifndef CHAT_H
#define CHAT_H

#include "player/player.h"
#include "core/camera.h"
#include "platform/platform.h"
#include "world/world.h"
#include "ui/ui.h"
#include <stdbool.h>

#define CHAT_MAX_INPUT_CHARS 256
#define CHAT_MAX_HISTORY 256
#define CHAT_DISPLAY_TIME 3.0F
#define CHAT_FADE_TIME 1.0F
#define CHAT_MAX_WRAPPED_LINES 16

typedef struct {
  char Text[CHAT_MAX_INPUT_CHARS];
  double TimeCreated;
  // Where the wrapped lines start, cached for the width they were computed
  // at, so a message is measured once rather than every frame. A WrapWidth of
  // zero means not computed yet.
  int WrapWidth;
  int WrappedLineCount;
  int WrappedLineStarts[CHAT_MAX_WRAPPED_LINES];
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
void DrawChat(UiContext *Ctx, ChatState *Chat);
void AddChatHistory(ChatState *Chat, const char *Message);

#endif
