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

enum {
  UTF8_MAX_BYTES = 4
};

// Encodes a Unicode codepoint as UTF-8 into Out and returns the byte count
// (1-4), or 0 for a value outside the Unicode range or a surrogate half.
int EncodeUtf8(unsigned int Codepoint, char Out[UTF8_MAX_BYTES]);

#endif
