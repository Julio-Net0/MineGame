#ifndef LOG_H
#define LOG_H

// Renderer-free logging facility: level-tagged messages written to stderr.
// Standard-C only; no Raylib dependency, so non-render engine code and a
// future dedicated server can log without a renderer.

#if defined(__GNUC__)
#define LOG_PRINTF_FORMAT(FmtIdx, ArgsIdx)                                     \
  __attribute__((format(printf, FmtIdx, ArgsIdx)))
#else
#define LOG_PRINTF_FORMAT(FmtIdx, ArgsIdx)
#endif

// Renderer-free severity tag, so callers can select a level at runtime without
// depending on the renderer's log enum.
typedef enum {
  LOG_LEVEL_INFO,
  LOG_LEVEL_WARN,
  LOG_LEVEL_ERROR
} LogLevel;

// Write a printf-style message to stderr, prefixed with the level and
// terminated by a newline.
void LogInfo(const char *Fmt, ...) LOG_PRINTF_FORMAT(1, 2);
void LogWarn(const char *Fmt, ...) LOG_PRINTF_FORMAT(1, 2);
void LogError(const char *Fmt, ...) LOG_PRINTF_FORMAT(1, 2);

// Write the message like the above, then abort the process.
_Noreturn void LogFatal(const char *Fmt, ...) LOG_PRINTF_FORMAT(1, 2);

#endif
