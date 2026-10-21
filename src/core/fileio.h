#ifndef FILEIO_H
#define FILEIO_H

#include <stdbool.h>

enum {
  FILEIO_MAX_PATH = 512,
  ZIP_METHOD_STORED = 0,
  ZIP_METHOD_DEFLATE = 8
};

typedef struct ZipArchive ZipArchive;
typedef struct ZipEntry ZipEntry;

// Read an entire text file into a heap buffer (caller frees). Null on error.
char *ReadTextFile(const char *Path);

// Read an entire file into a heap buffer (caller frees) and write its byte
// length to OutSize. Null on error.
unsigned char *ReadBinaryFile(const char *Path, int *OutSize);

// True if Path names an existing directory.
bool IsDirectory(const char *Path);

// Write Content to Path, truncating any existing file. Returns true on success.
bool WriteTextFile(const char *Path, const char *Content);

// Enumerate directory entries into OutPaths, each a full "DirPath/name" up to
// FILEIO_MAX_PATH chars. Returns the number written (capped at MaxFiles);
// skips "." and "..". Caller filters by extension as needed.
int ListDirectoryFiles(const char *DirPath, char OutPaths[][FILEIO_MAX_PATH],
                       int MaxFiles);

// True if Path ends with Ext (case-insensitive), e.g. Ext ".json".
bool HasFileExtension(const char *Path, const char *Ext);

// Open a .zip archive and parse its Central Directory once. Returns null when
// the file cannot be opened or its directory is malformed or truncated.
ZipArchive *ZipOpen(const char *Path);

// Find a validated entry by exact archive-relative path. Returns null when the
// entry is absent or failed bounds/format validation.
const ZipEntry *ZipFind(const ZipArchive *Archive, const char *EntryName);

// Read the raw (stored or DEFLATE-compressed) payload of Entry into a heap
// buffer (caller frees with free()). Writes the payload byte length to OutSize
// and the compression method (ZIP_METHOD_STORED or ZIP_METHOD_DEFLATE) to
// OutMethod. Returns null on any bounds or I/O error.
unsigned char *ZipReadRaw(ZipArchive *Archive, const ZipEntry *Entry,
                          int *OutSize, int *OutMethod);

// Uncompressed byte length the Central Directory declares for Entry, already
// validated against the reader's size ceiling. Zero for a null entry.
int ZipEntryUncompressedSize(const ZipEntry *Entry);

// Close the archive and free its directory index.
void ZipClose(ZipArchive *Archive);

#endif
