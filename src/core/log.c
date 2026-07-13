#include "core/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static void LogVaList(const char *Prefix, const char *Fmt, va_list Args) {
  fputs(Prefix, stderr);
  vfprintf(stderr, Fmt, Args);
  fputc('\n', stderr);
}

void LogInfo(const char *Fmt, ...) {
  va_list Args;
  va_start(Args, Fmt);
  LogVaList("[INFO] ", Fmt, Args);
  va_end(Args);
}

void LogWarn(const char *Fmt, ...) {
  va_list Args;
  va_start(Args, Fmt);
  LogVaList("[WARN] ", Fmt, Args);
  va_end(Args);
}

void LogError(const char *Fmt, ...) {
  va_list Args;
  va_start(Args, Fmt);
  LogVaList("[ERROR] ", Fmt, Args);
  va_end(Args);
}

_Noreturn void LogFatal(const char *Fmt, ...) {
  va_list Args;
  va_start(Args, Fmt);
  LogVaList("[FATAL] ", Fmt, Args);
  va_end(Args);
  abort();
}
