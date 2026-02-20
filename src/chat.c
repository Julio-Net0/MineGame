#include <stdbool.h>
#include "raylib.h"
#include "chat.h"
#include "commands.h"

void InitChat(ChatState *chat){
  chat->inputText[0] = '\0';
  chat->letterCount = 0;
  chat->isActive = false;
}
void UpdateChat(ChatState *chat) {
    if (!chat->isActive) {
        if (IsKeyPressed(KEY_T)) {
            chat->isActive = true;
            while (GetCharPressed() > 0) {}
        }
    } else {
        if (IsKeyPressed(KEY_DELETE)) {
            chat->isActive = false;
        }

        int key = GetCharPressed();
        while (key > 0) {
            if ((key >= 32) && (key <= 125) && (chat->letterCount < CHAT_MAX_INPUT_CHARS)) {
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

        if(IsKeyPressed(KEY_ENTER)) {

          if(chat->inputText[0] == '/'){
            CommandHandler(chat->inputText);
          }

          TraceLog(LOG_NONE, chat->inputText);
          chat->letterCount = 0;
          chat->inputText[0] = '\0';
          chat->isActive = false;
        }
    }
}

void DrawChat(ChatState *chat) {
    if (chat->isActive) {
        DrawRectangle(CHAT_POSITION_X, CHAT_POSITION_Y, CHAT_WIDTH, CHAT_HEIGHT, Fade(BLACK, 0.5F));
        DrawRectangleLines(CHAT_POSITION_X, CHAT_POSITION_Y, CHAT_WIDTH, CHAT_HEIGHT, DARKGRAY);

        int textY = CHAT_POSITION_Y + (CHAT_HEIGHT / 2) - 10;
        DrawText(chat->inputText, CHAT_POSITION_X + 10, textY, 20, WHITE);

        if (((int)(GetTime() * 2)) % 2 == 0) {
            int textWidth = MeasureText(chat->inputText, 20);
            DrawText("|", CHAT_POSITION_X + 10 + textWidth, textY, 20, WHITE);
        }
    }
}

