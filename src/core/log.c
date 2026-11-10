#include "core/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

enum {
  LOG_LINE_CAPACITY = 1024
};

static void LogVaList(const char *Prefix, const char *Fmt, va_list Args) {
  char Buffer[LOG_LINE_CAPACITY];
  int PrefixWritten = snprintf(Buffer, sizeof(Buffer) - 1U, "%s", Prefix);
  size_t PrefixLen = (PrefixWritten > 0) ? (size_t)PrefixWritten : 0U;
  if (PrefixLen >= sizeof(Buffer) - 2U) {
    PrefixLen = sizeof(Buffer) - 2U;
  }

  int Written = vsnprintf(Buffer + PrefixLen,
                          sizeof(Buffer) - PrefixLen - 1U, Fmt, Args);
  size_t TotalLen = PrefixLen;
  if (Written > 0) {
    size_t BodyLen = (size_t)Written;
    size_t MaxBody = sizeof(Buffer) - PrefixLen - 2U;
    if (BodyLen > MaxBody) {
      BodyLen = MaxBody;
    }
    TotalLen += BodyLen;
  }
  Buffer[TotalLen++] = '\n';
  (void)fwrite(Buffer, 1U, TotalLen, stderr);
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
