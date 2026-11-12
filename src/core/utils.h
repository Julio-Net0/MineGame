#ifndef UTILS_H
#define UTILS_H

#include <stdbool.h>

typedef __SIZE_TYPE__ SizeType;

void SafeStrncpy(char *Dest, const char *Src, int MaxLen);
int CompareString(const char *Str1, const char *Str2);
float ParseFloat(const char *Str, char **EndPtr);
int ParseInt(const char *Str, char **EndPtr);
bool NameMatchesExact(const char *Name, const char *Query);
bool NameMatchesPrefix(const char *Name, const char *Query);
bool PrefixMatchesNoCase(const char *Str, const char *Prefix, SizeType PrefixLen);

#endif
