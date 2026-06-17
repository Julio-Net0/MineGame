#include "utils.h"

enum {
  MAX_STR_LOOP = 512,
  BASE_DECIMAL = 10
};

static const float MULTIPLIER_TEN = 10.0F;
static const float MULTIPLIER_TENTH = 0.1F;

void SafeStrncpy(char *Dest, const char *Src, int MaxLen) {
  if (MaxLen <= 0 || Dest == (void*)0 || Src == (void*)0) {
    return;
  }
  int Index = 0;
  #pragma unroll 4
  for (int LoopIdx = 0; LoopIdx < MAX_STR_LOOP; LoopIdx++) {
    if (Index < MaxLen - 1 && Src[Index] != '\0') {
      Dest[Index] = Src[Index];
      Index++;
    } else {
      break;
    }
  }
  Dest[Index] = '\0';
}

int CompareString(const char *Str1, const char *Str2) {
  if (Str1 == (void*)0 || Str2 == (void*)0) {
    if (Str1 == Str2) {
      return 0;
    }
    return (Str1 == (void*)0) ? -1 : 1;
  }
  int Index = 0;
  #pragma unroll 4
  for (int LoopIdx = 0; LoopIdx < MAX_STR_LOOP; LoopIdx++) {
    if (Str1[Index] != '\0' && Str2[Index] != '\0' && Str1[Index] == Str2[Index]) {
      Index++;
    } else {
      break;
    }
  }
  return (unsigned char)Str1[Index] - (unsigned char)Str2[Index];
}

float ParseFloat(const char *Str, char **EndPtr) {
  if (Str == (void*)0) {
    if (EndPtr != (char **)0) {
      *EndPtr = (char *)0;
    }
    return 0.0F;
  }
  int SpaceIndex = 0;
  #pragma unroll 4
  for (int Index = 0; Index < MAX_STR_LOOP; Index++) {
    if (Str[SpaceIndex] == ' ') {
      SpaceIndex++;
    } else {
      break;
    }
  }

  float Sign = 1.0F;
  int SignIndex = SpaceIndex;
  if (Str[SignIndex] == '-') {
    Sign = -1.0F;
    SignIndex++;
  } else if (Str[SignIndex] == '+') {
    SignIndex++;
  }

  float Value = 0.0F;
  int DigitIndex = SignIndex;
  #pragma unroll 4
  for (int Index = 0; Index < MAX_STR_LOOP; Index++) {
    char CharVal = Str[DigitIndex];
    if (CharVal >= '0' && CharVal <= '9') {
      Value = (Value * MULTIPLIER_TEN) + (float)(CharVal - '0');
      DigitIndex++;
    } else {
      break;
    }
  }

  int FinalIndex = DigitIndex;
  if (Str[FinalIndex] == '.') {
    FinalIndex++;
    float Factor = MULTIPLIER_TENTH;
    int DecIndex = FinalIndex;
    #pragma unroll 4
    for (int Index = 0; Index < MAX_STR_LOOP; Index++) {
      char CharVal = Str[DecIndex];
      if (CharVal >= '0' && CharVal <= '9') {
        Value += (float)(CharVal - '0') * Factor;
        Factor *= MULTIPLIER_TENTH;
        DecIndex++;
      } else {
        break;
      }
    }
    FinalIndex = DecIndex;
  }

  if (EndPtr != (char **)0) {
    *EndPtr = (char *)(Str + FinalIndex);
  }
  return Sign * Value;
}

int ParseInt(const char *Str, char **EndPtr) {
  if (Str == (void*)0) {
    if (EndPtr != (char **)0) {
      *EndPtr = (char *)0;
    }
    return 0;
  }
  int SpaceIndex = 0;
  #pragma unroll 4
  for (int Index = 0; Index < MAX_STR_LOOP; Index++) {
    if (Str[SpaceIndex] == ' ') {
      SpaceIndex++;
    } else {
      break;
    }
  }

  int Sign = 1;
  int SignIndex = SpaceIndex;
  if (Str[SignIndex] == '-') {
    Sign = -1;
    SignIndex++;
  } else if (Str[SignIndex] == '+') {
    SignIndex++;
  }

  int Value = 0;
  int DigitIndex = SignIndex;
  #pragma unroll 4
  for (int Index = 0; Index < MAX_STR_LOOP; Index++) {
    char CharVal = Str[DigitIndex];
    if (CharVal >= '0' && CharVal <= '9') {
      Value = (Value * BASE_DECIMAL) + (CharVal - '0');
      DigitIndex++;
    } else {
      break;
    }
  }

  if (EndPtr != (char **)0) {
    *EndPtr = (char *)(Str + DigitIndex);
  }
  return Sign * Value;
}
