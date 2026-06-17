#include "chat.h"
#include "commands.h"
#include "player.h"
#include "raylib.h"
#include "utils.h"

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
    Chat->History[Chat->HistoryCount].TimeCreated = GetTime();
    Chat->HistoryCount++;
  } else {
    #pragma unroll 4
    for (int Idx = 0; Idx < CHAT_MAX_HISTORY - 1; Idx++) {
      Chat->History[Idx] = Chat->History[Idx + 1];
    }
    SafeStrncpy(Chat->History[CHAT_MAX_HISTORY - 1].Text, Message, CHAT_MAX_INPUT_CHARS);
    Chat->History[CHAT_MAX_HISTORY - 1].TimeCreated = GetTime();
  }
}

static void UpdateChatScroll(ChatState *Chat) {
  float Wheel = GetMouseWheelMove();
  if (Wheel != 0.0F) {
    Chat->ScrollOffset += (int)Wheel;

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

static void UpdateChatInput(ChatState *Chat) {
  int Key = GetCharPressed();
  #pragma unroll 4
  for (int LoopIdx = 0; LoopIdx < CHAT_MAX_INPUT_CHARS; LoopIdx++) {
    if (Key > 0) {
      if ((Key >= ' ') && (Key <= '~') && (Chat->LetterCount < CHAT_MAX_INPUT_CHARS)) {
        Chat->InputText[Chat->LetterCount] = (char)Key;
        Chat->InputText[Chat->LetterCount + 1] = '\0';
        Chat->LetterCount++;
      }
      Key = GetCharPressed();
    } else {
      break;
    }
  }

  if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) {
    Chat->LetterCount--;
    if (Chat->LetterCount < 0) {
      Chat->LetterCount = 0;
    }
    Chat->InputText[Chat->LetterCount] = '\0';
  }
}

static void HandleChatActions(ChatState *Chat, Camera3D *Camera, Player *Player, World *World) {
  if (IsKeyPressed(KEY_DELETE)) {
    Chat->ScrollOffset = 0;
    Chat->IsActive = false;
    DisableCursor();
  }

  if (IsKeyPressed(KEY_ENTER)) {
    if (Chat->InputText[0] == '/') {
      CommandHandler(Chat->InputText, Chat, Camera, Player, World);
    } else {
      TraceLog(LOG_NONE, "%s", Chat->InputText);
      AddChatHistory(Chat, Chat->InputText);
    }

    Chat->LetterCount = 0;
    Chat->InputText[0] = '\0';
    Chat->ScrollOffset = 0;
    Chat->IsActive = false;
    DisableCursor();
  }
}

void UpdateChat(ChatState *Chat, Camera3D *Camera, Player *Player, World *World) {
  if (!Chat->IsActive) {
    if (IsKeyPressed(KEY_T)) {
      Chat->IsActive = true;
      EnableCursor();
      int Key = GetCharPressed();
      #pragma unroll 4
      for (int LoopIdx = 0; LoopIdx < CHAT_MAX_INPUT_CHARS; LoopIdx++) {
        if (Key > 0) {
          Key = GetCharPressed();
        } else {
          break;
        }
      }
    }
    return;
  }

  UpdateChatScroll(Chat);
  UpdateChatInput(Chat);
  HandleChatActions(Chat, Camera, Player, World);
}

void DrawChat(ChatState *Chat) {
  int HistoryBaseY = CHAT_POSITION_Y - CHAT_Y_MARGIN;
  double CurrentTime = GetTime();

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
      DrawRectangle(CHAT_POSITION_X, MsgY, (int)CHAT_HISTORY_WIDTH, CHAT_FONT_SIZE, Fade(BLACK, CHAT_HISTORY_BG_ALPHA * Alpha));
      DrawText(Chat->History[Idx].Text, CHAT_POSITION_X + CHAT_TEXT_PADDING, MsgY, CHAT_FONT_SIZE, Fade(WHITE, Alpha));
    }
  }

  if (Chat->IsActive) {
    DrawRectangle(CHAT_POSITION_X, CHAT_POSITION_Y, CHAT_WIDTH, CHAT_HEIGHT, Fade(BLACK, CHAT_BG_ALPHA));
    DrawRectangleLines(CHAT_POSITION_X, CHAT_POSITION_Y, CHAT_WIDTH, CHAT_HEIGHT, DARKGRAY);

    int TextY = CHAT_POSITION_Y + (CHAT_HEIGHT / 2) - (CHAT_FONT_SIZE / 2);
    DrawText(Chat->InputText, CHAT_POSITION_X + CHAT_TEXT_PADDING, TextY, CHAT_FONT_SIZE, WHITE);

    if (((int)(GetTime() * 2)) % 2 == 0) {
      int TextWidth = MeasureText(Chat->InputText, CHAT_FONT_SIZE);
      DrawText("|", CHAT_POSITION_X + CHAT_TEXT_PADDING + TextWidth, TextY, CHAT_FONT_SIZE, WHITE);
    }
  }
}
