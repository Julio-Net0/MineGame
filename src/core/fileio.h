#ifndef FILEIO_H
#define FILEIO_H

#include <stdbool.h>

enum {
  FILEIO_MAX_PATH = 512
};

// Read an entire text file into a heap buffer (caller frees). Null on error.
char *ReadTextFile(const char *Path);

// Enumerate directory entries into OutPaths, each a full "DirPath/name" up to
// FILEIO_MAX_PATH chars. Returns the number written (capped at MaxFiles);
// skips "." and "..". Caller filters by extension as needed.
int ListDirectoryFiles(const char *DirPath, char OutPaths[][FILEIO_MAX_PATH],
                       int MaxFiles);

// True if Path ends with Ext (case-insensitive), e.g. Ext ".json".
bool HasFileExtension(const char *Path, const char *Ext);

#endif
