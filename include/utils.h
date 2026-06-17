#ifndef UTILS_H
#define UTILS_H

typedef __SIZE_TYPE__ SizeType;

void SafeStrncpy(char *Dest, const char *Src, int MaxLen);
int CompareString(const char *Str1, const char *Str2);
float ParseFloat(const char *Str, char **EndPtr);
int ParseInt(const char *Str, char **EndPtr);

#endif
