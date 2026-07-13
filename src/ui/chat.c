#include "ui/chat.h"
#include "ui/commands.h"
#include "player/player.h"
#include "input/input.h"
#include "platform/platform.h"
#include "render/backend.h"
#include "core/color.h"
#include "core/log.h"
#include "core/utils.h"

void InitChat(ChatState *Chat) {
  Chat->InputText[0] = '\0';
  Chat->LetterCount = 0;
  Chat->IsActive = false;
  Chat->HistoryCount = 0;
  Chat->ScrollOffset = 0;
}

void AddChatHistory(ChatState *Chat, const char *Message) {
  if (Chat->HistoryCount < CHAT_MAX_HISTORY) {
    SafeStrncpy(Chat->History[Chat->HistoryCount].Text, Message, CHAT_MAX_INPUT_CHARS);
    Chat->History[Chat->HistoryCount].TimeCreated = PlatformGetTime();
    Chat->HistoryCount++;
  } else {
    #pragma unroll 4
    for (int Idx = 0; Idx < CHAT_MAX_HISTORY - 1; Idx++) {
      Chat->History[Idx] = Chat->History[Idx + 1];
    }
    SafeStrncpy(Chat->History[CHAT_MAX_HISTORY - 1].Text, Message, CHAT_MAX_INPUT_CHARS);
    Chat->History[CHAT_MAX_HISTORY - 1].TimeCreated = PlatformGetTime();
  }
}

static void UpdateChatScroll(ChatState *Chat, int Delta) {
  if (Delta != 0) {
    Chat->ScrollOffset += Delta;

    int MaxVisible = (CHAT_POSITION_Y - CHAT_HISTORY_LIMIT_Y) / CHAT_FONT_SIZE;
    int MaxScroll = Chat->HistoryCount - MaxVisible + 1;
    if (MaxScroll < 0) {
      MaxScroll = 0;
    }

    if (Chat->ScrollOffset > MaxScroll) {
      Chat->ScrollOffset = MaxScroll;
    }
    if (Chat->ScrollOffset < 0) {
      Chat->ScrollOffset = 0;
    }
  }
}

static void ApplyTextInput(ChatState *Chat, const TextInput *Text) {
  #pragma unroll 4
  for (int Idx = 0; Idx < Text->Count; Idx++) {
    if (Chat->LetterCount < CHAT_MAX_INPUT_CHARS) {
      Chat->InputText[Chat->LetterCount] = Text->Chars[Idx];
      Chat->InputText[Chat->LetterCount + 1] = '\0';
      Chat->LetterCount++;
    }
  }

  if (Text->Backspace) {
    Chat->LetterCount--;
    if (Chat->LetterCount < 0) {
      Chat->LetterCount = 0;
    }
    Chat->InputText[Chat->LetterCount] = '\0';
  }
}

static void HandleChatActions(ChatState *Chat, const TextInput *Text,
                              GameCamera *Camera, Player *Player, World *World) {
  if (Text->Cancel) {
    Chat->ScrollOffset = 0;
    Chat->IsActive = false;
    PlatformSetCursorDisabled(true);
  }

  if (Text->Submit) {
    if (Chat->InputText[0] == '/') {
      CommandHandler(Chat->InputText, Chat, Camera, Player, World);
    } else {
      LogInfo("%s", Chat->InputText);
      AddChatHistory(Chat, Chat->InputText);
    }

    Chat->LetterCount = 0;
    Chat->InputText[0] = '\0';
    Chat->ScrollOffset = 0;
    Chat->IsActive = false;
    PlatformSetCursorDisabled(true);
  }
}

void UpdateChat(ChatState *Chat, GameCamera *Camera, Player *Player, World *World) {
  if (!Chat->IsActive) {
    if (PollSystemInput().ChatOpen) {
      Chat->IsActive = true;
      PlatformSetCursorDisabled(false);
      // Drain the frame's typed characters so the chat-open key is not entered.
      PollTextInput();
    }
    return;
  }

  UpdateChatScroll(Chat, InputGetScrollDelta());

  TextInput Text = PollTextInput();
  ApplyTextInput(Chat, &Text);
  HandleChatActions(Chat, &Text, Camera, Player, World);
}

void DrawChat(ChatState *Chat) {
  int HistoryBaseY = CHAT_POSITION_Y - CHAT_Y_MARGIN;
  double CurrentTime = PlatformGetTime();

  int HistoryLimit = Chat->HistoryCount;
  #pragma unroll 4
  for (int Idx = 0; Idx < CHAT_MAX_HISTORY; Idx++) {
    if (Idx >= HistoryLimit) {
      break;
    }

    int MsgY = HistoryBaseY - ((Chat->HistoryCount - Idx - Chat->ScrollOffset) * CHAT_FONT_SIZE);
    if (MsgY < CHAT_HISTORY_LIMIT_Y || MsgY >= HistoryBaseY) {
      continue;
    }

    float Age = (float)(CurrentTime - Chat->History[Idx].TimeCreated);
    float Alpha = 1.0F;

    if (!Chat->IsActive) {
      if (Age < CHAT_DISPLAY_TIME) {
        Alpha = 1.0F;
      } else if (Age < CHAT_DISPLAY_TIME + CHAT_FADE_TIME) {
        Alpha = 1.0F - ((Age - CHAT_DISPLAY_TIME) / CHAT_FADE_TIME);
      } else {
        Alpha = 0.0F;
      }
    }

    if (Alpha > 0.0F) {
      RenderDrawRect(CHAT_POSITION_X, MsgY, (int)CHAT_HISTORY_WIDTH, CHAT_FONT_SIZE,
                     Color8Alpha(COLOR_BLACK, CHAT_HISTORY_BG_ALPHA * Alpha));
      RenderDrawText(Chat->History[Idx].Text, CHAT_POSITION_X + CHAT_TEXT_PADDING,
                     MsgY, CHAT_FONT_SIZE, Color8Alpha(COLOR_WHITE, Alpha));
    }
  }

  if (Chat->IsActive) {
    RenderDrawRect(CHAT_POSITION_X, CHAT_POSITION_Y, CHAT_WIDTH, CHAT_HEIGHT,
                   Color8Alpha(COLOR_BLACK, CHAT_BG_ALPHA));
    RenderDrawRectLines(CHAT_POSITION_X, CHAT_POSITION_Y, CHAT_WIDTH, CHAT_HEIGHT,
                        COLOR_DARKGRAY);

    int TextY = CHAT_POSITION_Y + (CHAT_HEIGHT / 2) - (CHAT_FONT_SIZE / 2);
    RenderDrawText(Chat->InputText, CHAT_POSITION_X + CHAT_TEXT_PADDING, TextY,
                   CHAT_FONT_SIZE, COLOR_WHITE);

    if (((int)(PlatformGetTime() * 2)) % 2 == 0) {
      int TextWidth = RenderMeasureText(Chat->InputText, CHAT_FONT_SIZE);
      RenderDrawText("|", CHAT_POSITION_X + CHAT_TEXT_PADDING + TextWidth, TextY,
                     CHAT_FONT_SIZE, COLOR_WHITE);
    }
  }
}
