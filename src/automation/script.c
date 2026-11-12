#include "automation/script.h"
#include "automation/worldgen_preview.h"

#include "core/fileio.h"
#include "core/image.h"
#include "core/log.h"
#include "core/profiler.h"
#include "core/utils.h"
#include "platform/platform.h"
#include "render/backend.h"
#include "render/camera.h"
#include "render/renderer.h"
#include "third_party/cJSON.h"
#include "ui/commands.h"
#include "ui/creative_inventory.h"
#include "ui/debug.h"
#include "persistence/world_save.h"
#include "world/block_system.h"
#include "world/chunk.h"
#include "world/surface.h"
#include "world/terrain_density.h"
#include "world/worldgen_column_cache.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define ScriptMakeDir(Path) _mkdir(Path)
#define ScriptGetCwd(Buf, Size) _getcwd(Buf, Size)
#else
#include <sys/stat.h>
#include <unistd.h>
#define SCRIPT_DIR_MODE 0777
#define ScriptMakeDir(Path) mkdir(Path, SCRIPT_DIR_MODE)
#define ScriptGetCwd(Buf, Size) getcwd(Buf, Size)
#endif

enum {
  SCRIPT_MAX_STEPS = 512,
  SCRIPT_MAX_TEXT = CHAT_MAX_INPUT_CHARS,
  SCRIPT_MAX_NAME = 64,
  WORLDGEN_PREVIEW_TOKENS = 5,
  // worldgen_biome_map <name> <centerX> <centerZ> <size> <step> is the
  // longest form.
  WORLDGEN_BIOME_MAP_TOKENS = 6,
  SCRIPT_MAX_TOKENS = WORLDGEN_BIOME_MAP_TOKENS,
  SCRIPT_LOG_LINE_CHARS = 512,
  SCRIPT_DECIMAL_BASE = 10,

  // Chunk loading settles in bursts: a worker finishes a chunk, the main thread
  // notices on the next frame and dirties its neighbours, and for an instant in
  // between nothing is in flight and the world looks done. Requiring the quiet
  // state to hold for several consecutive frames is what tells a real finish
  // from one of those gaps.
  SETTLED_FRAMES_REQUIRED = 8
};

#define SCRIPT_LOG_NAME "run.log"

// What the runner acts on. Gathered once per frame by the caller rather than
// stored, so the runner holds no pointer into state it does not own.
typedef struct {
  ChatState *Chat;
  GameCamera *Camera;
  Player *PlayerVal;
  World *WorldVal;
  int RenderDistance;
} ScriptContext;

typedef enum {
  SCRIPT_OP_COMMAND,
  SCRIPT_OP_WAIT_FRAMES,
  SCRIPT_OP_WAIT_TICKS,
  SCRIPT_OP_WAIT_LOADED,
  SCRIPT_OP_LOOK,
  SCRIPT_OP_HOLD,
  SCRIPT_OP_PRESS,
  SCRIPT_OP_HOTBAR,
  SCRIPT_OP_SCREENSHOT,
  SCRIPT_OP_STATE,
  SCRIPT_OP_INVENTORY,
  SCRIPT_OP_WORLDGEN_PREVIEW,
  SCRIPT_OP_WORLDGEN_BIOME_MAP,
  SCRIPT_OP_QUIT
} ScriptOpKind;

// What an inventory statement does; its argument travels in Count.
typedef enum {
  SCRIPT_INV_OPEN,
  SCRIPT_INV_CLOSE,
  SCRIPT_INV_TAB,
  SCRIPT_INV_SCROLL,
  SCRIPT_INV_HOVER,
  SCRIPT_INV_PICK,
  SCRIPT_INV_PUT
} ScriptInventoryAction;

// Movement axes a hold statement can drive. They are pairs on the same axis of
// the abstract intent, so forward and back held at once cancel out exactly as
// pressing both keys does.
typedef enum {
  SCRIPT_AXIS_FORWARD,
  SCRIPT_AXIS_BACK,
  SCRIPT_AXIS_LEFT,
  SCRIPT_AXIS_RIGHT,
  SCRIPT_AXIS_ASCEND,
  SCRIPT_AXIS_DESCEND,
  SCRIPT_AXIS_SPRINT,
  SCRIPT_AXIS_COUNT
} ScriptAxis;

typedef enum {
  SCRIPT_PRESS_JUMP,
  SCRIPT_PRESS_BREAK,
  SCRIPT_PRESS_PLACE,
  SCRIPT_PRESS_PICK
} ScriptPressKind;

// One parsed statement. Text always holds the source line as written, which is
// what the run log echoes; for a command statement it is also the command.
typedef struct {
  ScriptOpKind Kind;
  int Line;
  int Count;
  // Which axis, press kind or hotbar slot the statement names; unused by the
  // statements that take no such choice.
  int Index;
  float Yaw;
  float Pitch;
  // worldgen_preview / worldgen_biome_map center column, or the
  // generator-derived spawn column when PreviewAtSpawn is set; the size travels
  // in Count and the biome map's step in Index.
  int PreviewX;
  int PreviewZ;
  bool PreviewAtSpawn;
  char Name[SCRIPT_MAX_NAME];
  char Text[SCRIPT_MAX_TEXT];
} ScriptStatement;

typedef enum {
  SCRIPT_WAIT_NONE,
  SCRIPT_WAIT_FRAMES,
  SCRIPT_WAIT_TICKS,
  // Waiting for chunk loading to stop changing. Released only after the world
  // has reported itself settled for several frames in a row -- see
  // SETTLED_FRAMES_REQUIRED.
  SCRIPT_WAIT_LOADED,
  // Waiting for the frame requested by a screenshot statement to come back:
  // the request reaches the backend at the top of a frame and the pixels only
  // exist once that frame has been drawn and read, one frame later.
  SCRIPT_WAIT_CAPTURE
} ScriptWaitKind;

typedef struct {
  bool Active;
  bool Ended;
  int ExitCode;

  ScriptStatement Steps[SCRIPT_MAX_STEPS];
  int StepCount;
  int Pc;

  ScriptWaitKind Wait;
  long long WaitTarget;
  int WaitLine;
  int SettledFrames;
  char WaitName[SCRIPT_MAX_NAME];

  long long Frame;
  long long Tick;

  // Synthetic intent. Holds are counted down as the fixed-step loop consumes
  // ticks; edges are latched here and handed over once.
  int HoldTicks[SCRIPT_AXIS_COUNT];
  bool PressJump;
  bool PressBreak;
  bool PressPlace;
  bool PressPick;
  int HotbarSelect;

  double TimeoutSeconds;
  double Deadline;
  bool DeadlineArmed;

  FILE *Log;
  char OutDir[FILEIO_MAX_PATH];
} ScriptState;

static ScriptState *GetScriptState(void) {
  static ScriptState State;
  return &State;
}

// Flushed per line: a run that ends in a crash or a kill still leaves a log
// that says how far it got, which is the only record such a run produces.
static void ScriptLog(const char *Format, ...) {
  ScriptState *State = GetScriptState();
  if (State->Log == (FILE *)0) {
    return;
  }

  char Line[SCRIPT_LOG_LINE_CHARS];
  va_list Args;
  va_start(Args, Format);
  (void)vsnprintf(Line, sizeof(Line), Format, Args);
  va_end(Args);

  (void)fprintf(State->Log, "%s\n", Line);
  (void)fflush(State->Log);
}

static void ScriptCommandOutput(const char *Message) {
  ScriptLog("      | %s", Message);
}

static void ScriptFinish(int ExitCode, const char *Outcome) {
  ScriptState *State = GetScriptState();
  State->Ended = true;
  State->ExitCode = ExitCode;
  ScriptLog("%s", Outcome);
}

static void ScriptFail(int Line, const char *Reason) {
  LogError("SCRIPT: line %d: %s", Line, Reason);
  char Outcome[SCRIPT_LOG_LINE_CHARS];
  (void)snprintf(Outcome, sizeof(Outcome), "FAILED at line %d: %s", Line,
                 Reason);
  ScriptFinish(SCRIPT_EXIT_FAILED, Outcome);
}

static bool IsAbsolutePath(const char *Path) {
  if (Path[0] == '\0') {
    return false;
  }
#ifdef _WIN32
  if (Path[0] == '/' || Path[0] == '\\') {
    return true;
  }
  return Path[1] == ':';
#else
  return Path[0] == '/';
#endif
}

// Resolved against the working directory the process was launched from, before
// the platform layer anchors it to the executable: output then lands where the
// run was started, not next to the binary.
static bool ResolveOutDir(const char *OutDir, char *Out, size_t OutSize) {
  if (IsAbsolutePath(OutDir)) {
    if (strlen(OutDir) >= OutSize) {
      return false;
    }
    SafeStrncpy(Out, OutDir, (int)OutSize);
    return true;
  }

  char Cwd[FILEIO_MAX_PATH];
  if (ScriptGetCwd(Cwd, (int)sizeof(Cwd)) == (char *)0) {
    return false;
  }
  int Written = snprintf(Out, OutSize, "%s/%s", Cwd, OutDir);
  return Written > 0 && (size_t)Written < OutSize;
}

static bool OpenRunLog(const char *OutDir) {
  ScriptState *State = GetScriptState();

  if (!IsDirectory(OutDir) && ScriptMakeDir(OutDir) != 0 &&
      !IsDirectory(OutDir)) {
    LogError("SCRIPT: cannot create output directory \"%s\" (%s)", OutDir,
             strerror(errno));
    return false;
  }

  char Path[FILEIO_MAX_PATH];
  int Written = snprintf(Path, sizeof(Path), "%s/%s", OutDir, SCRIPT_LOG_NAME);
  if (Written <= 0 || (size_t)Written >= sizeof(Path)) {
    LogError("SCRIPT: output directory path is too long");
    return false;
  }

  State->Log = fopen(Path, "w");
  if (State->Log == (FILE *)0) {
    LogError("SCRIPT: cannot write \"%s\" (%s)", Path, strerror(errno));
    return false;
  }
  return true;
}

static char *SkipSpaces(char *Text) {
  while (*Text == ' ' || *Text == '\t') {
    Text++;
  }
  return Text;
}

static void TrimTrailingSpaces(char *Text) {
  size_t Len = strlen(Text);
  while (Len > 0 && (Text[Len - 1] == ' ' || Text[Len - 1] == '\t' ||
                     Text[Len - 1] == '\r')) {
    Text[--Len] = '\0';
  }
}

// Splits Line in place on runs of spaces and tabs. Returns the token count,
// which may exceed MaxTokens; the caller rejects that as a wrong arity.
static int SplitTokens(char *Line, char **Tokens, int MaxTokens) {
  int Count = 0;
  char *Cursor = Line;
  while (*Cursor != '\0') {
    Cursor = SkipSpaces(Cursor);
    if (*Cursor == '\0') {
      break;
    }
    if (Count < MaxTokens) {
      Tokens[Count] = Cursor;
    }
    Count++;
    while (*Cursor != '\0' && *Cursor != ' ' && *Cursor != '\t') {
      Cursor++;
    }
    if (*Cursor != '\0') {
      *Cursor = '\0';
      Cursor++;
    }
  }
  return Count;
}

static bool ParsePositiveInt(const char *Str, int *OutValue) {
  errno = 0;
  char *End = (char *)0;
  long Value = strtol(Str, &End, SCRIPT_DECIMAL_BASE);
  if (errno == ERANGE || End == Str || *End != '\0' || Value < 1 ||
      Value > INT32_MAX) {
    return false;
  }
  *OutValue = (int)Value;
  return true;
}

static bool ParseSignedInt(const char *Str, int *OutValue) {
  errno = 0;
  char *End = (char *)0;
  long Value = strtol(Str, &End, SCRIPT_DECIMAL_BASE);
  if (errno == ERANGE || End == Str || *End != '\0' || Value < INT32_MIN ||
      Value > INT32_MAX) {
    return false;
  }
  *OutValue = (int)Value;
  return true;
}

static bool ParseFinite(const char *Str, float *OutValue) {
  errno = 0;
  char *End = (char *)0;
  double Value = strtod(Str, &End);
  if (errno == ERANGE || End == Str || *End != '\0' || !isfinite(Value)) {
    return false;
  }
  *OutValue = (float)Value;
  return true;
}

// Names accepted by `hold`, in ScriptAxis order.
static const char *const AXIS_NAMES[SCRIPT_AXIS_COUNT] = {
    "forward", "back", "left", "right", "ascend", "descend", "sprint"};

static int FindAxis(const char *Name) {
  for (int Idx = 0; Idx < SCRIPT_AXIS_COUNT; Idx++) {
    if (CompareString(Name, AXIS_NAMES[Idx]) == 0) {
      return Idx;
    }
  }
  return -1;
}

static void ParseError(int Line, const char *Reason) {
  LogError("SCRIPT: line %d: %s", Line, Reason);
  ScriptLog("PARSE ERROR at line %d: %s", Line, Reason);
}

// Appends one statement. Text is the trimmed line, kept for the log, and the
// tokens the parser reads are its in-place split, so the line must be copied
// before they are consulted -- both point into the same buffer.
static bool AppendStatement(ScriptStatement Step, const char *Name,
                            const char *Text) {
  ScriptState *State = GetScriptState();
  if (State->StepCount >= SCRIPT_MAX_STEPS) {
    ParseError(Step.Line, "too many statements");
    return false;
  }
  SafeStrncpy(Step.Name, Name, (int)sizeof(Step.Name));
  SafeStrncpy(Step.Text, Text, (int)sizeof(Step.Text));
  State->Steps[State->StepCount] = Step;
  State->StepCount++;
  return true;
}

// An output name becomes part of a file path, so it is restricted to characters
// that cannot climb out of the output directory or name a device: no dot, no
// separator, nothing but letters, digits, underscore and dash.
static bool IsSafeOutputName(const char *Name) {
  if (Name[0] == '\0' || strlen(Name) >= SCRIPT_MAX_NAME) {
    return false;
  }
  for (const char *Cursor = Name; *Cursor != '\0'; Cursor++) {
    char Char = *Cursor;
    bool Allowed = (Char >= 'a' && Char <= 'z') || (Char >= 'A' && Char <= 'Z') ||
                   (Char >= '0' && Char <= '9') || Char == '_' || Char == '-';
    if (!Allowed) {
      return false;
    }
  }
  return true;
}

// "worldgen_biome_map <name> <centerX|spawn> <centerZ|spawn> <size> <step>".
static bool ParseWorldgenBiomeMap(char **Tokens, int TokenCount, int LineNumber,
                                  const char *LineText) {
  if (TokenCount != WORLDGEN_BIOME_MAP_TOKENS) {
    ParseError(LineNumber, "expected \"worldgen_biome_map <name> <centerX> "
                           "<centerZ> <size> <step>\" (center may be \"spawn "
                           "spawn\")");
    return false;
  }
  if (!IsSafeOutputName(Tokens[1])) {
    ParseError(LineNumber, "worldgen_biome_map name must be 1-63 characters "
                           "of [A-Za-z0-9_-]");
    return false;
  }

  int CenterX = 0;
  int CenterZ = 0;
  int Size = 0;
  int Step = 0;
  bool AtSpawn = CompareString(Tokens[2], "spawn") == 0 &&
                 CompareString(Tokens[3], "spawn") == 0;
  if ((!AtSpawn && (!ParseSignedInt(Tokens[2], &CenterX) ||
                    !ParseSignedInt(Tokens[3], &CenterZ))) ||
      !ParseSignedInt(Tokens[4], &Size) ||
      !ParseSignedInt(Tokens[WORLDGEN_BIOME_MAP_TOKENS - 1], &Step)) {
    ParseError(LineNumber, "worldgen_biome_map center must be two integers or "
                           "\"spawn spawn\", and size and step integers");
    return false;
  }
  if (Size <= 0 || Step <= 0 || Size % Step != 0 ||
      Size / Step > WORLDGEN_PREVIEW_MAX_SIZE) {
    ParseError(LineNumber, "worldgen_biome_map size must be a positive "
                           "multiple of step with size / step <= 1024");
    return false;
  }

  return AppendStatement((ScriptStatement){.Kind = SCRIPT_OP_WORLDGEN_BIOME_MAP,
                                           .Line = LineNumber,
                                           .Count = Size,
                                           .Index = Step,
                                           .PreviewX = CenterX,
                                           .PreviewZ = CenterZ,
                                           .PreviewAtSpawn = AtSpawn},
                         Tokens[1], LineText);
}

// "inventory <action> [argument]". Arguments are validated here; whether the
// action can apply (the screen open, the entry present) is only known when the
// statement runs.
static bool ParseWorldgenPreview(char **Tokens, int TokenCount, int LineNumber,
                                 const char *LineText) {
  if (TokenCount != WORLDGEN_PREVIEW_TOKENS) {
    ParseError(LineNumber, "expected \"worldgen_preview <name> <centerX> "
                           "<centerZ> <size>\" (center may be \"spawn spawn\")");
    return false;
  }
  if (!IsSafeOutputName(Tokens[1])) {
    ParseError(LineNumber, "worldgen_preview name must be 1-63 characters of "
                           "[A-Za-z0-9_-]");
    return false;
  }

  int CenterX = 0;
  int CenterZ = 0;
  int Size = 0;
  bool AtSpawn = CompareString(Tokens[2], "spawn") == 0 &&
                 CompareString(Tokens[3], "spawn") == 0;
  if ((!AtSpawn && (!ParseSignedInt(Tokens[2], &CenterX) ||
                    !ParseSignedInt(Tokens[3], &CenterZ))) ||
      !ParseSignedInt(Tokens[4], &Size)) {
    ParseError(LineNumber, "worldgen_preview center must be two integers or "
                           "\"spawn spawn\", and size an integer");
    return false;
  }
  if (Size < WORLDGEN_PREVIEW_MIN_SIZE || Size > WORLDGEN_PREVIEW_MAX_SIZE) {
    ParseError(LineNumber, "worldgen_preview size must be within 1..1024");
    return false;
  }

  return AppendStatement((ScriptStatement){.Kind = SCRIPT_OP_WORLDGEN_PREVIEW,
                                           .Line = LineNumber,
                                           .Count = Size,
                                           .PreviewX = CenterX,
                                           .PreviewZ = CenterZ,
                                           .PreviewAtSpawn = AtSpawn},
                         Tokens[1], LineText);
}

static bool ParseInventoryStatement(char **Tokens, int TokenCount,
                                    int LineNumber, const char *LineText) {
  static const char *const USAGE =
      "expected \"inventory <open|close|tab <building|colored|natural>|"
      "scroll <rows>|hover <n|off>|pick <n>|put <1-9>>\"";
  ScriptStatement Step = {.Kind = SCRIPT_OP_INVENTORY, .Line = LineNumber};
  bool Valid = false;
  const char *Action = (TokenCount >= 2) ? Tokens[1] : "";
  const char *Argument = (TokenCount == 3) ? Tokens[2] : "";

  if (TokenCount == 2 && CompareString(Action, "open") == 0) {
    Step.Index = SCRIPT_INV_OPEN;
    Valid = true;
  } else if (TokenCount == 2 && CompareString(Action, "close") == 0) {
    Step.Index = SCRIPT_INV_CLOSE;
    Valid = true;
  } else if (TokenCount == 3 && CompareString(Action, "tab") == 0) {
    Step.Index = SCRIPT_INV_TAB;
    for (int Tab = 0; Tab < INVENTORY_TAB_COUNT; Tab++) {
      if (CompareString(Argument,
                        CreativeInventoryTabName((InventoryTab)Tab)) == 0) {
        Step.Count = Tab;
        Valid = true;
      }
    }
  } else if (TokenCount == 3 && CompareString(Action, "scroll") == 0) {
    Step.Index = SCRIPT_INV_SCROLL;
    Valid = ParseSignedInt(Argument, &Step.Count);
  } else if (TokenCount == 3 && CompareString(Action, "hover") == 0) {
    Step.Index = SCRIPT_INV_HOVER;
    Step.Count = 0;
    Valid = CompareString(Argument, "off") == 0 ||
            ParsePositiveInt(Argument, &Step.Count);
  } else if (TokenCount == 3 && CompareString(Action, "pick") == 0) {
    Step.Index = SCRIPT_INV_PICK;
    Valid = ParsePositiveInt(Argument, &Step.Count);
  } else if (TokenCount == 3 && CompareString(Action, "put") == 0) {
    Step.Index = SCRIPT_INV_PUT;
    Valid = ParsePositiveInt(Argument, &Step.Count) &&
            Step.Count <= HOTBAR_SIZE;
  }

  if (!Valid) {
    ParseError(LineNumber, USAGE);
    return false;
  }
  return AppendStatement(Step, "", LineText);
}

static bool ParseStatement(char *Statement, int LineNumber) {
  char LineText[SCRIPT_MAX_TEXT];
  if (strlen(Statement) >= sizeof(LineText)) {
    ParseError(LineNumber, "statement is too long");
    return false;
  }
  SafeStrncpy(LineText, Statement, (int)sizeof(LineText));

  if (Statement[0] == '/') {
    return AppendStatement(
        (ScriptStatement){.Kind = SCRIPT_OP_COMMAND, .Line = LineNumber}, "",
        LineText);
  }

  char *Tokens[SCRIPT_MAX_TOKENS] = {0};
  int TokenCount = SplitTokens(Statement, Tokens, SCRIPT_MAX_TOKENS);

  if (CompareString(Tokens[0], "quit") == 0) {
    if (TokenCount != 1) {
      ParseError(LineNumber, "quit takes no arguments");
      return false;
    }
    return AppendStatement(
        (ScriptStatement){.Kind = SCRIPT_OP_QUIT, .Line = LineNumber}, "",
        LineText);
  }

  if (CompareString(Tokens[0], "wait") == 0) {
    if (TokenCount == 2 && CompareString(Tokens[1], "loaded") == 0) {
      return AppendStatement(
          (ScriptStatement){.Kind = SCRIPT_OP_WAIT_LOADED, .Line = LineNumber},
          "", LineText);
    }

    bool IsFrames = (TokenCount == 3) && CompareString(Tokens[1], "frames") == 0;
    bool IsTicks = (TokenCount == 3) && CompareString(Tokens[1], "ticks") == 0;
    if (!IsFrames && !IsTicks) {
      ParseError(LineNumber,
                 "expected \"wait loaded\", \"wait ticks <count>\" or "
                 "\"wait frames <count>\"");
      return false;
    }
    int Count = 0;
    if (!ParsePositiveInt(Tokens[2], &Count)) {
      ParseError(LineNumber, "wait count must be a positive integer");
      return false;
    }
    return AppendStatement((ScriptStatement){.Kind = IsFrames
                                                 ? SCRIPT_OP_WAIT_FRAMES
                                                 : SCRIPT_OP_WAIT_TICKS,
                                             .Line = LineNumber,
                                             .Count = Count},
                           "", LineText);
  }

  if (CompareString(Tokens[0], "look") == 0) {
    float Yaw = 0.0F;
    float Pitch = 0.0F;
    if (TokenCount != 3 || !ParseFinite(Tokens[1], &Yaw) ||
        !ParseFinite(Tokens[2], &Pitch)) {
      ParseError(LineNumber, "expected \"look <yaw> <pitch>\" in degrees");
      return false;
    }
    return AppendStatement((ScriptStatement){.Kind = SCRIPT_OP_LOOK,
                                             .Line = LineNumber,
                                             .Yaw = Yaw,
                                             .Pitch = Pitch},
                           "", LineText);
  }

  if (CompareString(Tokens[0], "hold") == 0) {
    int Axis = (TokenCount == 3) ? FindAxis(Tokens[1]) : -1;
    int Count = 0;
    if (Axis < 0 || !ParsePositiveInt(Tokens[2], &Count)) {
      ParseError(LineNumber,
                 "expected \"hold <forward|back|left|right|ascend|descend> "
                 "<ticks>\"");
      return false;
    }
    return AppendStatement((ScriptStatement){.Kind = SCRIPT_OP_HOLD,
                                             .Line = LineNumber,
                                             .Count = Count,
                                             .Index = Axis},
                           "", LineText);
  }

  if (CompareString(Tokens[0], "press") == 0) {
    int Kind = -1;
    if (TokenCount == 2) {
      if (CompareString(Tokens[1], "jump") == 0) {
        Kind = SCRIPT_PRESS_JUMP;
      } else if (CompareString(Tokens[1], "break") == 0) {
        Kind = SCRIPT_PRESS_BREAK;
      } else if (CompareString(Tokens[1], "place") == 0) {
        Kind = SCRIPT_PRESS_PLACE;
      } else if (CompareString(Tokens[1], "pick") == 0) {
        Kind = SCRIPT_PRESS_PICK;
      }
    }
    if (Kind < 0) {
      ParseError(LineNumber, "expected \"press <jump|break|place|pick>\"");
      return false;
    }
    return AppendStatement((ScriptStatement){.Kind = SCRIPT_OP_PRESS,
                                             .Line = LineNumber,
                                             .Index = Kind},
                           "", LineText);
  }

  if (CompareString(Tokens[0], "hotbar") == 0) {
    int Slot = 0;
    if (TokenCount != 2 || !ParsePositiveInt(Tokens[1], &Slot) ||
        Slot > HOTBAR_SIZE) {
      ParseError(LineNumber, "expected \"hotbar <1-9>\"");
      return false;
    }
    // Scripts name slots the way the hotbar is labelled on screen; the
    // simulation indexes them from zero.
    return AppendStatement((ScriptStatement){.Kind = SCRIPT_OP_HOTBAR,
                                             .Line = LineNumber,
                                             .Index = Slot - 1},
                           "", LineText);
  }

  if (CompareString(Tokens[0], "inventory") == 0) {
    return ParseInventoryStatement(Tokens, TokenCount, LineNumber, LineText);
  }

  if (CompareString(Tokens[0], "worldgen_preview") == 0) {
    return ParseWorldgenPreview(Tokens, TokenCount, LineNumber, LineText);
  }

  if (CompareString(Tokens[0], "worldgen_biome_map") == 0) {
    return ParseWorldgenBiomeMap(Tokens, TokenCount, LineNumber, LineText);
  }

  bool IsScreenshot = CompareString(Tokens[0], "screenshot") == 0;
  if (IsScreenshot || CompareString(Tokens[0], "state") == 0) {
    const char *Keyword = IsScreenshot ? "screenshot" : "state";
    if (TokenCount != 2) {
      char Reason[SCRIPT_LOG_LINE_CHARS];
      (void)snprintf(Reason, sizeof(Reason), "expected \"%s <name>\"", Keyword);
      ParseError(LineNumber, Reason);
      return false;
    }
    if (!IsSafeOutputName(Tokens[1])) {
      char Reason[SCRIPT_LOG_LINE_CHARS];
      (void)snprintf(Reason, sizeof(Reason),
                     "%s name must be 1-63 characters of [A-Za-z0-9_-]",
                     Keyword);
      ParseError(LineNumber, Reason);
      return false;
    }
    return AppendStatement((ScriptStatement){.Kind = IsScreenshot
                                                 ? SCRIPT_OP_SCREENSHOT
                                                 : SCRIPT_OP_STATE,
                                             .Line = LineNumber},
                           Tokens[1], LineText);
  }

  ParseError(LineNumber, "unknown statement");
  return false;
}

static bool ParseScriptText(char *Text) {
  int LineNumber = 0;
  char *Cursor = Text;

  while (*Cursor != '\0') {
    char *LineStart = Cursor;
    while (*Cursor != '\0' && *Cursor != '\n') {
      Cursor++;
    }
    if (*Cursor == '\n') {
      *Cursor = '\0';
      Cursor++;
    }
    LineNumber++;

    char *Statement = SkipSpaces(LineStart);
    TrimTrailingSpaces(Statement);
    if (Statement[0] == '\0' || Statement[0] == '#') {
      continue;
    }
    if (!ParseStatement(Statement, LineNumber)) {
      return false;
    }
  }
  return true;
}

bool ScriptLoad(const char *Path, const char *OutDir, double TimeoutSeconds) {
  ScriptState *State = GetScriptState();
  *State = (ScriptState){0};
  State->TimeoutSeconds = TimeoutSeconds;
  // Zero is a real hotbar slot, so "no selection" has to be set explicitly.
  State->HotbarSelect = -1;

  if (!ResolveOutDir(OutDir, State->OutDir, sizeof(State->OutDir))) {
    LogError("SCRIPT: cannot resolve output directory \"%s\"", OutDir);
    State->ExitCode = SCRIPT_EXIT_FAILED;
    return false;
  }
  if (!OpenRunLog(State->OutDir)) {
    State->ExitCode = SCRIPT_EXIT_FAILED;
    return false;
  }

  ScriptLog("script: %s", Path);
  ScriptLog("output: %s", State->OutDir);
  ScriptLog("timeout: %.0f s", TimeoutSeconds);

  char *Text = ReadTextFile(Path);
  if (Text == (char *)0) {
    LogError("SCRIPT: cannot read \"%s\"", Path);
    ScriptLog("PARSE ERROR: cannot read the script file");
    State->ExitCode = SCRIPT_EXIT_FAILED;
    return false;
  }

  bool Parsed = ParseScriptText(Text);
  free(Text);

  if (!Parsed) {
    State->StepCount = 0;
    State->ExitCode = SCRIPT_EXIT_FAILED;
    return false;
  }

  State->Active = true;
  SetCommandOutputSink(ScriptCommandOutput);
  LogInfo("SCRIPT: loaded %d statements from \"%s\"", State->StepCount, Path);
  ScriptLog("parsed %d statements", State->StepCount);
  return true;
}

bool ScriptIsActive(void) { return GetScriptState()->Active; }

bool ScriptRunEnded(void) {
  const ScriptState *State = GetScriptState();
  return State->Active && State->Ended;
}

int ScriptExitCode(void) { return GetScriptState()->ExitCode; }

PlayerInput ScriptPollInput(void) {
  PlayerInput Input = {0};
  Input.HotbarSelect = -1;

  ScriptState *State = GetScriptState();
  if (!State->Active) {
    return Input;
  }

  Input.MoveForward = (State->HoldTicks[SCRIPT_AXIS_FORWARD] > 0 ? 1.0F : 0.0F) -
                      (State->HoldTicks[SCRIPT_AXIS_BACK] > 0 ? 1.0F : 0.0F);
  Input.MoveX = (State->HoldTicks[SCRIPT_AXIS_RIGHT] > 0 ? 1.0F : 0.0F) -
                (State->HoldTicks[SCRIPT_AXIS_LEFT] > 0 ? 1.0F : 0.0F);
  Input.AscendHeld = State->HoldTicks[SCRIPT_AXIS_ASCEND] > 0;
  Input.DescendHeld = State->HoldTicks[SCRIPT_AXIS_DESCEND] > 0;
  Input.SprintHeld = State->HoldTicks[SCRIPT_AXIS_SPRINT] > 0;

  // Handed over once. The caller latches edges until a tick consumes them, so
  // an edge raised on a frame that runs no tick is not lost.
  Input.Jump = State->PressJump;
  Input.Break = State->PressBreak;
  Input.Place = State->PressPlace;
  Input.PickBlock = State->PressPick;
  Input.HotbarSelect = State->HotbarSelect;
  State->PressJump = false;
  State->PressBreak = false;
  State->PressPlace = false;
  State->PressPick = false;
  State->HotbarSelect = -1;

  return Input;
}

void ScriptRecordTicks(int Ticks) {
  ScriptState *State = GetScriptState();
  if (!State->Active) {
    return;
  }
  State->Tick += Ticks;

  // Holds are spent in simulation time, not frames, so the same script moves
  // the player the same distance whatever the framerate.
  for (int Axis = 0; Axis < SCRIPT_AXIS_COUNT; Axis++) {
    State->HoldTicks[Axis] -= Ticks;
    if (State->HoldTicks[Axis] < 0) {
      State->HoldTicks[Axis] = 0;
    }
  }
}

void ScriptEndFrame(void) {
  ScriptState *State = GetScriptState();
  if (State->Active) {
    State->Frame++;
  }
}

static void ScriptTimeout(void) {
  const ScriptState *State = GetScriptState();
  int Line = State->WaitLine;
  if (State->Wait == SCRIPT_WAIT_NONE && State->Pc < State->StepCount) {
    Line = State->Steps[State->Pc].Line;
  }
  LogError("SCRIPT: timed out after %.0f s, blocked at line %d",
           State->TimeoutSeconds, Line);
  char Outcome[SCRIPT_LOG_LINE_CHARS];
  (void)snprintf(Outcome, sizeof(Outcome),
                 "TIMEOUT after %.0f s, blocked at line %d",
                 State->TimeoutSeconds, Line);
  ScriptFinish(SCRIPT_EXIT_TIMEOUT, Outcome);
}

// The point chunk loading is centred on, which is the free camera's position
// while freecam is on and the player's otherwise -- the same choice the main
// loop makes when it asks the world to load.
static Vec3 ScriptLoadCenter(const ScriptContext *Ctx) {
  if (GetDebugState()->Freecam) {
    return Ctx->Camera->Position;
  }
  return Ctx->PlayerVal->Position;
}

// Settled means the world has nothing left in flight AND the meshes it produced
// have all reached the GPU; a chunk stops being dirty when its job is queued,
// so the chunk flags alone would call a world with 24 meshes still uploading
// finished.
static bool ScriptWorldSettled(const ScriptContext *Ctx) {
  return WorldIsSettled(Ctx->WorldVal, ScriptLoadCenter(Ctx),
                        Ctx->RenderDistance) &&
         !RendererHasPendingMeshWork();
}

static cJSON *ScriptVec3Json(Vec3 Value) {
  cJSON *Node = cJSON_CreateObject();
  if (Node == (cJSON *)0) {
    return (cJSON *)0;
  }
  cJSON_AddNumberToObject(Node, "x", (double)Value.x);
  cJSON_AddNumberToObject(Node, "y", (double)Value.y);
  cJSON_AddNumberToObject(Node, "z", (double)Value.z);
  return Node;
}

static void ScriptAddPlayerJson(cJSON *Root, const ScriptContext *Ctx) {
  const Player *PlayerVal = Ctx->PlayerVal;
  cJSON *Node = cJSON_CreateObject();
  if (Node == (cJSON *)0) {
    return;
  }
  cJSON_AddItemToObject(Node, "position", ScriptVec3Json(PlayerVal->Position));
  cJSON_AddItemToObject(Node, "velocity", ScriptVec3Json(PlayerVal->Velocity));
  cJSON_AddBoolToObject(Node, "grounded", PlayerVal->IsGrounded);
  cJSON_AddBoolToObject(Node, "noclip", PlayerVal->Noclip);
  cJSON_AddNumberToObject(Node, "hotbarSlot", PlayerVal->SelectedHotbarSlot);
  cJSON *Hotbar = cJSON_CreateArray();
  if (Hotbar != (cJSON *)0) {
    for (int Slot = 0; Slot < HOTBAR_SIZE; Slot++) {
      cJSON_AddItemToArray(Hotbar,
                           cJSON_CreateNumber((double)PlayerVal->Hotbar[Slot]));
    }
    cJSON_AddItemToObject(Node, "hotbar", Hotbar);
  }
  cJSON_AddItemToObject(Root, "player", Node);
}

static void ScriptAddRenderJson(cJSON *Root) {
  cJSON *Node = cJSON_CreateObject();
  if (Node == (cJSON *)0) {
    return;
  }
  cJSON_AddNumberToObject(Node, "missingBlockTextures",
                          RenderGetMissingBlockTextureCount());
  cJSON_AddItemToObject(Root, "render", Node);
}

static void ScriptAddInventoryJson(cJSON *Root) {
  cJSON *Node = cJSON_CreateObject();
  if (Node == (cJSON *)0) {
    return;
  }
  CreativeInventoryState Inventory = CreativeInventoryGetState();
  cJSON_AddBoolToObject(Node, "open", Inventory.Open);
  cJSON_AddStringToObject(Node, "tab",
                          CreativeInventoryTabName(Inventory.Tab));
  cJSON_AddNumberToObject(Node, "scrollRow", Inventory.ScrollRow);
  cJSON_AddNumberToObject(Node, "maxScrollRow", Inventory.MaxScrollRow);
  cJSON_AddNumberToObject(Node, "carried", (double)Inventory.Carried);
  cJSON_AddItemToObject(Root, "inventory", Node);
}

static void ScriptAddCameraJson(cJSON *Root, const ScriptContext *Ctx) {
  cJSON *Node = cJSON_CreateObject();
  if (Node == (cJSON *)0) {
    return;
  }
  cJSON_AddItemToObject(Node, "position", ScriptVec3Json(Ctx->Camera->Position));
  cJSON_AddItemToObject(Node, "target", ScriptVec3Json(Ctx->Camera->Target));
  cJSON_AddNumberToObject(Node, "fovY", (double)Ctx->Camera->FovY);
  cJSON_AddItemToObject(Root, "camera", Node);
}

static void ScriptAddTargetBlockJson(cJSON *Root, const ScriptContext *Ctx) {
  const RaycastResult *Target = &Ctx->PlayerVal->TargetBlock;
  cJSON *Node = cJSON_CreateObject();
  if (Node == (cJSON *)0) {
    return;
  }
  cJSON_AddBoolToObject(Node, "hit", Target->Hit);
  if (Target->Hit) {
    cJSON_AddItemToObject(Node, "position", ScriptVec3Json(Target->BlockPos));
    cJSON_AddNumberToObject(Node, "blockId", Target->BlockId);
    BlockType *Def = GetBlockDef(Target->BlockId);
    cJSON_AddStringToObject(Node, "blockName",
                            (Def != (BlockType *)0) ? Def->Name : "unknown");
  }
  cJSON_AddItemToObject(Root, "targetBlock", Node);
}

// Generator-derived reference points, so checks can be written against the
// terrain that was actually generated instead of literals that break whenever
// the generator changes.
static void ScriptAddWorldgenJson(cJSON *Root, const ScriptContext *Ctx) {
  cJSON *Node = cJSON_CreateObject();
  if (Node == (cJSON *)0) {
    return;
  }

  uint64_t Seed = GetWorldSeed();
  SpawnPoint Spawn = FindSpawnColumn(Seed);
  cJSON *SpawnNode = cJSON_CreateObject();
  if (SpawnNode != (cJSON *)0) {
    cJSON_AddNumberToObject(SpawnNode, "x", Spawn.X);
    cJSON_AddNumberToObject(SpawnNode, "z", Spawn.Z);
    cJSON_AddNumberToObject(SpawnNode, "surface", Spawn.Surface);
    cJSON_AddNumberToObject(SpawnNode, "feetY", (double)Spawn.FeetPosition.y);
    cJSON_AddItemToObject(Node, "spawn", SpawnNode);
  }

  int ColumnX = (int)floorf(Ctx->PlayerVal->Position.x + BLOCK_HALF_SIZE);
  int ColumnZ = (int)floorf(Ctx->PlayerVal->Position.z + BLOCK_HALF_SIZE);
  // The real top of the generated terrain, which the player stands on; the 2D
  // preliminary surface is reported beside it.
  int SurfaceY = GetTerrainTop(ColumnX, ColumnZ, Seed);
  int PreliminaryY = GetPreliminarySurface(ColumnX, ColumnZ, Seed);
  int SurfaceBlock = GetBlockIDFromWorld(
      Ctx->WorldVal, (Vec3){(float)ColumnX, (float)SurfaceY, (float)ColumnZ});
  const BlockType *SurfaceDef = GetBlockDef(SurfaceBlock);

  uint64_t Classes[CUBE_CLASS_COUNT] = {0};
  GetCubeClassCounts(Classes);
  cJSON *ClassNode = cJSON_CreateObject();
  if (ClassNode != (cJSON *)0) {
    cJSON_AddNumberToObject(ClassNode, "air", (double)Classes[CUBE_CLASS_AIR]);
    cJSON_AddNumberToObject(ClassNode, "solid",
                            (double)Classes[CUBE_CLASS_SOLID]);
    cJSON_AddNumberToObject(ClassNode, "mixed",
                            (double)Classes[CUBE_CLASS_MIXED]);
    cJSON_AddItemToObject(Node, "cubeClasses", ClassNode);
  }

  uint64_t Hits = 0;
  uint64_t Misses = 0;
  ColumnCacheGetTotals(&Hits, &Misses);
  cJSON *CacheNode = cJSON_CreateObject();
  if (CacheNode != (cJSON *)0) {
    cJSON_AddNumberToObject(CacheNode, "hits", (double)Hits);
    cJSON_AddNumberToObject(CacheNode, "misses", (double)Misses);
    cJSON_AddItemToObject(Node, "columnCache", CacheNode);
  }

  cJSON_AddNumberToObject(Node, "generatorVersion", WORLDGEN_VERSION);
  cJSON_AddNumberToObject(Node, "surfaceY", SurfaceY);
  cJSON_AddNumberToObject(Node, "preliminarySurfaceY", PreliminaryY);
  cJSON_AddStringToObject(Node, "surfaceBlock",
                          (SurfaceDef != (BlockType *)0) ? SurfaceDef->Name
                                                         : "unknown");
  cJSON_AddItemToObject(Root, "worldgen", Node);
}

static void ScriptAddChunksJson(cJSON *Root, const ScriptContext *Ctx) {
  int Generated = 0;
  int Generating = 0;
  int Dirty = 0;
  int Meshing = 0;
  WorldGetLoadCounts(Ctx->WorldVal, &Generated, &Generating, &Dirty, &Meshing);

  cJSON *Node = cJSON_CreateObject();
  if (Node == (cJSON *)0) {
    return;
  }
  cJSON_AddNumberToObject(Node, "active", WorldActiveChunkCount(Ctx->WorldVal));
  cJSON_AddNumberToObject(Node, "generated", Generated);
  cJSON_AddNumberToObject(Node, "generating", Generating);
  cJSON_AddNumberToObject(Node, "dirty", Dirty);
  cJSON_AddNumberToObject(Node, "meshing", Meshing);
  cJSON_AddNumberToObject(Node, "rendered", GetLastRenderedChunkCount());
  cJSON_AddBoolToObject(Node, "meshUploadPending", RendererHasPendingMeshWork());

  RenderCullStats Cull = {0};
  GetLastCullStats(&Cull);
  cJSON_AddBoolToObject(Node, "occlusion", Cull.OcclusionEnabled);
  cJSON_AddNumberToObject(Node, "occluded", Cull.Occluded);
  cJSON *CullNode = cJSON_CreateObject();
  if (CullNode != (cJSON *)0) {
    cJSON_AddNumberToObject(CullNode, "renderable", Cull.Renderable);
    cJSON_AddNumberToObject(CullNode, "columns", Cull.Columns);
    cJSON_AddNumberToObject(CullNode, "columnsOutside", Cull.ColumnsOutside);
    cJSON_AddNumberToObject(CullNode, "columnsInside", Cull.ColumnsInside);
    cJSON_AddNumberToObject(CullNode, "columnsIntersecting", Cull.ColumnsIntersecting);
    cJSON_AddNumberToObject(CullNode, "chunksTested", Cull.ChunksTested);
    cJSON_AddNumberToObject(CullNode, "visible", Cull.Visible);
    cJSON_AddNumberToObject(CullNode, "occlusionNodes", Cull.OcclusionNodes);
    cJSON_AddNumberToObject(CullNode, "occlusionReachable",
                            Cull.OcclusionReachable);
    cJSON_AddBoolToObject(CullNode, "occlusionFallback", Cull.OcclusionFallback);
    cJSON_AddNumberToObject(CullNode, "submittedVertices",
                            (double)Cull.SubmittedVertices);
    cJSON_AddItemToObject(Node, "cull", CullNode);
  }
  cJSON_AddItemToObject(Root, "chunks", Node);

  MeshVertexTotals Vertices = {0};
  RendererGetMeshVertexTotals(&Vertices);
  cJSON *VertexNode = cJSON_CreateObject();
  if (VertexNode != (cJSON *)0) {
    cJSON_AddNumberToObject(VertexNode, "opaque", (double)Vertices.Opaque);
    cJSON_AddNumberToObject(VertexNode, "cutout", (double)Vertices.Cutout);
    cJSON_AddNumberToObject(VertexNode, "translucent",
                            (double)Vertices.Translucent);
    cJSON_AddItemToObject(Root, "meshVertices", VertexNode);
  }

  LoadLayerRange Window = {0};
  WorldGetLoadLayerRange(Ctx->WorldVal, ScriptLoadCenter(Ctx),
                         Ctx->RenderDistance, &Window);
  cJSON *WindowNode = cJSON_CreateObject();
  if (WindowNode != (cJSON *)0) {
    cJSON_AddNumberToObject(WindowNode, "lo", Window.Lo);
    cJSON_AddNumberToObject(WindowNode, "hi", Window.Hi);
    cJSON_AddNumberToObject(WindowNode, "layers", Window.Count);
    cJSON_AddItemToObject(Root, "loadWindow", WindowNode);
  }
}

#ifdef MINEGAME_PROFILE
static void ScriptAddScopeJson(cJSON *Parent, const char *Name,
                               const ProfileStats *Stats) {
  cJSON *Node = cJSON_CreateObject();
  if (Node == (cJSON *)0) {
    return;
  }
  cJSON_AddNumberToObject(Node, "averageUs", Stats->AverageUs);
  cJSON_AddNumberToObject(Node, "maxUs", Stats->MaxUs);
  cJSON_AddNumberToObject(Node, "onePercentLowUs", Stats->OnePercentLowUs);
  cJSON_AddItemToObject(Parent, Name, Node);
}

// The rolling window, not this frame: one frame's timings say nothing, and the
// window is what the F3 overlay reports too, so a dump and a screenshot of the
// overlay taken together agree.
static void ScriptAddProfilerJson(cJSON *Root) {
  cJSON *Node = cJSON_CreateObject();
  if (Node == (cJSON *)0) {
    return;
  }

  ProfileStats Frame = {0};
  ProfilerQueryFrame(&Frame);
  ScriptAddScopeJson(Node, "frame", &Frame);

  for (int Scope = 0; Scope < PROFILE_SCOPE_COUNT; Scope++) {
    ProfileStats Stats = {0};
    ProfilerQuery((ProfileScope)Scope, &Stats);
    ScriptAddScopeJson(Node, GetProfileScopeName((ProfileScope)Scope), &Stats);
  }

  uint64_t SolidSamples = 0;
  float SolidOverdraw = 0.0F;
  if (RenderQuerySolidSamples(&SolidSamples, &SolidOverdraw)) {
    cJSON_AddNumberToObject(Node, "solidSamples", (double)SolidSamples);
    cJSON_AddNumberToObject(Node, "solidOverdraw", (double)SolidOverdraw);
  }

  cJSON_AddItemToObject(Root, "profiler", Node);
}
#endif

// Writes everything a run can check without looking at pixels. Cheaper to read
// than a screenshot and unambiguous where a screenshot is a judgement call.
static bool ScriptWriteState(const ScriptContext *Ctx, const char *Name,
                             int Line) {
  ScriptState *State = GetScriptState();

  cJSON *Root = cJSON_CreateObject();
  if (Root == (cJSON *)0) {
    ScriptFail(Line, "out of memory building the state dump");
    return false;
  }

  cJSON_AddNumberToObject(Root, "frame", (double)State->Frame);
  cJSON_AddNumberToObject(Root, "tick", (double)State->Tick);
  cJSON_AddBoolToObject(Root, "settled", ScriptWorldSettled(Ctx));
  cJSON_AddNumberToObject(Root, "renderDistance", Ctx->RenderDistance);
  ScriptAddPlayerJson(Root, Ctx);
  ScriptAddCameraJson(Root, Ctx);
  ScriptAddTargetBlockJson(Root, Ctx);
  ScriptAddChunksJson(Root, Ctx);
  ScriptAddWorldgenJson(Root, Ctx);
  ScriptAddInventoryJson(Root);
  ScriptAddRenderJson(Root);
#ifdef MINEGAME_PROFILE
  ScriptAddProfilerJson(Root);
#endif

  char *Text = cJSON_Print(Root);
  cJSON_Delete(Root);
  if (Text == (char *)0) {
    ScriptFail(Line, "out of memory serialising the state dump");
    return false;
  }

  char Path[FILEIO_MAX_PATH];
  int Written = snprintf(Path, sizeof(Path), "%s/%s.json", State->OutDir, Name);
  bool Ok = Written > 0 && (size_t)Written < sizeof(Path) &&
            WriteTextFile(Path, Text);
  cJSON_free(Text);

  if (!Ok) {
    ScriptFail(Line, "could not write the state dump");
    return false;
  }
  ScriptLog("      | wrote %s.json", Name);
  return true;
}

// Writes the frame the backend has read back, if it is ready yet. Returns false
// while it is not; a write failure ends the run instead of being retried.
static bool ScriptSaveCapture(void) {
  ScriptState *State = GetScriptState();
  CoreImage Image = {0};
  if (!RenderTakeCapture(&Image)) {
    return false;
  }

  char Path[FILEIO_MAX_PATH];
  int Written = snprintf(Path, sizeof(Path), "%s/%s.png", State->OutDir,
                         State->WaitName);
  bool Fits = Written > 0 && (size_t)Written < sizeof(Path);
  if (!Fits || !CoreImageEncodePng(&Image, Path)) {
    CoreImageFree(&Image);
    ScriptFail(State->WaitLine, "could not write the screenshot");
    return true;
  }

  ScriptLog("      | wrote %s.png (%dx%d)", State->WaitName, Image.Width,
            Image.Height);
  CoreImageFree(&Image);
  return true;
}

// True while the current wait is unsatisfied. Clears the wait once it is.
static bool ScriptIsWaiting(const ScriptContext *Ctx) {
  ScriptState *State = GetScriptState();
  switch (State->Wait) {
  case SCRIPT_WAIT_NONE:
    return false;
  case SCRIPT_WAIT_FRAMES:
    if (State->Frame < State->WaitTarget) {
      return true;
    }
    break;
  case SCRIPT_WAIT_TICKS:
    if (State->Tick < State->WaitTarget) {
      return true;
    }
    break;
  case SCRIPT_WAIT_LOADED:
    State->SettledFrames = ScriptWorldSettled(Ctx) ? State->SettledFrames + 1 : 0;
    if (State->SettledFrames == 1) {
      // Discard the loading spike at the start of the quiet streak so the
      // window holds only settled frames once the wait releases.
      ProfilerResetWindow();
    }
    if (State->SettledFrames < SETTLED_FRAMES_REQUIRED) {
      return true;
    }
    ScriptLog("      | world settled after %lld frames and %lld ticks",
              State->Frame, State->Tick);
    break;
  case SCRIPT_WAIT_CAPTURE:
    if (!ScriptSaveCapture()) {
      return true;
    }
    break;
  }

  State->Wait = SCRIPT_WAIT_NONE;
  return false;
}

// Runs one inventory statement through the same handlers the mouse uses. A
// statement that cannot apply fails the run, so a script never silently
// screenshots a state it did not reach.
static bool ScriptRunInventory(const ScriptStatement *Step,
                               const Player *PlayerVal) {
  if (Step->Index == SCRIPT_INV_OPEN) {
    CreativeInventoryOpen(PlayerVal);
    // A scripted run never captures the cursor, open screen or not.
    PlatformSetCursorDisabled(false);
    return true;
  }
  if (!CreativeInventoryIsOpen()) {
    ScriptFail(Step->Line, "the inventory is not open");
    return false;
  }

  switch ((ScriptInventoryAction)Step->Index) {
  case SCRIPT_INV_CLOSE:
    CreativeInventoryClose();
    PlatformSetCursorDisabled(false);
    return true;
  case SCRIPT_INV_TAB:
    CreativeInventorySelectTab((InventoryTab)Step->Count);
    return true;
  case SCRIPT_INV_SCROLL:
    CreativeInventoryScrollRows(Step->Count);
    return true;
  case SCRIPT_INV_HOVER:
  case SCRIPT_INV_PICK:
    // Entries are counted from 1 in scripts, as they are read on screen.
    if (Step->Count > CreativeInventoryVisibleEntryCount()) {
      ScriptFail(Step->Line, "no visible catalog entry with that number");
      return false;
    }
    if (Step->Index == SCRIPT_INV_HOVER) {
      CreativeInventoryHoverEntry(Step->Count - 1);
      return true;
    }
    return CreativeInventoryPickEntry(Step->Count - 1);
  case SCRIPT_INV_PUT:
    if (!CreativeInventoryPutCarried(Step->Count - 1)) {
      ScriptFail(Step->Line, "no block is carried");
      return false;
    }
    return true;
  case SCRIPT_INV_OPEN:
    return true;
  }
  return true;
}

void ScriptStep(ChatState *Chat, GameCamera *Camera, Player *PlayerVal,
                World *WorldVal, int RenderDistance) {
  ScriptState *State = GetScriptState();
  if (!State->Active || State->Ended) {
    return;
  }

  // Armed on the first stepped frame rather than at load, because the platform
  // clock starts with the window and a cold start can spend seconds before it.
  if (!State->DeadlineArmed) {
    State->Deadline = PlatformGetTime() + State->TimeoutSeconds;
    State->DeadlineArmed = true;
  }
  if (PlatformGetTime() >= State->Deadline) {
    ScriptTimeout();
    return;
  }

  ScriptContext Ctx = {
      .Chat = Chat,
      .Camera = Camera,
      .PlayerVal = PlayerVal,
      .WorldVal = WorldVal,
      .RenderDistance = RenderDistance,
  };

  if (ScriptIsWaiting(&Ctx)) {
    return;
  }
  // A wait can end the run itself, when the file it was waiting to write could
  // not be written.
  if (State->Ended) {
    return;
  }

  while (State->Pc < State->StepCount) {
    const ScriptStatement *Step = &State->Steps[State->Pc];
    State->Pc++;
    ScriptLog("[f%lld t%lld] %d: %s", State->Frame, State->Tick, Step->Line,
              Step->Text);

    switch (Step->Kind) {
    case SCRIPT_OP_COMMAND: {
      char Command[SCRIPT_MAX_TEXT];
      SafeStrncpy(Command, Step->Text, (int)sizeof(Command));
      if (!CommandHandler(Command, Chat, Camera, PlayerVal, WorldVal)) {
        ScriptFail(Step->Line, "unknown command");
        return;
      }
      break;
    }
    case SCRIPT_OP_WAIT_FRAMES:
      State->Wait = SCRIPT_WAIT_FRAMES;
      State->WaitTarget = State->Frame + Step->Count;
      State->WaitLine = Step->Line;
      return;
    case SCRIPT_OP_WAIT_TICKS:
      State->Wait = SCRIPT_WAIT_TICKS;
      State->WaitTarget = State->Tick + Step->Count;
      State->WaitLine = Step->Line;
      return;
    case SCRIPT_OP_WAIT_LOADED:
      State->Wait = SCRIPT_WAIT_LOADED;
      State->SettledFrames = 0;
      State->WaitLine = Step->Line;
      return;
    case SCRIPT_OP_LOOK:
      // Whichever camera is active, matching what the mouse would have moved.
      if (GetDebugState()->Freecam) {
        FreeCameraSetAim(Ctx.Camera, Step->Yaw, Step->Pitch);
      } else {
        CameraSetAim(Ctx.Camera, Step->Yaw, Step->Pitch);
      }
      break;
    case SCRIPT_OP_HOLD:
      // Not a wait: several axes can be held at once, and the statements after
      // it keep running while it counts down.
      State->HoldTicks[Step->Index] = Step->Count;
      break;
    case SCRIPT_OP_PRESS:
      if (Step->Index == SCRIPT_PRESS_JUMP) {
        State->PressJump = true;
      } else if (Step->Index == SCRIPT_PRESS_BREAK) {
        State->PressBreak = true;
      } else if (Step->Index == SCRIPT_PRESS_PLACE) {
        State->PressPlace = true;
      } else if (Step->Index == SCRIPT_PRESS_PICK) {
        State->PressPick = true;
      }
      break;
    case SCRIPT_OP_HOTBAR:
      State->HotbarSelect = Step->Index;
      break;
    case SCRIPT_OP_INVENTORY:
      if (!ScriptRunInventory(Step, PlayerVal)) {
        return;
      }
      break;
    case SCRIPT_OP_STATE:
      if (!ScriptWriteState(&Ctx, Step->Name, Step->Line)) {
        return;
      }
      break;
    case SCRIPT_OP_WORLDGEN_PREVIEW: {
      char Reason[WORLDGEN_PREVIEW_ERROR_CHARS];
      int CenterX = Step->PreviewX;
      int CenterZ = Step->PreviewZ;
      if (Step->PreviewAtSpawn) {
        SpawnPoint Spawn = FindSpawnColumn(GetWorldSeed());
        CenterX = Spawn.X;
        CenterZ = Spawn.Z;
      }
      if (!WorldgenPreviewWrite(State->OutDir, Step->Name, CenterX, CenterZ,
                                Step->Count, Reason, (int)sizeof(Reason))) {
        ScriptFail(Step->Line, Reason);
        return;
      }
      ScriptLog("      | wrote %s_height.png, %s_biome.png, %s_slice.png and "
                "%s_worldgen.json",
                Step->Name, Step->Name, Step->Name, Step->Name);
      break;
    }
    case SCRIPT_OP_WORLDGEN_BIOME_MAP: {
      char Reason[WORLDGEN_PREVIEW_ERROR_CHARS];
      int CenterX = Step->PreviewX;
      int CenterZ = Step->PreviewZ;
      if (Step->PreviewAtSpawn) {
        SpawnPoint Spawn = FindSpawnColumn(GetWorldSeed());
        CenterX = Spawn.X;
        CenterZ = Spawn.Z;
      }
      if (!WorldgenBiomeMapWrite(State->OutDir, Step->Name, CenterX, CenterZ,
                                 Step->Count, Step->Index, Reason,
                                 (int)sizeof(Reason))) {
        ScriptFail(Step->Line, Reason);
        return;
      }
      ScriptLog("      | wrote %s_biomes.png and %s_biomes.json", Step->Name,
                Step->Name);
      break;
    }
    case SCRIPT_OP_SCREENSHOT:
      // The frame this statement is reached on is the one captured, so every
      // statement above it has already taken effect on what gets drawn.
      RenderRequestCapture();
      State->Wait = SCRIPT_WAIT_CAPTURE;
      State->WaitLine = Step->Line;
      SafeStrncpy(State->WaitName, Step->Name, (int)sizeof(State->WaitName));
      return;
    case SCRIPT_OP_QUIT:
      ScriptFinish(SCRIPT_EXIT_OK, "OK: quit");
      return;
    }
  }

  ScriptFinish(SCRIPT_EXIT_OK, "OK: end of script");
}

void ScriptShutdown(void) {
  ScriptState *State = GetScriptState();
  if (State->Active) {
    SetCommandOutputSink((CommandOutputSink)0);
    // The loop left for a reason of its own -- the window was closed, or the
    // pause menu asked to quit -- so the script never reached its end and the
    // run did not pass.
    if (!State->Ended) {
      ScriptFinish(SCRIPT_EXIT_FAILED, "ABORTED: the game exited before the "
                                       "script finished");
    }
    LogInfo("SCRIPT: exit %d after %lld frames and %lld ticks", State->ExitCode,
            State->Frame, State->Tick);
    ScriptLog("exit %d after %lld frames and %lld ticks", State->ExitCode,
              State->Frame, State->Tick);
  }
  if (State->Log != (FILE *)0) {
    (void)fclose(State->Log);
    State->Log = (FILE *)0;
  }
  State->Active = false;
}
