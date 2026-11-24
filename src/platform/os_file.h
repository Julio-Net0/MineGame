#ifndef PLATFORM_OS_FILE_H
#define PLATFORM_OS_FILE_H

#include <stdbool.h>

// Creates Path and every missing parent directory. True when the directory
// exists afterwards.
bool OsMakeDirectories(const char *Path);

// Moves Source over Target, replacing Target in one step where the file system
// allows it, so a reader sees either the old file or the new one, never a
// partial one.
bool OsReplaceFile(const char *Source, const char *Target);

// Removes an empty directory.
bool OsRemoveDirectory(const char *Path);

#endif
