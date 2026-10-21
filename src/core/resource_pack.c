#include "core/resource_pack.h"
#include "core/fileio.h"
#include "core/image.h"
#include "core/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  MAX_RESOURCE_PACKS = 64
};

typedef struct {
  ZipArchive *Archives[MAX_RESOURCE_PACKS];
  int PackCount;
  const char *TexturesRoot;
} ResourcePackState;

static ResourcePackState *GetResourcePackState(void) {
  static ResourcePackState State;
  return &State;
}

static int ComparePackPaths(const void *Lhs, const void *Rhs) {
  return strcmp((const char *)Lhs, (const char *)Rhs);
}

void ResourcePacksInit(void) {
  ResourcePackState *State = GetResourcePackState();
  ResourcePacksShutdown();

  State->TexturesRoot =
      IsDirectory("assets/textures") ? "assets/textures" : "textures";

  char ExternalDir[FILEIO_MAX_PATH];
  snprintf(ExternalDir, sizeof(ExternalDir), "%s/external", State->TexturesRoot);

  static char CandidatePaths[MAX_RESOURCE_PACKS][FILEIO_MAX_PATH];
  static char ZipPaths[MAX_RESOURCE_PACKS][FILEIO_MAX_PATH];

  int CandidateCount =
      ListDirectoryFiles(ExternalDir, CandidatePaths, MAX_RESOURCE_PACKS);
  int ZipCount = 0;
  for (int Idx = 0; Idx < CandidateCount; Idx++) {
    if (HasFileExtension(CandidatePaths[Idx], ".zip")) {
      snprintf(ZipPaths[ZipCount], FILEIO_MAX_PATH, "%s", CandidatePaths[Idx]);
      ZipCount++;
    }
  }

  // Directory enumeration order is filesystem-defined; sorting makes which
  // pack wins a conflict reproducible across machines.
  if (ZipCount > 1) {
    qsort(ZipPaths, (size_t)ZipCount, FILEIO_MAX_PATH, ComparePackPaths);
  }

  for (int Idx = 0; Idx < ZipCount; Idx++) {
    ZipArchive *Archive = ZipOpen(ZipPaths[Idx]);
    if (Archive == NULL) {
      LogWarn("RESOURCE: Unusable resource pack archive '%s', skipping",
              ZipPaths[Idx]);
      continue;
    }
    LogInfo("RESOURCE: Using resource pack '%s'", ZipPaths[Idx]);
    State->Archives[State->PackCount++] = Archive;
  }
}

void ResourcePacksShutdown(void) {
  ResourcePackState *State = GetResourcePackState();
  for (int Idx = 0; Idx < State->PackCount; Idx++) {
    ZipClose(State->Archives[Idx]);
    State->Archives[Idx] = NULL;
  }
  State->PackCount = 0;
}

int ResourcePackCount(void) { return GetResourcePackState()->PackCount; }

static void FormatPackEntry(ResourceCategory Category, const char *RelPath,
                            char *Out, size_t OutSize) {
  const char *Dir = (Category == RESOURCE_FONT_DEF) ? "font" : "textures";
  snprintf(Out, OutSize, "assets/minecraft/%s/%s", Dir, RelPath);
}

static void FormatBasePath(ResourceCategory Category, const char *RelPath,
                           char *Out, size_t OutSize) {
  const char *Root = GetResourcePackState()->TexturesRoot;
  if (Root == NULL) {
    Root = "textures";
  }
  if (Category == RESOURCE_FONT_DEF) {
    snprintf(Out, OutSize, "%s/font/%s", Root, RelPath);
  } else {
    snprintf(Out, OutSize, "%s/%s", Root, RelPath);
  }
}

// Appends the zero byte the header promises. Takes ownership of Payload.
static unsigned char *TerminatePayload(unsigned char *Payload, int Size) {
  unsigned char *Grown = (unsigned char *)realloc(Payload, (size_t)Size + 1U);
  if (Grown == NULL) {
    free(Payload);
    return NULL;
  }
  Grown[Size] = 0;
  return Grown;
}

static unsigned char *ReadFromPack(ZipArchive *Archive, const char *EntryPath,
                                   int *OutSize) {
  const ZipEntry *Entry = ZipFind(Archive, EntryPath);
  if (Entry == NULL) {
    return NULL;
  }

  int RawSize = 0;
  int Method = 0;
  unsigned char *Raw = ZipReadRaw(Archive, Entry, &RawSize, &Method);
  if (Raw == NULL) {
    return NULL;
  }

  if (Method == ZIP_METHOD_STORED) {
    *OutSize = RawSize;
    return TerminatePayload(Raw, RawSize);
  }

  unsigned char *Result = NULL;
  if (Method == ZIP_METHOD_DEFLATE) {
    int UncompSize = ZipEntryUncompressedSize(Entry);
    unsigned char *Inflated = CoreInflateRaw(Raw, RawSize, UncompSize);
    if (Inflated != NULL) {
      *OutSize = UncompSize;
      Result = TerminatePayload(Inflated, UncompSize);
    }
  }
  free(Raw);
  return Result;
}

unsigned char *ResourceReadFrom(ResourceCategory Category, const char *RelPath,
                                int Source, int *OutSize) {
  ResourcePackState *State = GetResourcePackState();
  if (RelPath == NULL || RelPath[0] == '\0' || OutSize == NULL) {
    return NULL;
  }
  *OutSize = 0;

  char Path[FILEIO_MAX_PATH];
  if (Source == RESOURCE_SOURCE_BASE) {
    FormatBasePath(Category, RelPath, Path, sizeof(Path));
    return ReadBinaryFile(Path, OutSize);
  }
  if (Source < 0 || Source >= State->PackCount) {
    return NULL;
  }
  FormatPackEntry(Category, RelPath, Path, sizeof(Path));
  return ReadFromPack(State->Archives[Source], Path, OutSize);
}

unsigned char *ResourceRead(ResourceCategory Category, const char *RelPath,
                            int *OutSize, int *OutSource) {
  ResourcePackState *State = GetResourcePackState();
  if (OutSource != NULL) {
    *OutSource = RESOURCE_SOURCE_NONE;
  }

  for (int Idx = 0; Idx < State->PackCount; Idx++) {
    unsigned char *Data = ResourceReadFrom(Category, RelPath, Idx, OutSize);
    if (Data != NULL) {
      if (OutSource != NULL) {
        *OutSource = Idx;
      }
      return Data;
    }
  }

  unsigned char *Data =
      ResourceReadFrom(Category, RelPath, RESOURCE_SOURCE_BASE, OutSize);
  if (Data != NULL && OutSource != NULL) {
    *OutSource = RESOURCE_SOURCE_BASE;
  }
  return Data;
}
