#ifndef COMMANDS_H
#define COMMANDS_H

#include "world/block_system.h"
#include "ui/chat.h"
#include "core/camera.h"
#include "player/player.h"
#include "world/world.h"

typedef struct {
  ChatState *Chat;
  GameCamera *Camera;
  BlockType *BlockRegistry;
  int BlockCount;
  Player *Player;
  World *World;
} CommandContext;

typedef void (*CommandFunc)(const char *Args, CommandContext *Ctx);

typedef struct {
  const char *Name;
  const char *Use;
  const char *Description;
  CommandFunc Function;
} CommandInfo;

typedef void (*DebugFunc)(CommandContext *Ctx, bool State);

typedef struct {
  const char *Name;
  const char *Description;
  DebugFunc Func;
} DebugToggle;

// Runs one chat command. False means the command name was not recognized,
// which a scripted run treats as a failed step; a recognized command that
// rejects its own arguments still returns true, having reported why.
bool CommandHandler(char *Command, ChatState *Chat, GameCamera *Camera, Player *Player, World *World);

// Autocompletes commands and arguments based on the current input buffer and
// cursor position. Populates OutCompleted and OutNewCursor. Returns the number
// of matching candidates found (0 = no match, 1 = unique match, >1 = multiple).
int CommandAutocomplete(const char *Buffer, int CursorPos, char *OutCompleted,
                        int Capacity, int *OutNewCursor, ChatState *Chat,
                        int CycleIndex);

// Optional sink for every line a command reports, so a scripted run can record
// what the commands answered without the chat being visible. Null clears it.
typedef void (*CommandOutputSink)(const char *Message);
void SetCommandOutputSink(CommandOutputSink Sink);

// Once per frame: reports to the chat anything a command started that has
// since finished on its own, such as a timed profiler capture.
void PollCommandEvents(ChatState *Chat);

#endif
