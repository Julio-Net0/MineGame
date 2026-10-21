#include "ui/font.h"
#include "core/fileio.h"
#include "core/log.h"
#include "core/resource_pack.h"
#include "third_party/cJSON.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  MAX_FONT_SHEETS = 16,
  MAX_REFERENCE_DEPTH = 8,
  LATIN_TABLE_SIZE = 256,
  DEFAULT_BITMAP_HEIGHT = 8,
  // Vanilla puts the baseline 7 units below the top of a line; a provider's
  // ascent says how far above the baseline its cells start.
  FONT_BASELINE = 7,
  PLACEHOLDER_WIDTH = 5,
  PLACEHOLDER_HEIGHT = 8,
  PLACEHOLDER_ADVANCE = 6,
  MAX_LOGGED_MISSING = 64,
  INITIAL_GLYPH_CAPACITY = 512,
  NO_GLYPH = -1,
  SPACE_SHEET = -1,
  ALPHA_OFFSET = 3,
  SHADOW_DIVISOR = 4,
  ESCAPE_DIGITS = 4,

  UTF8_CONTINUATION_MASK = 0xC0,
  UTF8_CONTINUATION_TAG = 0x80,
  UTF8_PAYLOAD_MASK = 0x3F,
  UTF8_PAYLOAD_BITS = 6,
  UTF8_TWO_MASK = 0xE0,
  UTF8_TWO_TAG = 0xC0,
  UTF8_TWO_LEAD_BITS = 0x1F,
  UTF8_THREE_MASK = 0xF0,
  UTF8_THREE_TAG = 0xE0,
  UTF8_THREE_LEAD_BITS = 0x0F,
  UTF8_FOUR_MASK = 0xF8,
  UTF8_FOUR_TAG = 0xF0,
  UTF8_FOUR_LEAD_BITS = 0x07,
  UTF8_ASCII_LIMIT = 0x80
};

// cJSON ends a string at an embedded NUL, but font definitions mark empty
// cells with \u0000. Those escapes are rewritten to ￿ (a Unicode
// noncharacter, same length) before parsing, and U+FFFF reads back as empty.
#define EMPTY_CELL_CODEPOINT 0xFFFFU
#define REPLACEMENT_CODEPOINT 0xFFFDU
#define GLYPH_ROUNDING 0.5F

typedef struct {
  UiSpriteId Sprite;
  int Columns;
  int Rows;
  int Height;
  int Ascent;
  // Filled once the sheet is packed.
  int CellWidth;
  int CellHeight;
  float Scale;
  bool Usable;
} FontSheet;

typedef struct {
  uint32_t Codepoint;
  // Definition order, so that when two providers define the same codepoint
  // the one listed first wins.
  int Order;
  int Sheet;
  int Column;
  int Row;
  int PixelWidth;
  float Advance;
} FontGlyph;

typedef struct {
  FontSheet Sheets[MAX_FONT_SHEETS];
  int SheetCount;
  FontGlyph *Glyphs;
  int GlyphCount;
  int GlyphCapacity;
  int Latin[LATIN_TABLE_SIZE];
  uint32_t LoggedMissing[MAX_LOGGED_MISSING];
  int LoggedCount;
  bool LoggedUnihex;
  bool LoggedTtf;
} FontState;

static FontState *GetFontState(void) {
  static FontState State;
  return &State;
}

// Decodes one UTF-8 sequence, returning the bytes consumed (at least one, and
// never past a terminating NUL). Malformed input decodes as U+FFFD.
static int DecodeUtf8(const unsigned char *Str, uint32_t *OutCodepoint) {
  unsigned char Lead = Str[0];
  if (Lead < UTF8_ASCII_LIMIT) {
    *OutCodepoint = Lead;
    return 1;
  }

  int Length = 0;
  uint32_t Codepoint = 0;
  if ((Lead & UTF8_TWO_MASK) == UTF8_TWO_TAG) {
    Length = 2;
    Codepoint = Lead & (unsigned)UTF8_TWO_LEAD_BITS;
  } else if ((Lead & UTF8_THREE_MASK) == UTF8_THREE_TAG) {
    Length = 3;
    Codepoint = Lead & (unsigned)UTF8_THREE_LEAD_BITS;
  } else if ((Lead & UTF8_FOUR_MASK) == UTF8_FOUR_TAG) {
    Length = 4;
    Codepoint = Lead & (unsigned)UTF8_FOUR_LEAD_BITS;
  } else {
    *OutCodepoint = REPLACEMENT_CODEPOINT;
    return 1;
  }

  for (int Idx = 1; Idx < Length; Idx++) {
    unsigned char Byte = Str[Idx];
    if ((Byte & UTF8_CONTINUATION_MASK) != UTF8_CONTINUATION_TAG) {
      *OutCodepoint = REPLACEMENT_CODEPOINT;
      return Idx;
    }
    Codepoint = (Codepoint << UTF8_PAYLOAD_BITS) |
                (Byte & (unsigned)UTF8_PAYLOAD_MASK);
  }
  *OutCodepoint = Codepoint;
  return Length;
}

// ---------------------------------------------------------------------------
// Definition parsing
// ---------------------------------------------------------------------------

// "minecraft:include/default" -> "include/default". Other namespaces are not
// shipped by vanilla and are rejected.
static bool StripNamespace(const char *Id, char *Out, size_t OutSize) {
  const char *Colon = strchr(Id, ':');
  if (Colon == NULL) {
    snprintf(Out, OutSize, "%s", Id);
    return true;
  }
  if ((size_t)(Colon - Id) != strlen("minecraft") ||
      strncmp(Id, "minecraft", strlen("minecraft")) != 0) {
    LogWarn("FONT: resource '%s' is outside the minecraft namespace, skipping",
            Id);
    return false;
  }
  snprintf(Out, OutSize, "%s", Colon + 1);
  return true;
}

static void RewriteEmptyCellEscapes(char *Text, size_t Size) {
  for (size_t Idx = 0; Idx + 1 + 1 + ESCAPE_DIGITS <= Size; Idx++) {
    if (Text[Idx] != '\\') {
      continue;
    }
    if (memcmp(&Text[Idx + 1], "u0000", 1 + ESCAPE_DIGITS) == 0) {
      memcpy(&Text[Idx + 2], "ffff", ESCAPE_DIGITS);
    }
    // Skip the escaped character, so an escaped backslash is not mistaken
    // for the start of another escape.
    Idx++;
  }
}

static bool AddGlyph(FontState *State, FontGlyph Glyph) {
  if (State->GlyphCount >= State->GlyphCapacity) {
    int NewCapacity = (State->GlyphCapacity > 0) ? (State->GlyphCapacity * 2)
                                                 : INITIAL_GLYPH_CAPACITY;
    FontGlyph *Grown = (FontGlyph *)realloc(
        State->Glyphs, (size_t)NewCapacity * sizeof(FontGlyph));
    if (Grown == NULL) {
      LogError("FONT: out of memory growing the glyph table");
      return false;
    }
    State->Glyphs = Grown;
    State->GlyphCapacity = NewCapacity;
  }
  Glyph.Order = State->GlyphCount;
  State->Glyphs[State->GlyphCount++] = Glyph;
  return true;
}

// A provider is used when every flag its filter names has the value this
// client runs with. No option is enabled yet, so "uniform: false" passes and
// "jp: true" does not.
static bool PassesFilter(const cJSON *Provider) {
  const cJSON *Filter = cJSON_GetObjectItemCaseSensitive(Provider, "filter");
  if (cJSON_IsObject(Filter) == 0) {
    return true;
  }
  const cJSON *Option = NULL;
  cJSON_ArrayForEach(Option, Filter) {
    if (cJSON_IsTrue(Option) != 0) {
      return false;
    }
  }
  return true;
}

static int CountCodepoints(const char *Row) {
  int Count = 0;
  const unsigned char *Cursor = (const unsigned char *)Row;
  while (*Cursor != 0) {
    uint32_t Codepoint = 0;
    Cursor += DecodeUtf8(Cursor, &Codepoint);
    Count++;
  }
  return Count;
}

static void AddBitmapProvider(FontState *State, const cJSON *Provider) {
  const cJSON *File = cJSON_GetObjectItemCaseSensitive(Provider, "file");
  const cJSON *Chars = cJSON_GetObjectItemCaseSensitive(Provider, "chars");
  const cJSON *Ascent = cJSON_GetObjectItemCaseSensitive(Provider, "ascent");
  const cJSON *Height = cJSON_GetObjectItemCaseSensitive(Provider, "height");
  if (cJSON_IsString(File) == 0 || cJSON_IsArray(Chars) == 0 ||
      cJSON_IsNumber(Ascent) == 0) {
    LogWarn("FONT: bitmap provider without file, chars or ascent, skipping");
    return;
  }
  if (State->SheetCount >= MAX_FONT_SHEETS) {
    LogWarn("FONT: too many bitmap sheets, skipping '%s'", File->valuestring);
    return;
  }

  char TexturePath[FILEIO_MAX_PATH];
  if (!StripNamespace(File->valuestring, TexturePath, sizeof(TexturePath))) {
    return;
  }

  int SheetIdx = State->SheetCount;
  FontSheet *Sheet = &State->Sheets[SheetIdx];
  *Sheet = (FontSheet){0};
  Sheet->Sprite = GuiAtlasRegisterTexture(TexturePath);
  Sheet->Height = cJSON_IsNumber(Height) ? (int)cJSON_GetNumberValue(Height)
                                         : DEFAULT_BITMAP_HEIGHT;
  Sheet->Ascent = (int)cJSON_GetNumberValue(Ascent);
  Sheet->Rows = cJSON_GetArraySize(Chars);

  const cJSON *Row = NULL;
  int RowIdx = 0;
  cJSON_ArrayForEach(Row, Chars) {
    if (cJSON_IsString(Row) == 0) {
      RowIdx++;
      continue;
    }
    if (RowIdx == 0) {
      Sheet->Columns = CountCodepoints(Row->valuestring);
    }
    const unsigned char *Cursor = (const unsigned char *)Row->valuestring;
    for (int Col = 0; *Cursor != 0 && Col < Sheet->Columns; Col++) {
      uint32_t Codepoint = 0;
      Cursor += DecodeUtf8(Cursor, &Codepoint);
      if (Codepoint == 0 || Codepoint == EMPTY_CELL_CODEPOINT) {
        continue;
      }
      FontGlyph Glyph = {0};
      Glyph.Codepoint = Codepoint;
      Glyph.Sheet = SheetIdx;
      Glyph.Column = Col;
      Glyph.Row = RowIdx;
      if (!AddGlyph(State, Glyph)) {
        return;
      }
    }
    RowIdx++;
  }

  if (Sheet->Rows > 0 && Sheet->Columns > 0 && Sheet->Height > 0) {
    State->SheetCount++;
  } else {
    LogWarn("FONT: bitmap provider '%s' has an empty grid, skipping",
            File->valuestring);
  }
}

static void AddSpaceProvider(FontState *State, const cJSON *Provider) {
  const cJSON *Advances =
      cJSON_GetObjectItemCaseSensitive(Provider, "advances");
  const cJSON *Entry = NULL;
  cJSON_ArrayForEach(Entry, Advances) {
    if (Entry->string == NULL || cJSON_IsNumber(Entry) == 0) {
      continue;
    }
    FontGlyph Glyph = {0};
    DecodeUtf8((const unsigned char *)Entry->string, &Glyph.Codepoint);
    Glyph.Sheet = SPACE_SHEET;
    Glyph.Advance = (float)cJSON_GetNumberValue(Entry);
    if (!AddGlyph(State, Glyph)) {
      return;
    }
  }
}

static void LoadDefinition(FontState *State, const char *Id, int Depth);

static void LoadProvider(FontState *State, const cJSON *Provider, int Depth) {
  if (!PassesFilter(Provider)) {
    return;
  }
  const cJSON *Type = cJSON_GetObjectItemCaseSensitive(Provider, "type");
  if (cJSON_IsString(Type) == 0) {
    return;
  }
  const char *Name = Type->valuestring;
  if (strcmp(Name, "reference") == 0) {
    const cJSON *Ref = cJSON_GetObjectItemCaseSensitive(Provider, "id");
    if (cJSON_IsString(Ref) != 0) {
      LoadDefinition(State, Ref->valuestring, Depth + 1);
    }
  } else if (strcmp(Name, "bitmap") == 0) {
    AddBitmapProvider(State, Provider);
  } else if (strcmp(Name, "space") == 0) {
    AddSpaceProvider(State, Provider);
  } else if (strcmp(Name, "unihex") == 0) {
    if (!State->LoggedUnihex) {
      LogInfo("FONT: unihex providers are not supported, skipping them");
      State->LoggedUnihex = true;
    }
  } else if (strcmp(Name, "ttf") == 0) {
    if (!State->LoggedTtf) {
      LogInfo("FONT: ttf providers are not supported, skipping them");
      State->LoggedTtf = true;
    }
  } else {
    LogWarn("FONT: unknown provider type '%s', skipping", Name);
  }
}

static void LoadDefinition(FontState *State, const char *Id, int Depth) {
  if (Depth > MAX_REFERENCE_DEPTH) {
    LogWarn("FONT: reference chain deeper than %d at '%s', stopping",
            MAX_REFERENCE_DEPTH, Id);
    return;
  }

  char Rel[FILEIO_MAX_PATH];
  if (!StripNamespace(Id, Rel, sizeof(Rel))) {
    return;
  }
  char Path[FILEIO_MAX_PATH];
  snprintf(Path, sizeof(Path), "%s.json", Rel);

  int Size = 0;
  char *Text = (char *)ResourceRead(RESOURCE_FONT_DEF, Path, &Size, NULL);
  if (Text == NULL) {
    LogWarn("FONT: font definition '%s' not found", Path);
    return;
  }
  RewriteEmptyCellEscapes(Text, (size_t)Size);
  cJSON *Json = cJSON_ParseWithLength(Text, (size_t)Size);
  free(Text);
  if (Json == NULL) {
    LogWarn("FONT: malformed font definition '%s'", Path);
    return;
  }

  const cJSON *Providers = cJSON_GetObjectItemCaseSensitive(Json, "providers");
  const cJSON *Provider = NULL;
  cJSON_ArrayForEach(Provider, Providers) { LoadProvider(State, Provider, Depth); }
  cJSON_Delete(Json);
}

static int CompareGlyphs(const void *Lhs, const void *Rhs) {
  const FontGlyph *GlyphA = (const FontGlyph *)Lhs;
  const FontGlyph *GlyphB = (const FontGlyph *)Rhs;
  if (GlyphA->Codepoint != GlyphB->Codepoint) {
    return (GlyphA->Codepoint < GlyphB->Codepoint) ? -1 : 1;
  }
  return GlyphA->Order - GlyphB->Order;
}

// Sorts by codepoint and keeps only the first definition of each, so lookup
// can binary-search and the earliest provider wins.
static void FinalizeGlyphs(FontState *State) {
  if (State->GlyphCount > 1) {
    qsort(State->Glyphs, (size_t)State->GlyphCount, sizeof(FontGlyph),
          CompareGlyphs);
  }
  int Kept = 0;
  for (int Idx = 0; Idx < State->GlyphCount; Idx++) {
    if (Kept > 0 &&
        State->Glyphs[Kept - 1].Codepoint == State->Glyphs[Idx].Codepoint) {
      continue;
    }
    State->Glyphs[Kept++] = State->Glyphs[Idx];
  }
  State->GlyphCount = Kept;

  for (int Idx = 0; Idx < LATIN_TABLE_SIZE; Idx++) {
    State->Latin[Idx] = NO_GLYPH;
  }
  for (int Idx = 0; Idx < State->GlyphCount; Idx++) {
    uint32_t Codepoint = State->Glyphs[Idx].Codepoint;
    if (Codepoint < LATIN_TABLE_SIZE) {
      State->Latin[Codepoint] = Idx;
    }
  }
}

void FontInit(void) {
  FontState *State = GetFontState();
  FontShutdown();
  LoadDefinition(State, "minecraft:default", 0);
  FinalizeGlyphs(State);
  LogInfo("FONT: %d glyphs from %d bitmap sheets", State->GlyphCount,
          State->SheetCount);
}

void FontShutdown(void) {
  FontState *State = GetFontState();
  free(State->Glyphs);
  *State = (FontState){0};
  for (int Idx = 0; Idx < LATIN_TABLE_SIZE; Idx++) {
    State->Latin[Idx] = NO_GLYPH;
  }
}

// ---------------------------------------------------------------------------
// Metrics
// ---------------------------------------------------------------------------

static int MeasureCellWidth(const CoreImage *Atlas, int OriginX, int OriginY,
                            int CellWidth, int CellHeight) {
  for (int Col = CellWidth - 1; Col >= 0; Col--) {
    for (int Row = 0; Row < CellHeight; Row++) {
      size_t Offset = ((((size_t)OriginY + (size_t)Row) * (size_t)Atlas->Width) +
                       (size_t)OriginX + (size_t)Col) *
                      (size_t)CORE_IMAGE_CHANNELS;
      if (Atlas->Pixels[Offset + ALPHA_OFFSET] != 0) {
        return Col + 1;
      }
    }
  }
  return 0;
}

void FontBuildMetrics(const CoreImage *Atlas) {
  FontState *State = GetFontState();
  for (int SheetIdx = 0; SheetIdx < State->SheetCount; SheetIdx++) {
    FontSheet *Sheet = &State->Sheets[SheetIdx];
    const GuiSprite *Sprite = GuiAtlasGetSprite(Sheet->Sprite);
    Sheet->Usable = false;
    if (!Sprite->Resolved) {
      LogWarn("FONT: a bitmap sheet is missing, its glyphs draw as boxes");
      continue;
    }
    Sheet->CellWidth = Sprite->PixelWidth / Sheet->Columns;
    Sheet->CellHeight = Sprite->PixelHeight / Sheet->Rows;
    if (Sheet->CellWidth <= 0 || Sheet->CellHeight <= 0) {
      continue;
    }
    Sheet->Scale = (float)Sheet->Height / (float)Sheet->CellHeight;
    Sheet->Usable = true;
  }

  for (int Idx = 0; Idx < State->GlyphCount; Idx++) {
    FontGlyph *Glyph = &State->Glyphs[Idx];
    if (Glyph->Sheet == SPACE_SHEET) {
      continue;
    }
    const FontSheet *Sheet = &State->Sheets[Glyph->Sheet];
    if (!Sheet->Usable) {
      continue;
    }
    const GuiSprite *Sprite = GuiAtlasGetSprite(Sheet->Sprite);
    int OriginX = Sprite->AtlasX + (Glyph->Column * Sheet->CellWidth);
    int OriginY = Sprite->AtlasY + (Glyph->Row * Sheet->CellHeight);
    Glyph->PixelWidth = MeasureCellWidth(Atlas, OriginX, OriginY,
                                         Sheet->CellWidth, Sheet->CellHeight);
    // Vanilla rounds the scaled width and leaves one unit of spacing.
    Glyph->Advance =
        floorf(((float)Glyph->PixelWidth * Sheet->Scale) + GLYPH_ROUNDING) +
        1.0F;
  }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

static const FontGlyph *FindGlyph(const FontState *State, uint32_t Codepoint) {
  if (Codepoint < LATIN_TABLE_SIZE) {
    int Idx = State->Latin[Codepoint];
    return (Idx == NO_GLYPH) ? NULL : &State->Glyphs[Idx];
  }
  int Low = 0;
  int High = State->GlyphCount - 1;
  while (Low <= High) {
    int Mid = Low + ((High - Low) / 2);
    uint32_t MidCodepoint = State->Glyphs[Mid].Codepoint;
    if (MidCodepoint == Codepoint) {
      return &State->Glyphs[Mid];
    }
    if (MidCodepoint < Codepoint) {
      Low = Mid + 1;
    } else {
      High = Mid - 1;
    }
  }
  return NULL;
}

static bool IsDrawable(const FontState *State, const FontGlyph *Glyph) {
  return Glyph != NULL &&
         (Glyph->Sheet == SPACE_SHEET || State->Sheets[Glyph->Sheet].Usable);
}

static void ReportMissing(FontState *State, uint32_t Codepoint) {
  for (int Idx = 0; Idx < State->LoggedCount; Idx++) {
    if (State->LoggedMissing[Idx] == Codepoint) {
      return;
    }
  }
  if (State->LoggedCount >= MAX_LOGGED_MISSING) {
    return;
  }
  State->LoggedMissing[State->LoggedCount++] = Codepoint;
  LogWarn("FONT: no glyph for U+%04X, drawing a box", (unsigned)Codepoint);
}

static void DrawPlaceholder(UiContext *Ctx, float X, float Y, Color8 Color) {
  float U = 0.0F;
  float V = 0.0F;
  GuiAtlasGetWhiteUV(&U, &V);
  float W = (float)PLACEHOLDER_WIDTH;
  float H = (float)PLACEHOLDER_HEIGHT;
  UiTexturedRect(Ctx, UI_TEXTURE_GUI_ATLAS, X, Y, W, 1.0F, U, V, U, V, Color);
  UiTexturedRect(Ctx, UI_TEXTURE_GUI_ATLAS, X, Y + H - 1.0F, W, 1.0F, U, V, U,
                 V, Color);
  UiTexturedRect(Ctx, UI_TEXTURE_GUI_ATLAS, X, Y, 1.0F, H, U, V, U, V, Color);
  UiTexturedRect(Ctx, UI_TEXTURE_GUI_ATLAS, X + W - 1.0F, Y, 1.0F, H, U, V, U,
                 V, Color);
}

static void DrawGlyph(UiContext *Ctx, const FontState *State,
                      const FontGlyph *Glyph, float X, float Y, Color8 Color) {
  const FontSheet *Sheet = &State->Sheets[Glyph->Sheet];
  const GuiSprite *Sprite = GuiAtlasGetSprite(Sheet->Sprite);
  float TexelU = (Sprite->U1 - Sprite->U0) / (float)Sprite->PixelWidth;
  float TexelV = (Sprite->V1 - Sprite->V0) / (float)Sprite->PixelHeight;
  float U0 = Sprite->U0 + ((float)(Glyph->Column * Sheet->CellWidth) * TexelU);
  float V0 = Sprite->V0 + ((float)(Glyph->Row * Sheet->CellHeight) * TexelV);
  float U1 = U0 + ((float)Glyph->PixelWidth * TexelU);
  float V1 = V0 + ((float)Sheet->CellHeight * TexelV);
  float Top = Y + (float)(FONT_BASELINE - Sheet->Ascent);
  UiTexturedRect(Ctx, UI_TEXTURE_GUI_ATLAS, X, Top,
                 (float)Glyph->PixelWidth * Sheet->Scale,
                 (float)Sheet->CellHeight * Sheet->Scale, U0, V0, U1, V1,
                 Color);
}

// Walks Text, drawing when Ctx is not null, and returns the advance.
static float RunText(UiContext *Ctx, const char *Text, float X, float Y,
                     Color8 Color) {
  FontState *State = GetFontState();
  if (Text == NULL) {
    return 0.0F;
  }
  float Pen = X;
  const unsigned char *Cursor = (const unsigned char *)Text;
  while (*Cursor != 0) {
    uint32_t Codepoint = 0;
    Cursor += DecodeUtf8(Cursor, &Codepoint);
    const FontGlyph *Glyph = FindGlyph(State, Codepoint);
    if (!IsDrawable(State, Glyph)) {
      if (Ctx != NULL) {
        DrawPlaceholder(Ctx, Pen, Y, Color);
        ReportMissing(State, Codepoint);
      }
      Pen += (float)PLACEHOLDER_ADVANCE;
      continue;
    }
    if (Ctx != NULL && Glyph->Sheet != SPACE_SHEET && Glyph->PixelWidth > 0) {
      DrawGlyph(Ctx, State, Glyph, Pen, Y, Color);
    }
    Pen += Glyph->Advance;
  }
  return Pen - X;
}

void UiText(UiContext *Ctx, const char *Text, float X, float Y, Color8 Color) {
  RunText(Ctx, Text, X, Y, Color);
}

void UiTextShadow(UiContext *Ctx, const char *Text, float X, float Y,
                  Color8 Color) {
  Color8 Shadow = {(unsigned char)(Color.R / SHADOW_DIVISOR),
                   (unsigned char)(Color.G / SHADOW_DIVISOR),
                   (unsigned char)(Color.B / SHADOW_DIVISOR), Color.A};
  RunText(Ctx, Text, X + 1.0F, Y + 1.0F, Shadow);
  RunText(Ctx, Text, X, Y, Color);
}

float UiMeasureText(const char *Text) {
  return RunText(NULL, Text, 0.0F, 0.0F, COLOR_WHITE);
}
