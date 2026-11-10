#ifndef AUTOMATION_SCRIPT_H
#define AUTOMATION_SCRIPT_H

#include <stdbool.h>

#include "core/camera.h"
#include "player/player.h"
#include "ui/chat.h"
#include "world/world.h"

// Scripted run driver. A script is an alternative source of player intent and
// chat commands, so this is a client-side module: the simulation never learns a
// script is driving it. It is parsed before the window exists and then stepped
// once per frame from the main loop.

// Process exit codes a run can produce. 1 is left to the existing startup
// failure path, so a caller can tell "the game did not start" from "the script
// did not pass".
enum {
  SCRIPT_EXIT_OK = 0,
  SCRIPT_EXIT_FAILED = 2,
  SCRIPT_EXIT_TIMEOUT = 3
};

// Reads and parses Path, resolves OutDir against the current working directory
// and creates it, and opens the run log inside it. Must be called before the
// window and the working directory move with it. False means the run already
// failed and ScriptExitCode() holds the code to exit with; nothing needs to be
// torn down in that case beyond ScriptShutdown().
bool ScriptLoad(const char *Path, const char *OutDir, double TimeoutSeconds);

// True once a script has been loaded successfully, which is what puts the
// client in script mode: real keyboard and mouse input is ignored, and the
// world is not written to disk.
bool ScriptIsActive(void);

// Advances the script. Called once per frame at the top of the main loop,
// before input is polled, and runs statements until one blocks or the script
// ends.
void ScriptStep(ChatState *Chat, GameCamera *Camera, Player *PlayerVal,
                World *WorldVal);

// This frame's synthetic player intent, to be folded into the pending input
// the same way the polled intent is. Edge actions are handed over once and
// cleared; held axes keep reporting until their tick count runs out. Returns a
// neutral intent when no script is running.
PlayerInput ScriptPollInput(void);

// Number of simulation ticks the fixed-step loop consumed this frame, and the
// end of the rendered frame. Both feed the state-based waits, and the ticks
// also count down the held axes.
void ScriptRecordTicks(int Ticks);
void ScriptEndFrame(void);

// True once the run is over for any reason, so the main loop can leave.
bool ScriptRunEnded(void);

// The code the process should exit with. Zero when no script ran.
int ScriptExitCode(void);

// Writes the outcome line and closes the run log.
void ScriptShutdown(void);

#endif
