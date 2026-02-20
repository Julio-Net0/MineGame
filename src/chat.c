#include <stdbool.h>
#include <string.h>
#include "raylib.h"
#include "chat.h"
#include "commands.h"

void InitChat(ChatState *chat){
  chat->inputText[0] = '\0';
  chat->letterCount = 0;
  chat->isActive = false;
  chat->historyCount = 0;
  chat->scrollOffset = 0;
}

void AddChatHistory(ChatState *chat, const char *message){
  if(chat->historyCount < CHAT_MAX_HISTORY){
    strncpy(chat->history[chat->historyCount]->text, message, CHAT_MAX_INPUT_CHARS - 1);
    chat->history[chat->historyCount]->text[CHAT_MAX_INPUT_CHARS - 1] = '\0';
    chat->history[chat->historyCount]->timeCreated = GetTime();
    chat->historyCount++;
  }else{
    for(int i = 0; i < CHAT_MAX_HISTORY - 1; i++){
      strcpy(chat->history[i]->text, chat->history[i + 1]->text);

      chat->history[i]->timeCreated = chat->history[i + 1]->timeCreated;
    }

    strncpy(chat->history[CHAT_MAX_HISTORY - 1]->text, message, CHAT_MAX_INPUT_CHARS - 1);
    chat->history[CHAT_MAX_HISTORY - 1]->text[CHAT_MAX_INPUT_CHARS - 1] = '\0';

    chat->history[CHAT_MAX_HISTORY - 1]->timeCreated = GetTime();
  }
}

static void UpdateChatScroll(ChatState *chat){
  float wheel = GetMouseWheelMove();
  if(wheel != 0.0F){
    chat->scrollOffset += (int)wheel;

    int maxVisible = (CHAT_POSITION_Y - CHAT_HISTORY_LIMIT_Y) / CHAT_FONT_SIZE;

    int maxScroll = chat->historyCount - maxVisible + 1;
    if(maxScroll < 0){ maxScroll = 0; }

    if(chat->scrollOffset > maxScroll) { chat->scrollOffset = maxScroll; }
    if(chat->scrollOffset < 0) { chat->scrollOffset = 0; }
  }
}

static void UpdateChatInput(ChatState *chat){
  int key = GetCharPressed();
  while (key > 0) {
    if ((key >= ' ') && (key <= '~') && (chat->letterCount < CHAT_MAX_INPUT_CHARS)) {
      chat->inputText[chat->letterCount] = (char)key;
      chat->inputText[chat->letterCount + 1] = '\0';
      chat->letterCount++;
    }
    key = GetCharPressed();
  }

  if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) {
    chat->letterCount--;
    if (chat->letterCount < 0) { chat->letterCount = 0; }
    chat->inputText[chat->letterCount] = '\0';
  }
}

static void HandleChatActions(ChatState *chat){
  if (IsKeyPressed(KEY_DELETE)) {
    chat->scrollOffset = 0;
    chat->isActive = false;
    DisableCursor();
  }

  if(IsKeyPressed(KEY_ENTER)) {

    if(chat->inputText[0] == '/'){
      CommandHandler(chat->inputText);
    }

    TraceLog(LOG_NONE, chat->inputText);
    AddChatHistory(chat, chat->inputText);

    chat->letterCount = 0;
    chat->inputText[0] = '\0';
    chat->scrollOffset = 0;
    chat->isActive = false;
    DisableCursor();
  }
}

void UpdateChat(ChatState *chat) {
  if (!chat->isActive) {
    if (IsKeyPressed(KEY_T)) {
      chat->isActive = true;
      EnableCursor();
      while (GetCharPressed() > 0) {}
    }
    return;
  }

  UpdateChatScroll(chat);
  UpdateChatInput(chat);
  HandleChatActions(chat);
}

void DrawChat(ChatState *chat) {

  int historyBaseY = CHAT_POSITION_Y - CHAT_Y_MARGIN;
  double currentTime = GetTime();

  for(int i = 0; i < chat->historyCount; i++){

    int msgY = historyBaseY - ((chat->historyCount - i - chat->scrollOffset) * CHAT_FONT_SIZE);

    if(msgY < CHAT_HISTORY_LIMIT_Y || msgY >= historyBaseY){
      continue;
    }

    float age = (float)(currentTime - chat->history[i]->timeCreated);
    float alpha = 1.0F;

    if(!chat->isActive){
      if(age < CHAT_DISPLAY_TIME){
        alpha = 1.0F;
      }else if(age < CHAT_DISPLAY_TIME + CHAT_FADE_TIME){
        alpha = 1.0F - ((age - CHAT_DISPLAY_TIME) / CHAT_FADE_TIME);
      }else{
        alpha = 0.0F;
      }
    }

    if(alpha > 0.0F){
      DrawRectangle(CHAT_POSITION_X, msgY, CHAT_HISTORY_WIDTH, CHAT_FONT_SIZE, Fade(BLACK, CHAT_HISTORY_BG_ALPHA * alpha));
      DrawText(chat->history[i]->text, CHAT_POSITION_X + CHAT_TEXT_PADDING, msgY, CHAT_FONT_SIZE, Fade(WHITE, alpha));
    }
  }


  if (chat->isActive) {
    DrawRectangle(CHAT_POSITION_X, CHAT_POSITION_Y, CHAT_WIDTH, CHAT_HEIGHT, Fade(BLACK, CHAT_BG_ALPHA));
    DrawRectangleLines(CHAT_POSITION_X, CHAT_POSITION_Y, CHAT_WIDTH, CHAT_HEIGHT, DARKGRAY);

    int textY = CHAT_POSITION_Y + (CHAT_HEIGHT / 2) - (CHAT_FONT_SIZE / 2);
    DrawText(chat->inputText, CHAT_POSITION_X + CHAT_TEXT_PADDING, textY, CHAT_FONT_SIZE, WHITE);

    if (((int)(GetTime() * 2)) % 2 == 0) {
      int textWidth = MeasureText(chat->inputText, CHAT_FONT_SIZE);
      DrawText("|", CHAT_POSITION_X + CHAT_TEXT_PADDING + textWidth, textY, CHAT_FONT_SIZE, WHITE);
    }
  }
}

