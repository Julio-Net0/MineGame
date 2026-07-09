#include "core/fileio.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *ReadTextFile(const char *Path) {
  if (Path == (void *)0) {
    return (void *)0;
  }

  FILE *File = fopen(Path, "rb");
  if (File == (void *)0) {
    return (void *)0;
  }

  if (fseek(File, 0, SEEK_END) != 0) {
    fclose(File);
    return (void *)0;
  }

  long Size = ftell(File);
  if (Size < 0) {
    fclose(File);
    return (void *)0;
  }
  if (fseek(File, 0, SEEK_SET) != 0) {
    fclose(File);
    return (void *)0;
  }

  char *Buffer = (char *)malloc((size_t)Size + 1);
  if (Buffer == (void *)0) {
    fclose(File);
    return (void *)0;
  }

  size_t ReadCount = fread(Buffer, 1, (size_t)Size, File);
  fclose(File);
  Buffer[ReadCount] = '\0';
  return Buffer;
}

int ListDirectoryFiles(const char *DirPath, char OutPaths[][FILEIO_MAX_PATH],
                       int MaxFiles) {
  if (DirPath == (void *)0 || OutPaths == (void *)0 || MaxFiles <= 0) {
    return 0;
  }

  DIR *Dir = opendir(DirPath);
  if (Dir == (void *)0) {
    return 0;
  }

  int Count = 0;
  struct dirent *Entry = readdir(Dir);
  while (Entry != (void *)0 && Count < MaxFiles) {
    if (strcmp(Entry->d_name, ".") != 0 && strcmp(Entry->d_name, "..") != 0) {
      snprintf(OutPaths[Count], FILEIO_MAX_PATH, "%s/%s", DirPath,
               Entry->d_name);
      Count++;
    }
    Entry = readdir(Dir);
  }

  closedir(Dir);
  return Count;
}

bool HasFileExtension(const char *Path, const char *Ext) {
  if (Path == (void *)0 || Ext == (void *)0) {
    return false;
  }

  size_t PathLen = strlen(Path);
  size_t ExtLen = strlen(Ext);
  if (ExtLen > PathLen) {
    return false;
  }

  const char *Suffix = Path + (PathLen - ExtLen);
  for (size_t Idx = 0; Idx < ExtLen; Idx++) {
    char CharA = Suffix[Idx];
    char CharB = Ext[Idx];
    if (CharA >= 'A' && CharA <= 'Z') {
      CharA = (char)(CharA - 'A' + 'a');
    }
    if (CharB >= 'A' && CharB <= 'Z') {
      CharB = (char)(CharB - 'A' + 'a');
    }
    if (CharA != CharB) {
      return false;
    }
  }
  return true;
}
