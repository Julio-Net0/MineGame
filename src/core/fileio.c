#include "core/fileio.h"
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_TEXT_FILE_SIZE = 16 * 1024 * 1024 };

// Reads the whole file into a heap buffer with one extra zero byte, so text
// callers get a terminated string and binary callers simply ignore it.
static unsigned char *ReadWholeFile(const char *Path, size_t *OutSize) {
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
  if (Size < 0 || Size > MAX_TEXT_FILE_SIZE) {
    fclose(File);
    return (void *)0;
  }
  if (fseek(File, 0, SEEK_SET) != 0) {
    fclose(File);
    return (void *)0;
  }

  size_t AllocSize = (size_t)Size;
  unsigned char *Buffer = (unsigned char *)calloc(AllocSize + 1U, 1U);
  if (Buffer == (void *)0) {
    fclose(File);
    return (void *)0;
  }

  size_t ReadCount = fread(Buffer, 1, AllocSize, File);
  fclose(File);
  if (ReadCount != AllocSize) {
    free(Buffer);
    return (void *)0;
  }
  *OutSize = AllocSize;
  return Buffer;
}

char *ReadTextFile(const char *Path) {
  size_t Size = 0;
  return (char *)ReadWholeFile(Path, &Size);
}

unsigned char *ReadBinaryFile(const char *Path, int *OutSize) {
  if (OutSize == (void *)0) {
    return (void *)0;
  }
  size_t Size = 0;
  unsigned char *Buffer = ReadWholeFile(Path, &Size);
  if (Buffer != (void *)0) {
    *OutSize = (int)Size;
  }
  return Buffer;
}

bool IsDirectory(const char *Path) {
  if (Path == (void *)0) {
    return false;
  }
  DIR *Dir = opendir(Path);
  if (Dir == (void *)0) {
    return false;
  }
  closedir(Dir);
  return true;
}

bool WriteTextFile(const char *Path, const char *Content) {
  if (Path == (void *)0 || Content == (void *)0) {
    return false;
  }

  FILE *File = fopen(Path, "wb");
  if (File == (void *)0) {
    return false;
  }

  size_t Length = strlen(Content);
  size_t Written = fwrite(Content, 1, Length, File);
  if (fclose(File) != 0) {
    return false;
  }
  return Written == Length;
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

// ---------------------------------------------------------------------------
// Bounds-checked ZIP Central Directory reader
// ---------------------------------------------------------------------------

enum {
  ZIP_EOCD_MIN_SIZE = 22,
  ZIP_MAX_COMMENT_SIZE = 65535,
  ZIP_EOCD_MAX_SEARCH = ZIP_EOCD_MIN_SIZE + ZIP_MAX_COMMENT_SIZE,
  ZIP_CD_HEADER_SIZE = 46,
  ZIP_LOCAL_HEADER_SIZE = 30,
  ZIP_MAX_ENTRY_NAME = 256,
  ZIP_MAX_UNCOMPRESSED_SIZE = 4 * 1024 * 1024,
  ZIP_MAX_ENTRIES = 16384,

  EOCD_DISK_NUM_OFF = 4,
  EOCD_CD_DISK_OFF = 6,
  EOCD_DISK_ENTRIES_OFF = 8,
  EOCD_TOTAL_ENTRIES_OFF = 10,
  EOCD_CD_SIZE_OFF = 12,
  EOCD_CD_OFFSET_OFF = 16,
  EOCD_COMMENT_LEN_OFF = 20,

  CD_FLAGS_OFF = 8,
  CD_METHOD_OFF = 10,
  CD_COMP_SIZE_OFF = 20,
  CD_UNCOMP_SIZE_OFF = 24,
  CD_NAME_LEN_OFF = 28,
  CD_EXTRA_LEN_OFF = 30,
  CD_COMMENT_LEN_OFF = 32,
  CD_DISK_START_OFF = 34,
  CD_LOCAL_OFFSET_OFF = 42,

  LH_NAME_LEN_OFF = 26,
  LH_EXTRA_LEN_OFF = 28,

  BYTE_SHIFT_8 = 8,
  BYTE_SHIFT_16 = 16,
  BYTE_SHIFT_24 = 24,

  ZIP_FLAG_ENCRYPTED = 0x0001U,
  ZIP64_SENTINEL_16 = 0xFFFFU
};

static const uint32_t ZIP_SIG_EOCD = 0x06054B50U;
static const uint32_t ZIP_SIG_CD = 0x02014B50U;
static const uint32_t ZIP_SIG_LOCAL = 0x04034B50U;
static const uint32_t ZIP64_SENTINEL_32 = 0xFFFFFFFFU;

struct ZipEntry {
  char Name[ZIP_MAX_ENTRY_NAME];
  uint32_t LocalHeaderOffset;
  uint32_t CompressedSize;
  uint32_t UncompressedSize;
  uint16_t Method;
  bool Valid;
};

struct ZipArchive {
  FILE *File;
  size_t FileSize;
  ZipEntry *Entries;
  int EntryCount;
};

static uint16_t ReadLe16(const unsigned char *Buf) {
  return (uint16_t)((uint16_t)Buf[0] |
                    (uint16_t)((uint16_t)Buf[1] << BYTE_SHIFT_8));
}

static uint32_t ReadLe32(const unsigned char *Buf) {
  return (uint32_t)Buf[0] |
         ((uint32_t)Buf[1] << BYTE_SHIFT_8) |
         ((uint32_t)Buf[2] << BYTE_SHIFT_16) |
         ((uint32_t)Buf[3] << BYTE_SHIFT_24);
}

static bool FindEndOfCentralDir(FILE *File, size_t FileSize,
                                uint16_t *OutEntries, uint32_t *OutCdSize,
                                uint32_t *OutCdOffset) {
  if (FileSize < ZIP_EOCD_MIN_SIZE) {
    return false;
  }

  size_t TailSize = FileSize;
  if (TailSize > (size_t)ZIP_EOCD_MAX_SEARCH) {
    TailSize = (size_t)ZIP_EOCD_MAX_SEARCH;
  }

  size_t TailOffset = FileSize - TailSize;
  if (fseek(File, (long)TailOffset, SEEK_SET) != 0) {
    return false;
  }

  unsigned char *TailBuf = (unsigned char *)malloc(ZIP_EOCD_MAX_SEARCH);
  if (TailBuf == NULL) {
    return false;
  }

  if (fread(TailBuf, 1, TailSize, File) != TailSize) {
    free(TailBuf);
    return false;
  }

  bool Found = false;
  size_t EocdPos = 0;
  size_t MaxPos = TailSize - (size_t)ZIP_EOCD_MIN_SIZE;
  for (size_t Step = 0; Step <= MaxPos; Step++) {
    size_t Pos = MaxPos - Step;
    if (Pos > (size_t)(ZIP_EOCD_MAX_SEARCH - ZIP_EOCD_MIN_SIZE)) {
      continue;
    }
    if (ReadLe32(&TailBuf[Pos]) != ZIP_SIG_EOCD) {
      continue;
    }
    uint16_t CommentLen = ReadLe16(&TailBuf[Pos + EOCD_COMMENT_LEN_OFF]);
    if (Pos + (size_t)ZIP_EOCD_MIN_SIZE + (size_t)CommentLen <= TailSize) {
      EocdPos = Pos;
      Found = true;
      break;
    }
  }

  if (!Found) {
    free(TailBuf);
    return false;
  }

  const unsigned char *Eocd = &TailBuf[EocdPos];
  uint16_t DiskNum = ReadLe16(&Eocd[EOCD_DISK_NUM_OFF]);
  uint16_t CdDisk = ReadLe16(&Eocd[EOCD_CD_DISK_OFF]);
  uint16_t DiskEntries = ReadLe16(&Eocd[EOCD_DISK_ENTRIES_OFF]);
  uint16_t TotalEntries = ReadLe16(&Eocd[EOCD_TOTAL_ENTRIES_OFF]);
  uint32_t CdSize = ReadLe32(&Eocd[EOCD_CD_SIZE_OFF]);
  uint32_t CdOffset = ReadLe32(&Eocd[EOCD_CD_OFFSET_OFF]);
  free(TailBuf);

  if (DiskNum != 0 || CdDisk != 0 || DiskEntries != TotalEntries) {
    return false;
  }
  if (TotalEntries == ZIP64_SENTINEL_16 || CdSize == ZIP64_SENTINEL_32 ||
      CdOffset == ZIP64_SENTINEL_32) {
    return false;
  }
  if (TotalEntries > ZIP_MAX_ENTRIES) {
    return false;
  }
  if ((size_t)CdOffset > FileSize || (size_t)CdSize > FileSize - (size_t)CdOffset) {
    return false;
  }

  *OutEntries = TotalEntries;
  *OutCdSize = CdSize;
  *OutCdOffset = CdOffset;
  return true;
}

static bool ParseCentralDir(ZipArchive *Archive, const unsigned char *CdBuf,
                            size_t CdSize, uint16_t TotalEntries) {
  size_t Pos = 0;
  for (uint16_t Idx = 0; Idx < TotalEntries; Idx++) {
    if (CdSize - Pos < (size_t)ZIP_CD_HEADER_SIZE) {
      return false;
    }

    const unsigned char *Hdr = &CdBuf[Pos];
    if (ReadLe32(Hdr) != ZIP_SIG_CD) {
      return false;
    }

    uint16_t Flags = ReadLe16(&Hdr[CD_FLAGS_OFF]);
    uint16_t Method = ReadLe16(&Hdr[CD_METHOD_OFF]);
    uint32_t CompSize = ReadLe32(&Hdr[CD_COMP_SIZE_OFF]);
    uint32_t UncompSize = ReadLe32(&Hdr[CD_UNCOMP_SIZE_OFF]);
    uint16_t NameLen = ReadLe16(&Hdr[CD_NAME_LEN_OFF]);
    uint16_t ExtraLen = ReadLe16(&Hdr[CD_EXTRA_LEN_OFF]);
    uint16_t CommentLen = ReadLe16(&Hdr[CD_COMMENT_LEN_OFF]);
    uint16_t DiskStart = ReadLe16(&Hdr[CD_DISK_START_OFF]);
    uint32_t LocalOffset = ReadLe32(&Hdr[CD_LOCAL_OFFSET_OFF]);

    size_t RecordSize = (size_t)ZIP_CD_HEADER_SIZE + (size_t)NameLen +
                        (size_t)ExtraLen + (size_t)CommentLen;
    if (RecordSize > CdSize - Pos) {
      return false;
    }

    ZipEntry *Entry = &Archive->Entries[Idx];
    const char *RawName = (const char *)&Hdr[ZIP_CD_HEADER_SIZE];
    size_t CopyLen = NameLen;
    bool NameFits = (NameLen > 0) && (NameLen < ZIP_MAX_ENTRY_NAME);
    if (CopyLen >= (size_t)ZIP_MAX_ENTRY_NAME) {
      CopyLen = (size_t)ZIP_MAX_ENTRY_NAME - 1U;
    }
    if (CopyLen > 0) {
      memcpy(Entry->Name, RawName, CopyLen);
    }
    Entry->Name[CopyLen] = '\0';

    bool SupportedMethod =
        (Method == ZIP_METHOD_STORED) || (Method == ZIP_METHOD_DEFLATE);
    bool NotEncrypted = (Flags & ZIP_FLAG_ENCRYPTED) == 0;
    bool NotZip64 = (CompSize != ZIP64_SENTINEL_32) &&
                    (UncompSize != ZIP64_SENTINEL_32) &&
                    (LocalOffset != ZIP64_SENTINEL_32);
    bool WithinSizeCeiling =
        (UncompSize <= (uint32_t)ZIP_MAX_UNCOMPRESSED_SIZE) &&
        (CompSize <= (uint32_t)ZIP_MAX_UNCOMPRESSED_SIZE) &&
        ((Method != ZIP_METHOD_STORED) || (CompSize == UncompSize));
    bool WithinFileBounds =
        ((size_t)LocalOffset <= Archive->FileSize - (size_t)ZIP_LOCAL_HEADER_SIZE) &&
        ((size_t)CompSize <=
         Archive->FileSize - (size_t)LocalOffset - (size_t)ZIP_LOCAL_HEADER_SIZE);

    Entry->LocalHeaderOffset = LocalOffset;
    Entry->CompressedSize = CompSize;
    Entry->UncompressedSize = UncompSize;
    Entry->Method = Method;
    Entry->Valid = NameFits && (DiskStart == 0) && SupportedMethod &&
                   NotEncrypted && NotZip64 && WithinSizeCeiling &&
                   WithinFileBounds;

    Pos += RecordSize;
  }

  return true;
}

ZipArchive *ZipOpen(const char *Path) {
  if (Path == NULL) {
    return NULL;
  }

  FILE *File = fopen(Path, "rb");
  if (File == NULL) {
    return NULL;
  }

  if (fseek(File, 0, SEEK_END) != 0) {
    (void)fclose(File);
    return NULL;
  }

  long SignedSize = ftell(File);
  if (SignedSize < ZIP_EOCD_MIN_SIZE) {
    (void)fclose(File);
    return NULL;
  }
  size_t FileSize = (size_t)SignedSize;

  uint16_t TotalEntries = 0;
  uint32_t CdSize = 0;
  uint32_t CdOffset = 0;
  if (!FindEndOfCentralDir(File, FileSize, &TotalEntries, &CdSize, &CdOffset)) {
    (void)fclose(File);
    return NULL;
  }

  ZipArchive *Archive = (ZipArchive *)calloc(1, sizeof(ZipArchive));
  if (Archive == NULL) {
    (void)fclose(File);
    return NULL;
  }
  Archive->File = File;
  Archive->FileSize = FileSize;
  Archive->EntryCount = (int)TotalEntries;

  if (TotalEntries == 0) {
    return Archive;
  }

  Archive->Entries = (ZipEntry *)calloc(TotalEntries, sizeof(ZipEntry));
  if (Archive->Entries == NULL) {
    ZipClose(Archive);
    return NULL;
  }

  if (CdSize == 0 || fseek(File, (long)CdOffset, SEEK_SET) != 0) {
    ZipClose(Archive);
    return NULL;
  }

  unsigned char *CdBuf = (unsigned char *)malloc(CdSize);
  if (CdBuf == NULL) {
    ZipClose(Archive);
    return NULL;
  }

  if (fread(CdBuf, 1, CdSize, File) != CdSize) {
    free(CdBuf);
    ZipClose(Archive);
    return NULL;
  }

  bool Ok = ParseCentralDir(Archive, CdBuf, CdSize, TotalEntries);
  free(CdBuf);
  if (!Ok) {
    ZipClose(Archive);
    return NULL;
  }

  return Archive;
}

const ZipEntry *ZipFind(const ZipArchive *Archive, const char *EntryName) {
  if (Archive == NULL || EntryName == NULL) {
    return NULL;
  }

  for (int Idx = 0; Idx < Archive->EntryCount; Idx++) {
    const ZipEntry *Entry = &Archive->Entries[Idx];
    if (Entry->Valid && strcmp(Entry->Name, EntryName) == 0) {
      return Entry;
    }
  }

  return NULL;
}

unsigned char *ZipReadRaw(ZipArchive *Archive, const ZipEntry *Entry,
                          int *OutSize, int *OutMethod) {
  if (Archive == NULL || Archive->File == NULL || Entry == NULL ||
      !Entry->Valid || OutSize == NULL || OutMethod == NULL) {
    return NULL;
  }

  if (fseek(Archive->File, (long)Entry->LocalHeaderOffset, SEEK_SET) != 0) {
    return NULL;
  }

  unsigned char LocalHdr[ZIP_LOCAL_HEADER_SIZE];
  if (fread(LocalHdr, 1, sizeof(LocalHdr), Archive->File) != sizeof(LocalHdr)) {
    return NULL;
  }

  if (ReadLe32(LocalHdr) != ZIP_SIG_LOCAL) {
    return NULL;
  }

  uint16_t LocalNameLen = ReadLe16(&LocalHdr[LH_NAME_LEN_OFF]);
  uint16_t LocalExtraLen = ReadLe16(&LocalHdr[LH_EXTRA_LEN_OFF]);
  size_t HeaderSpan = (size_t)ZIP_LOCAL_HEADER_SIZE + (size_t)LocalNameLen +
                      (size_t)LocalExtraLen;
  if (HeaderSpan > Archive->FileSize - (size_t)Entry->LocalHeaderOffset) {
    return NULL;
  }

  size_t DataOffset = (size_t)Entry->LocalHeaderOffset + HeaderSpan;
  size_t CompSize = (size_t)Entry->CompressedSize;
  if (CompSize > Archive->FileSize - DataOffset) {
    return NULL;
  }

  size_t AllocSize = (CompSize > 0) ? CompSize : 1U;
  unsigned char *Buf = (unsigned char *)malloc(AllocSize);
  if (Buf == NULL) {
    return NULL;
  }

  if (CompSize > 0) {
    if (fseek(Archive->File, (long)DataOffset, SEEK_SET) != 0 ||
        fread(Buf, 1, CompSize, Archive->File) != CompSize) {
      free(Buf);
      return NULL;
    }
  }

  *OutSize = (int)CompSize;
  *OutMethod = (int)Entry->Method;
  return Buf;
}

int ZipEntryUncompressedSize(const ZipEntry *Entry) {
  if (Entry == NULL || !Entry->Valid) {
    return 0;
  }
  return (int)Entry->UncompressedSize;
}

void ZipClose(ZipArchive *Archive) {
  if (Archive == NULL) {
    return;
  }
  if (Archive->File != NULL) {
    (void)fclose(Archive->File);
  }
  free(Archive->Entries);
  free(Archive);
}
