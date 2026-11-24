#include "platform/os_file.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define OS_MAKE_DIR(Path) _mkdir(Path)
#define OS_REMOVE_DIR(Path) _rmdir(Path)
#else
#include <sys/stat.h>
#include <unistd.h>
#define OS_MAKE_DIR(Path) mkdir((Path), 0777)
#define OS_REMOVE_DIR(Path) rmdir(Path)
#endif

enum { OS_PATH_CAPACITY = 1024 };

static bool IsSeparator(char Value) { return Value == '/' || Value == '\\'; }

static bool MakeOne(const char *Path) {
  return OS_MAKE_DIR(Path) == 0 || errno == EEXIST;
}

bool OsMakeDirectories(const char *Path) {
  if (Path == (const char *)0) {
    return false;
  }
  size_t Length = strlen(Path);
  if (Length == 0 || Length >= OS_PATH_CAPACITY) {
    return false;
  }
  char Buffer[OS_PATH_CAPACITY];
  memcpy(Buffer, Path, Length + 1);
  // Every prefix that ends at a separator, skipping a leading one and a drive
  // ("C:") that cannot be created.
  for (size_t Idx = 1; Idx < Length; Idx++) {
    if (!IsSeparator(Buffer[Idx]) || Buffer[Idx - 1] == ':') {
      continue;
    }
    char Saved = Buffer[Idx];
    Buffer[Idx] = '\0';
    if (!MakeOne(Buffer)) {
      return false;
    }
    Buffer[Idx] = Saved;
  }
  return MakeOne(Buffer);
}

bool OsReplaceFile(const char *Source, const char *Target) {
#ifdef _WIN32
  return MoveFileExA(Source, Target, MOVEFILE_REPLACE_EXISTING) != 0;
#else
  return rename(Source, Target) == 0;
#endif
}

bool OsRemoveDirectory(const char *Path) { return OS_REMOVE_DIR(Path) == 0; }
