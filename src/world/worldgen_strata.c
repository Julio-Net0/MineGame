#include "world/worldgen_strata.h"
#include "core/fileio.h"
#include "core/log.h"
#include "core/utils.h"
#include "third_party/cJSON.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

enum {
  DEFAULT_GENERATION_CEILING = 640,
  DEFAULT_OVERWORLD_MIN_Y = -511,
  DEFAULT_BEDROCK_MIN_Y = -516,
  DEFAULT_BEDROCK_MAX_Y = -512
};

typedef struct {
  Stratum Strata[MAX_STRATA];
  int Count;
  int GenerationCeiling;
} StrataTable;

static StrataTable *GetStrataTable(void) {
  static StrataTable STable = {.Count = 0,
                               .GenerationCeiling = DEFAULT_GENERATION_CEILING};
  return &STable;
}

static const char *const GENERATOR_NAMES[STRATUM_GEN_COUNT] = {
    "overworld", "bedrock_band", "void"};

static bool ParseGenerator(const char *Name, StratumGenerator *Out) {
  for (int Idx = 0; Idx < STRATUM_GEN_COUNT; Idx++) {
    if (strcmp(Name, GENERATOR_NAMES[Idx]) == 0) {
      *Out = (StratumGenerator)Idx;
      return true;
    }
  }
  return false;
}

static Stratum MakeStratum(const char *Name, StratumGenerator Generator,
                           int MinY, int MaxY) {
  Stratum Entry = {.Generator = Generator, .MinY = MinY, .MaxY = MaxY};
  SafeStrncpy(Entry.Name, Name, MAX_STRATUM_NAME);
  return Entry;
}

static void InstallDefault(StrataTable *Table) {
  Table->Strata[0] = MakeStratum("reserved", STRATUM_GEN_VOID, INT_MIN,
                                 DEFAULT_BEDROCK_MIN_Y - 1);
  Table->Strata[1] = MakeStratum("bedrock", STRATUM_GEN_BEDROCK_BAND,
                                 DEFAULT_BEDROCK_MIN_Y, DEFAULT_BEDROCK_MAX_Y);
  Table->Strata[2] = MakeStratum("overworld", STRATUM_GEN_OVERWORLD,
                                 DEFAULT_OVERWORLD_MIN_Y, INT_MAX);
  Table->Count = 3;
  Table->GenerationCeiling = DEFAULT_GENERATION_CEILING;
}

void InitStrata(void) { InstallDefault(GetStrataTable()); }

static int CompareStrataByMinY(const void *Left, const void *Right) {
  const Stratum *A = (const Stratum *)Left;
  const Stratum *B = (const Stratum *)Right;
  if (A->MinY < B->MinY) {
    return -1;
  }
  return (A->MinY > B->MinY) ? 1 : 0;
}

// Sorted ascending, the table is valid when it starts open below, ends open
// above, and each stratum begins exactly one block above the previous one's top:
// that rules out both gaps and overlaps with one comparison per pair.
static bool ValidateCoverage(const StrataTable *Table, const char *FilePath) {
  if (Table->Count == 0) {
    LogError("STRATA: %s declares no strata", FilePath);
    return false;
  }
  if (Table->Strata[0].MinY != INT_MIN) {
    LogError("STRATA: %s leaves Y below %d uncovered", FilePath,
             Table->Strata[0].MinY);
    return false;
  }
  if (Table->Strata[Table->Count - 1].MaxY != INT_MAX) {
    LogError("STRATA: %s leaves Y above %d uncovered", FilePath,
             Table->Strata[Table->Count - 1].MaxY);
    return false;
  }
  for (int Idx = 0; Idx < Table->Count; Idx++) {
    const Stratum *Entry = &Table->Strata[Idx];
    if (Entry->MinY > Entry->MaxY) {
      LogError("STRATA: %s stratum '%s' has minY above maxY", FilePath,
               Entry->Name);
      return false;
    }
    if (Idx == 0) {
      continue;
    }
    const Stratum *Below = &Table->Strata[Idx - 1];
    // Checked before any Below->MaxY + 1, which would overflow at INT_MAX.
    if (Below->MaxY == INT_MAX || Entry->MinY <= Below->MaxY) {
      LogError("STRATA: %s strata '%s' and '%s' overlap", FilePath,
               Below->Name, Entry->Name);
      return false;
    }
    if (Entry->MinY > Below->MaxY + 1) {
      LogError("STRATA: %s leaves Y %d..%d uncovered between '%s' and '%s'",
               FilePath, Below->MaxY + 1, Entry->MinY - 1, Below->Name,
               Entry->Name);
      return false;
    }
  }
  return true;
}

static bool ParseBound(const cJSON *Object, const char *Key, int OpenValue,
                       int *Out) {
  const cJSON *Item = cJSON_GetObjectItemCaseSensitive(Object, Key);
  if (Item == NULL) {
    *Out = OpenValue;
    return true;
  }
  if (cJSON_IsNumber(Item) == 0) {
    return false;
  }
  *Out = Item->valueint;
  return true;
}

static bool ParseStratum(const cJSON *Item, Stratum *Out,
                         const char *FilePath) {
  const cJSON *NameItem = cJSON_GetObjectItemCaseSensitive(Item, "name");
  const cJSON *GenItem = cJSON_GetObjectItemCaseSensitive(Item, "generator");
  if (cJSON_IsString(NameItem) == 0 || cJSON_IsString(GenItem) == 0) {
    LogError("STRATA: %s has a stratum without a name or generator", FilePath);
    return false;
  }

  StratumGenerator Generator = STRATUM_GEN_VOID;
  if (!ParseGenerator(GenItem->valuestring, &Generator)) {
    LogError("STRATA: %s stratum '%s' names unknown generator '%s'", FilePath,
             NameItem->valuestring, GenItem->valuestring);
    return false;
  }

  int MinY = 0;
  int MaxY = 0;
  if (!ParseBound(Item, "minY", INT_MIN, &MinY) ||
      !ParseBound(Item, "maxY", INT_MAX, &MaxY)) {
    LogError("STRATA: %s stratum '%s' has a non-numeric bound", FilePath,
             NameItem->valuestring);
    return false;
  }

  *Out = MakeStratum(NameItem->valuestring, Generator, MinY, MaxY);
  return true;
}

static bool ParseTable(const cJSON *Json, StrataTable *Out,
                       const char *FilePath) {
  const cJSON *CeilingItem =
      cJSON_GetObjectItemCaseSensitive(Json, "generationCeiling");
  Out->GenerationCeiling = (cJSON_IsNumber(CeilingItem) != 0)
                               ? CeilingItem->valueint
                               : DEFAULT_GENERATION_CEILING;

  const cJSON *List = cJSON_GetObjectItemCaseSensitive(Json, "strata");
  if (cJSON_IsArray(List) == 0) {
    LogError("STRATA: %s has no \"strata\" array", FilePath);
    return false;
  }

  Out->Count = 0;
  const cJSON *Item = NULL;
  cJSON_ArrayForEach(Item, List) {
    if (Out->Count >= MAX_STRATA) {
      LogError("STRATA: %s declares more than %d strata", FilePath,
               MAX_STRATA);
      return false;
    }
    if (!ParseStratum(Item, &Out->Strata[Out->Count], FilePath)) {
      return false;
    }
    Out->Count++;
  }

  qsort(Out->Strata, (size_t)Out->Count, sizeof(Stratum), CompareStrataByMinY);
  return ValidateCoverage(Out, FilePath);
}

void LoadStrata(const char *FilePath) {
  StrataTable *Table = GetStrataTable();

  char *FileContent = ReadTextFile(FilePath);
  if (FileContent == NULL) {
    LogWarn("STRATA: no table at %s, using the built-in default", FilePath);
    InstallDefault(Table);
    return;
  }

  cJSON *Json = cJSON_Parse(FileContent);
  free(FileContent);
  if (Json == NULL) {
    LogError("STRATA: JSON syntax error in %s, using the built-in default",
             FilePath);
    InstallDefault(Table);
    return;
  }

  StrataTable Parsed = {.Count = 0};
  if (ParseTable(Json, &Parsed, FilePath)) {
    *Table = Parsed;
    LogInfo("STRATA: %d strata loaded, generation ceiling Y=%d", Table->Count,
            Table->GenerationCeiling);
  } else {
    LogError("STRATA: %s rejected, using the built-in default", FilePath);
    InstallDefault(Table);
  }
  cJSON_Delete(Json);
}

StratumGenerator ResolveStratum(int GlobalY) {
  const StrataTable *Table = GetStrataTable();
  for (int Idx = 0; Idx < Table->Count; Idx++) {
    if (GlobalY >= Table->Strata[Idx].MinY &&
        GlobalY <= Table->Strata[Idx].MaxY) {
      return Table->Strata[Idx].Generator;
    }
  }
  return STRATUM_GEN_VOID;
}

int GetGenerationCeiling(void) { return GetStrataTable()->GenerationCeiling; }

bool GetStratumRange(StratumGenerator Generator, int *MinY, int *MaxY) {
  const StrataTable *Table = GetStrataTable();
  for (int Idx = 0; Idx < Table->Count; Idx++) {
    if (Table->Strata[Idx].Generator == Generator) {
      *MinY = Table->Strata[Idx].MinY;
      *MaxY = Table->Strata[Idx].MaxY;
      return true;
    }
  }
  return false;
}
