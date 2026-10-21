#include "ui/chat.h"
#include "ui/commands.h"
#include "player/player.h"
#include "input/input.h"
#include "platform/platform.h"
#include "ui/font.h"
#include "core/color.h"
#include "core/log.h"
#include "core/utils.h"
#include <string.h>

// Vanilla chat layout, in GUI units: the newest line sits 40 units above the
// bottom edge (clear of the hotbar), lines are one font line apart, and the
// input box runs along the bottom.
enum {
  CHAT_BOTTOM_OFFSET = 40,
  CHAT_LINE_WIDTH = 320,
  CHAT_UNFOCUSED_LINES = 10,
  CHAT_FOCUSED_LINES = 20,
  CHAT_TEXT_INSET = 2,
  CHAT_INPUT_MARGIN = 2,
  CHAT_INPUT_HEIGHT = 12,
  CHAT_INPUT_TEXT_INSET = 4,
  CHAT_INPUT_TEXT_RAISE = 12,
  CHAT_CARET_BLINK_RATE = 2,
  UTF8_CONTINUATION_MASK = 0xC0,
  UTF8_CONTINUATION_TAG = 0x80,
  UTF8_TWO_MASK = 0xE0,
  UTF8_TWO_TAG = 0xC0,
  UTF8_THREE_MASK = 0xF0,
  UTF8_THREE_TAG = 0xE0,
  UTF8_FOUR_MASK = 0xF8,
  UTF8_FOUR_TAG = 0xF0
};

#define CHAT_HISTORY_BG_ALPHA 0.5F
#define CHAT_INPUT_BG_ALPHA 0.5F

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

    int MaxScroll = Chat->HistoryCount - CHAT_FOCUSED_LINES;
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

static int Utf8SequenceLength(unsigned char Lead) {
  if ((Lead & UTF8_TWO_MASK) == UTF8_TWO_TAG) {
    return 2;
  }
  if ((Lead & UTF8_THREE_MASK) == UTF8_THREE_TAG) {
    return 3;
  }
  if ((Lead & UTF8_FOUR_MASK) == UTF8_FOUR_TAG) {
    return 4;
  }
  return 1;
}

// Input arrives as UTF-8. Characters are appended and removed as whole
// sequences, so a full buffer or a backspace never leaves half a character.
static void ApplyTextInput(ChatState *Chat, const TextInput *Text) {
  int Idx = 0;
  while (Idx < Text->Count) {
    int Length = Utf8SequenceLength((unsigned char)Text->Chars[Idx]);
    if (Idx + Length > Text->Count) {
      break;
    }
    if (Chat->LetterCount >= 0 && Chat->LetterCount <= CHAT_MAX_INPUT_CHARS &&
        Length <= CHAT_MAX_INPUT_CHARS - Chat->LetterCount) {
      memcpy(&Chat->InputText[Chat->LetterCount], &Text->Chars[Idx],
             (size_t)Length);
      Chat->LetterCount += Length;
      Chat->InputText[Chat->LetterCount] = '\0';
    }
    Idx += Length;
  }

  if (Text->Backspace && Chat->LetterCount > 0) {
    Chat->LetterCount--;
    while (Chat->LetterCount > 0 &&
           (((unsigned char)Chat->InputText[Chat->LetterCount]) &
            UTF8_CONTINUATION_MASK) == UTF8_CONTINUATION_TAG) {
      Chat->LetterCount--;
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

static float HistoryAlpha(const ChatState *Chat, const ChatMessage *Message,
                          double CurrentTime) {
  if (Chat->IsActive) {
    return 1.0F;
  }
  float Age = (float)(CurrentTime - Message->TimeCreated);
  if (Age < CHAT_DISPLAY_TIME) {
    return 1.0F;
  }
  if (Age < CHAT_DISPLAY_TIME + CHAT_FADE_TIME) {
    return 1.0F - ((Age - CHAT_DISPLAY_TIME) / CHAT_FADE_TIME);
  }
  return 0.0F;
}

void DrawChat(UiContext *Ctx, ChatState *Chat) {
  double CurrentTime = PlatformGetTime();
  int MaxLines = Chat->IsActive ? CHAT_FOCUSED_LINES : CHAT_UNFOCUSED_LINES;
  int LineWidth = CHAT_LINE_WIDTH;
  if (LineWidth > Ctx->GuiWidth - (CHAT_TEXT_INSET * 2)) {
    LineWidth = Ctx->GuiWidth - (CHAT_TEXT_INSET * 2);
  }

  for (int Line = 0; Line < MaxLines; Line++) {
    int Idx = Chat->HistoryCount - 1 - Chat->ScrollOffset - Line;
    int Top = Ctx->GuiHeight - CHAT_BOTTOM_OFFSET - ((Line + 1) * FONT_LINE_HEIGHT);
    if (Idx < 0 || Top < 0) {
      break;
    }

    float Alpha = HistoryAlpha(Chat, &Chat->History[Idx], CurrentTime);
    if (Alpha <= 0.0F) {
      continue;
    }
    UiFillRect(Ctx,
               (UiRect){0, Top, LineWidth + (CHAT_TEXT_INSET * 2),
                        FONT_LINE_HEIGHT},
               Color8Alpha(COLOR_BLACK, CHAT_HISTORY_BG_ALPHA * Alpha));
    UiTextShadow(Ctx, Chat->History[Idx].Text, (float)CHAT_TEXT_INSET,
                 (float)(Top + 1), Color8Alpha(COLOR_WHITE, Alpha));
  }

  if (Chat->IsActive) {
    UiFillRect(Ctx,
               (UiRect){CHAT_INPUT_MARGIN,
                        Ctx->GuiHeight - CHAT_INPUT_MARGIN - CHAT_INPUT_HEIGHT,
                        Ctx->GuiWidth - (CHAT_INPUT_MARGIN * 2),
                        CHAT_INPUT_HEIGHT},
               Color8Alpha(COLOR_BLACK, CHAT_INPUT_BG_ALPHA));

    float TextX = (float)CHAT_INPUT_TEXT_INSET;
    float TextY = (float)(Ctx->GuiHeight - CHAT_INPUT_TEXT_RAISE);
    UiTextShadow(Ctx, Chat->InputText, TextX, TextY, COLOR_WHITE);

    if (((int)(CurrentTime * CHAT_CARET_BLINK_RATE)) % 2 == 0) {
      float CaretX = TextX + UiMeasureText(Chat->InputText);
      UiTextShadow(Ctx, "_", CaretX, TextY, COLOR_WHITE);
    }
  }
}
