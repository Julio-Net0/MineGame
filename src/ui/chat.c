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
};

#define CHAT_HISTORY_BG_ALPHA 0.5F

void InitChat(ChatState *Chat) {
  Chat->InputText[0] = '\0';
  UiTextEditStateInit(&Chat->EditState);
  Chat->IsActive = false;
  Chat->HistoryCount = 0;
  Chat->ScrollOffset = 0;
  Chat->CommandHistory.Count = 0;
  Chat->HistoryBrowseIndex = -1;
  Chat->SavedDraft[0] = '\0';
  Chat->AutocompleteTabCount = 0;
  Chat->AutocompleteTokenStart = 0;
  Chat->AutocompleteBasePrefix[0] = '\0';
}

void AddChatHistory(ChatState *Chat, const char *Message) {
  if (Chat->HistoryCount < CHAT_MAX_HISTORY) {
    SafeStrncpy(Chat->History[Chat->HistoryCount].Text, Message, CHAT_MAX_INPUT_CHARS);
    Chat->History[Chat->HistoryCount].TimeCreated = PlatformGetTime();
    Chat->History[Chat->HistoryCount].WrapWidth = 0;
    Chat->HistoryCount++;
  } else {
    for (int Idx = 0; Idx < CHAT_MAX_HISTORY - 1; Idx++) {
      Chat->History[Idx] = Chat->History[Idx + 1];
    }
    SafeStrncpy(Chat->History[CHAT_MAX_HISTORY - 1].Text, Message, CHAT_MAX_INPUT_CHARS);
    Chat->History[CHAT_MAX_HISTORY - 1].TimeCreated = PlatformGetTime();
    Chat->History[CHAT_MAX_HISTORY - 1].WrapWidth = 0;
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

static void CloseChat(ChatState *Chat) {
  Chat->InputText[0] = '\0';
  UiTextEditStateReset(&Chat->EditState);
  Chat->ScrollOffset = 0;
  Chat->IsActive = false;
  Chat->HistoryBrowseIndex = -1;
  Chat->SavedDraft[0] = '\0';
  Chat->AutocompleteTabCount = 0;
  Chat->AutocompleteBasePrefix[0] = '\0';
  PlatformSetCursorShape(PLATFORM_CURSOR_DEFAULT);
  PlatformSetCursorDisabled(true);
}

static void RecordCommandHistory(ChatState *Chat, const char *Input) {
  if (Input == NULL || Input[0] == '\0') {
    return;
  }
  int Count = Chat->CommandHistory.Count;
  if (Count > 0 &&
      strncmp(Chat->CommandHistory.Entries[Count - 1], Input,
              CHAT_MAX_INPUT_CHARS) == 0) {
    return;
  }
  if (Count < CHAT_COMMAND_HISTORY_CAPACITY) {
    SafeStrncpy(Chat->CommandHistory.Entries[Count], Input,
                CHAT_MAX_INPUT_CHARS);
    Chat->CommandHistory.Count++;
  } else {
    memmove(&Chat->CommandHistory.Entries[0],
            &Chat->CommandHistory.Entries[1],
            (size_t)(CHAT_COMMAND_HISTORY_CAPACITY - 1) *
                sizeof(Chat->CommandHistory.Entries[0]));
    SafeStrncpy(Chat->CommandHistory.Entries[CHAT_COMMAND_HISTORY_CAPACITY - 1],
                Input, CHAT_MAX_INPUT_CHARS);
  }
}

static void HandleChatAction(ChatState *Chat, UiTextAction Action,
                             GameCamera *Camera, Player *Player, World *World) {
  if (Action == UI_TEXT_CANCEL) {
    CloseChat(Chat);
    return;
  }
  if (Action == UI_TEXT_SUBMIT) {
    RecordCommandHistory(Chat, Chat->InputText);
    Chat->HistoryBrowseIndex = -1;
    Chat->SavedDraft[0] = '\0';
    Chat->AutocompleteTabCount = 0;
    Chat->AutocompleteBasePrefix[0] = '\0';

    if (Chat->InputText[0] == '/') {
      (void)CommandHandler(Chat->InputText, Chat, Camera, Player, World);
    } else {
      LogInfo("%s", Chat->InputText);
      AddChatHistory(Chat, Chat->InputText);
    }
    CloseChat(Chat);
  }
}

void UpdateChat(ChatState *Chat, GameCamera *Camera, Player *Player, World *World) {
  if (!Chat->IsActive) {
    SystemInput System = PollSystemInput();
    if (System.ChatOpen || System.CommandOpen) {
      Chat->IsActive = true;
      UiTextEditStateReset(&Chat->EditState);
      Chat->HistoryBrowseIndex = -1;
      Chat->SavedDraft[0] = '\0';
      Chat->AutocompleteTabCount = 0;
      Chat->AutocompleteBasePrefix[0] = '\0';
      PlatformSetCursorDisabled(false);
      // Drain the frame's typed characters so the chat-open key is not entered.
      PollTextInput();
      if (System.CommandOpen) {
        Chat->InputText[0] = '/';
        Chat->InputText[1] = '\0';
        Chat->EditState.Cursor = 1;
      }
    }
    return;
  }

  UpdateChatScroll(Chat, InputGetScrollDelta());

  TextInput Text = PollTextInput();
  UiTextEvents Events = {
      .Chars = Text.Chars,
      .Count = Text.Count,
      .Backspace = Text.Backspace,
      .Delete = Text.Delete,
      .Submit = Text.Submit,
      .Cancel = Text.Cancel,
      .MoveLeft = Text.MoveLeft,
      .MoveRight = Text.MoveRight,
      .MoveHome = Text.MoveHome,
      .MoveEnd = Text.MoveEnd,
      .WordStep = Text.WordStep,
      .ShiftHeld = Text.ShiftHeld,
      .SelectAll = Text.SelectAll,
      .Copy = Text.Copy,
      .Cut = Text.Cut,
      .Paste = Text.Paste,
      .Undo = Text.Undo,
      .HistoryPrev = Text.HistoryPrev,
      .HistoryNext = Text.HistoryNext,
      .Autocomplete = Text.Autocomplete,
  };

  if (Events.HistoryPrev) {
    int Count = Chat->CommandHistory.Count;
    if (Count > 0) {
      if (Chat->HistoryBrowseIndex == -1) {
        SafeStrncpy(Chat->SavedDraft, Chat->InputText, (int)sizeof(Chat->SavedDraft));
        Chat->HistoryBrowseIndex = Count - 1;
      } else if (Chat->HistoryBrowseIndex > 0) {
        Chat->HistoryBrowseIndex--;
      }
      SafeStrncpy(Chat->InputText,
                  Chat->CommandHistory.Entries[Chat->HistoryBrowseIndex],
                  (int)sizeof(Chat->InputText));
      int Len = (int)strlen(Chat->InputText);
      Chat->EditState.Cursor = Len;
      Chat->EditState.SelectionStart = -1;
      Chat->EditState.SelectionEnd = -1;
      Chat->AutocompleteTabCount = 0;
      Chat->AutocompleteBasePrefix[0] = '\0';
    }
  } else if (Events.HistoryNext) {
    if (Chat->HistoryBrowseIndex != -1) {
      int Count = Chat->CommandHistory.Count;
      if (Chat->HistoryBrowseIndex < Count - 1) {
        Chat->HistoryBrowseIndex++;
        SafeStrncpy(Chat->InputText,
                    Chat->CommandHistory.Entries[Chat->HistoryBrowseIndex],
                    (int)sizeof(Chat->InputText));
      } else {
        Chat->HistoryBrowseIndex = -1;
        SafeStrncpy(Chat->InputText, Chat->SavedDraft,
                    (int)sizeof(Chat->InputText));
      }
      int Len = (int)strlen(Chat->InputText);
      Chat->EditState.Cursor = Len;
      Chat->EditState.SelectionStart = -1;
      Chat->EditState.SelectionEnd = -1;
      Chat->AutocompleteTabCount = 0;
      Chat->AutocompleteBasePrefix[0] = '\0';
    }
  }

  if (Events.Autocomplete) {
    if (Chat->InputText[0] == '/') {
      char Completed[CHAT_MAX_INPUT_CHARS + 1];
      int NewCursor = Chat->EditState.Cursor;
      int Matches = CommandAutocomplete(
          Chat->InputText, Chat->EditState.Cursor, Completed,
          (int)sizeof(Completed), &NewCursor, Chat, Chat->AutocompleteTabCount);
      if (Matches > 0) {
        SafeStrncpy(Chat->InputText, Completed, (int)sizeof(Chat->InputText));
        Chat->EditState.Cursor = NewCursor;
        Chat->EditState.SelectionStart = -1;
        Chat->EditState.SelectionEnd = -1;
        if (Matches > 1) {
          Chat->AutocompleteTabCount++;
        } else {
          Chat->AutocompleteTabCount = 0;
        }
      }
    }
  } else if (Events.Count > 0 || Events.Backspace || Events.Delete ||
             Events.Paste || Events.Cut) {
    Chat->AutocompleteTabCount = 0;
    Chat->AutocompleteBasePrefix[0] = '\0';
  }

  UiTextAction Action = UiTextFieldEdit(
      Chat->InputText, (int)sizeof(Chat->InputText), &Chat->EditState, &Events);
  HandleChatAction(Chat, Action, Camera, Player, World);
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

static void EnsureWrapped(ChatMessage *Message, int Width) {
  if (Message->WrapWidth == Width) {
    return;
  }
  Message->WrappedLineCount = UiWrapText(Message->Text, (float)Width,
                                         Message->WrappedLineStarts,
                                         CHAT_MAX_WRAPPED_LINES);
  Message->WrapWidth = Width;
}

void DrawChat(UiContext *Ctx, ChatState *Chat) {
  double CurrentTime = PlatformGetTime();
  int MaxRows = Chat->IsActive ? CHAT_FOCUSED_LINES : CHAT_UNFOCUSED_LINES;
  int LineWidth = CHAT_LINE_WIDTH;
  if (LineWidth > Ctx->GuiWidth - (CHAT_TEXT_INSET * 2)) {
    LineWidth = Ctx->GuiWidth - (CHAT_TEXT_INSET * 2);
  }

  // Rows fill upward from the bottom, newest message first; a wrapped message
  // keeps its own lines in reading order, so its last line is the lowest.
  int Row = 0;
  for (int Idx = Chat->HistoryCount - 1 - Chat->ScrollOffset;
       Idx >= 0 && Row < MaxRows; Idx--) {
    ChatMessage *Message = &Chat->History[Idx];
    float Alpha = HistoryAlpha(Chat, Message, CurrentTime);
    if (Alpha <= 0.0F) {
      continue;
    }
    EnsureWrapped(Message, LineWidth);

    Color8 Background = Color8Alpha(COLOR_BLACK, CHAT_HISTORY_BG_ALPHA * Alpha);
    Color8 Foreground = Color8Alpha(COLOR_WHITE, Alpha);
    for (int Line = Message->WrappedLineCount - 1; Line >= 0 && Row < MaxRows;
         Line--) {
      int Top =
          Ctx->GuiHeight - CHAT_BOTTOM_OFFSET - ((Row + 1) * FONT_LINE_HEIGHT);
      if (Top < 0) {
        Row = MaxRows;
        break;
      }
      int Start = Message->WrappedLineStarts[Line];
      int Length = (Line + 1 < Message->WrappedLineCount)
                       ? (Message->WrappedLineStarts[Line + 1] - Start)
                       : -1;
      UiFillRect(Ctx,
                 (UiRect){0, Top, LineWidth + (CHAT_TEXT_INSET * 2),
                          FONT_LINE_HEIGHT},
                 Background);
      UiTextRangeShadow(Ctx, &Message->Text[Start], Length,
                        (float)CHAT_TEXT_INSET, (float)(Top + 1), Foreground);
      Row++;
    }
  }

  if (Chat->IsActive) {
    if (Row > 0) {
      int TopBound =
          Ctx->GuiHeight - CHAT_BOTTOM_OFFSET - (Row * FONT_LINE_HEIGHT);
      int BottomBound = Ctx->GuiHeight - CHAT_BOTTOM_OFFSET;
      float Px = Ctx->Pointer.X;
      float Py = Ctx->Pointer.Y;
      if (Px >= 0.0F && Px <= (float)(LineWidth + (CHAT_TEXT_INSET * 2)) &&
          Py >= (float)TopBound && Py <= (float)BottomBound) {
        Ctx->CursorShape = PLATFORM_CURSOR_IBEAM;
      }
    }
    UiTextFieldDraw(Ctx,
                    (UiRect){CHAT_INPUT_MARGIN,
                             Ctx->GuiHeight - CHAT_INPUT_MARGIN -
                                 CHAT_INPUT_HEIGHT,
                             Ctx->GuiWidth - (CHAT_INPUT_MARGIN * 2),
                             CHAT_INPUT_HEIGHT},
                    Chat->InputText, &Chat->EditState, true, false);
  }
}
