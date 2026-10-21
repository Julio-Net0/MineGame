#include "ui/gui_atlas.h"
#include "core/fileio.h"
#include "core/log.h"
#include "core/resource_pack.h"
#include "third_party/cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  MAX_GUI_SPRITES = 256,
  MAX_SPRITE_NAME = 96,
  ATLAS_START_DIM = 1024,
  ATLAS_MAX_DIM = 2048,
  ATLAS_PADDING = 1,
  CHECKER_SIZE = 16,
  CHECKER_CELL = 8,
  WHITE_SIZE = 1,
  // The white texel and the checker are packed ahead of every sprite, so they
  // always fit and overflow can fall back to the checker.
  RESERVED_ITEMS = 2,
  MAX_PACK_ITEMS = MAX_GUI_SPRITES + RESERVED_ITEMS,
  OPAQUE = 255
};

// Solid fills sample the middle of the white texel, so nearest filtering can
// never pick up a neighbouring entry.
#define TEXEL_CENTER 0.5F

typedef struct {
  char Name[MAX_SPRITE_NAME];
  GuiSprite Sprite;
  // A whole texture addressed by path rather than a gui/sprites/ name.
  bool IsTexture;
} SpriteEntry;

typedef struct {
  CoreImage Image;
  int X;
  int Y;
  bool Placed;
} PackItem;

typedef struct {
  SpriteEntry Entries[MAX_GUI_SPRITES];
  int Count;
  bool Built;
  GuiSprite Checker;
  float WhiteU;
  float WhiteV;
} GuiAtlasState;

static GuiAtlasState *GetGuiAtlasState(void) {
  static GuiAtlasState State;
  return &State;
}

static UiSpriteId RegisterEntry(const char *Name, bool IsTexture) {
  GuiAtlasState *State = GetGuiAtlasState();
  if (Name == NULL || Name[0] == '\0') {
    return UI_SPRITE_INVALID;
  }
  for (int Idx = 0; Idx < State->Count; Idx++) {
    if (State->Entries[Idx].IsTexture == IsTexture &&
        strcmp(State->Entries[Idx].Name, Name) == 0) {
      return Idx;
    }
  }
  if (State->Built) {
    LogError("GUI: '%s' registered after the atlas was built", Name);
    return UI_SPRITE_INVALID;
  }
  if (State->Count >= MAX_GUI_SPRITES) {
    LogError("GUI: sprite registry full, '%s' not registered", Name);
    return UI_SPRITE_INVALID;
  }
  SpriteEntry *Entry = &State->Entries[State->Count];
  snprintf(Entry->Name, sizeof(Entry->Name), "%s", Name);
  Entry->IsTexture = IsTexture;
  return State->Count++;
}

UiSpriteId GuiAtlasRegister(const char *Name) {
  return RegisterEntry(Name, false);
}

UiSpriteId GuiAtlasRegisterTexture(const char *TexturePath) {
  return RegisterEntry(TexturePath, true);
}

const GuiSprite *GuiAtlasGetSprite(UiSpriteId Id) {
  GuiAtlasState *State = GetGuiAtlasState();
  if (Id < 0 || Id >= State->Count) {
    return &State->Checker;
  }
  return &State->Entries[Id].Sprite;
}

void GuiAtlasGetWhiteUV(float *OutU, float *OutV) {
  GuiAtlasState *State = GetGuiAtlasState();
  *OutU = State->WhiteU;
  *OutV = State->WhiteV;
}

void GuiAtlasShutdown(void) {
  GuiAtlasState *State = GetGuiAtlasState();
  State->Count = 0;
  State->Built = false;
}

// ---------------------------------------------------------------------------
// Sprite loading
// ---------------------------------------------------------------------------

static bool AllocImage(CoreImage *Out, int Width, int Height) {
  size_t Bytes = (size_t)Width * (size_t)Height * (size_t)CORE_IMAGE_CHANNELS;
  Out->Pixels = (unsigned char *)calloc(Bytes, 1U);
  if (Out->Pixels == NULL) {
    Out->Width = 0;
    Out->Height = 0;
    return false;
  }
  Out->Width = Width;
  Out->Height = Height;
  return true;
}

static bool MakeSolidImage(CoreImage *Out, int Size, bool Checker) {
  if (!AllocImage(Out, Size, Size)) {
    return false;
  }
  for (int Y = 0; Y < Size; Y++) {
    for (int X = 0; X < Size; X++) {
      size_t Offset = (((size_t)Y * (size_t)Size) + (size_t)X) *
                      (size_t)CORE_IMAGE_CHANNELS;
      unsigned char *Px = &Out->Pixels[Offset];
      bool Magenta = !Checker || (((X / CHECKER_CELL) + (Y / CHECKER_CELL)) % 2 == 0);
      Px[0] = Magenta ? OPAQUE : 0;
      Px[1] = Checker ? 0 : OPAQUE;
      Px[2] = Magenta ? OPAQUE : 0;
      Px[3] = OPAQUE;
    }
  }
  return true;
}

// Keeps the top-left Width x Height region, replacing Image's pixels.
static bool CropImage(CoreImage *Image, int Width, int Height) {
  if (Width >= Image->Width && Height >= Image->Height) {
    return true;
  }
  CoreImage Cropped;
  if (!AllocImage(&Cropped, Width, Height)) {
    return false;
  }
  size_t RowBytes = (size_t)Width * (size_t)CORE_IMAGE_CHANNELS;
  for (int Row = 0; Row < Height; Row++) {
    memcpy(&Cropped.Pixels[(size_t)Row * RowBytes],
           &Image->Pixels[(size_t)Row * (size_t)Image->Width *
                          (size_t)CORE_IMAGE_CHANNELS],
           RowBytes);
  }
  CoreImageFree(Image);
  *Image = Cropped;
  return true;
}

static int ReadIntField(const cJSON *Object, const char *Key, int Default) {
  const cJSON *Item = cJSON_GetObjectItemCaseSensitive(Object, Key);
  if (cJSON_IsNumber(Item) == 0) {
    return Default;
  }
  return (int)cJSON_GetNumberValue(Item);
}

static void ParseNineSlice(const cJSON *Scaling, GuiSprite *Sprite) {
  Sprite->Scaling = GUI_SCALING_NINE_SLICE;
  Sprite->LogicalWidth = ReadIntField(Scaling, "width", Sprite->LogicalWidth);
  Sprite->LogicalHeight =
      ReadIntField(Scaling, "height", Sprite->LogicalHeight);

  const cJSON *Border = cJSON_GetObjectItemCaseSensitive(Scaling, "border");
  if (cJSON_IsNumber(Border) != 0) {
    int All = (int)cJSON_GetNumberValue(Border);
    Sprite->BorderLeft = All;
    Sprite->BorderTop = All;
    Sprite->BorderRight = All;
    Sprite->BorderBottom = All;
  } else if (cJSON_IsObject(Border) != 0) {
    Sprite->BorderLeft = ReadIntField(Border, "left", 0);
    Sprite->BorderTop = ReadIntField(Border, "top", 0);
    Sprite->BorderRight = ReadIntField(Border, "right", 0);
    Sprite->BorderBottom = ReadIntField(Border, "bottom", 0);
  }

  const cJSON *Stretch =
      cJSON_GetObjectItemCaseSensitive(Scaling, "stretch_inner");
  Sprite->StretchInner = (cJSON_IsTrue(Stretch) != 0);
}

// Applies the .mcmeta that came from the same source as the image: the
// animation crop first (it changes the pixel size the logical size defaults
// to), then the GUI scaling.
static void ApplyMetadata(const char *Name, int Source, CoreImage *Image,
                          GuiSprite *Sprite) {
  char MetaPath[FILEIO_MAX_PATH];
  snprintf(MetaPath, sizeof(MetaPath), "gui/sprites/%s.png.mcmeta", Name);

  int Size = 0;
  unsigned char *Text =
      ResourceReadFrom(RESOURCE_TEXTURE, MetaPath, Source, &Size);
  if (Text == NULL) {
    return;
  }
  cJSON *Json = cJSON_ParseWithLength((const char *)Text, (size_t)Size);
  free(Text);
  if (Json == NULL) {
    LogWarn("GUI: malformed metadata for sprite '%s', ignoring it", Name);
    return;
  }

  const cJSON *Animation = cJSON_GetObjectItemCaseSensitive(Json, "animation");
  if (cJSON_IsObject(Animation) != 0) {
    int Side = (Image->Width < Image->Height) ? Image->Width : Image->Height;
    int FrameWidth = ReadIntField(Animation, "width", Side);
    int FrameHeight = ReadIntField(Animation, "height", Side);
    if (FrameWidth > 0 && FrameHeight > 0 && FrameWidth <= Image->Width &&
        FrameHeight <= Image->Height) {
      CropImage(Image, FrameWidth, FrameHeight);
    }
  }
  Sprite->LogicalWidth = Image->Width;
  Sprite->LogicalHeight = Image->Height;

  const cJSON *Gui = cJSON_GetObjectItemCaseSensitive(Json, "gui");
  const cJSON *Scaling = cJSON_GetObjectItemCaseSensitive(Gui, "scaling");
  const cJSON *Type = cJSON_GetObjectItemCaseSensitive(Scaling, "type");
  if (cJSON_IsString(Type) != 0) {
    if (strcmp(Type->valuestring, "nine_slice") == 0) {
      ParseNineSlice(Scaling, Sprite);
    } else if (strcmp(Type->valuestring, "stretch") != 0) {
      LogWarn("GUI: sprite '%s' uses unsupported scaling '%s', stretching it",
              Name, Type->valuestring);
    }
  }
  cJSON_Delete(Json);
}

static bool LoadSprite(const SpriteEntry *Entry, CoreImage *OutImage,
                       GuiSprite *Sprite) {
  char Path[FILEIO_MAX_PATH];
  if (Entry->IsTexture) {
    snprintf(Path, sizeof(Path), "%s", Entry->Name);
  } else {
    snprintf(Path, sizeof(Path), "gui/sprites/%s.png", Entry->Name);
  }

  int Size = 0;
  int Source = RESOURCE_SOURCE_NONE;
  unsigned char *Bytes = ResourceRead(RESOURCE_TEXTURE, Path, &Size, &Source);
  if (Bytes == NULL) {
    return false;
  }
  bool Decoded = CoreImageDecodePng(Bytes, Size, OutImage);
  free(Bytes);
  if (!Decoded) {
    return false;
  }

  *Sprite = (GuiSprite){0};
  Sprite->Scaling = GUI_SCALING_NONE;
  Sprite->LogicalWidth = OutImage->Width;
  Sprite->LogicalHeight = OutImage->Height;
  if (!Entry->IsTexture) {
    ApplyMetadata(Entry->Name, Source, OutImage, Sprite);
  }
  return true;
}

// ---------------------------------------------------------------------------
// Packing
// ---------------------------------------------------------------------------

static int CompareItemHeight(const void *Lhs, const void *Rhs) {
  const PackItem *ItemA = *(const PackItem *const *)Lhs;
  const PackItem *ItemB = *(const PackItem *const *)Rhs;
  return ItemB->Image.Height - ItemA->Image.Height;
}

// Shelf packing: rows filled left to right, a new row when one is full. Items
// arrive tallest first, so each row's height is set by its first item and the
// wasted space stays small. Returns true when every item was placed.
static bool PackShelves(PackItem **Order, int Count, int Dim) {
  int CursorX = ATLAS_PADDING;
  int CursorY = ATLAS_PADDING;
  int ShelfHeight = 0;
  bool AllPlaced = true;

  for (int Idx = 0; Idx < Count; Idx++) {
    PackItem *Item = Order[Idx];
    int Width = Item->Image.Width;
    int Height = Item->Image.Height;
    Item->Placed = false;

    if (CursorX + Width + ATLAS_PADDING > Dim) {
      CursorY += ShelfHeight + ATLAS_PADDING;
      CursorX = ATLAS_PADDING;
      ShelfHeight = 0;
    }
    if (Width + (2 * ATLAS_PADDING) > Dim ||
        CursorY + Height + ATLAS_PADDING > Dim) {
      AllPlaced = false;
      continue;
    }

    Item->X = CursorX;
    Item->Y = CursorY;
    Item->Placed = true;
    CursorX += Width + ATLAS_PADDING;
    if (Height > ShelfHeight) {
      ShelfHeight = Height;
    }
  }
  return AllPlaced;
}

static void BlitItem(CoreImage *Atlas, const PackItem *Item) {
  size_t RowBytes = (size_t)Item->Image.Width * (size_t)CORE_IMAGE_CHANNELS;
  for (int Row = 0; Row < Item->Image.Height; Row++) {
    size_t Dest = ((((size_t)Item->Y + (size_t)Row) * (size_t)Atlas->Width) +
                   (size_t)Item->X) *
                  (size_t)CORE_IMAGE_CHANNELS;
    memcpy(&Atlas->Pixels[Dest], &Item->Image.Pixels[(size_t)Row * RowBytes],
           RowBytes);
  }
}

static void AssignUV(GuiSprite *Sprite, const PackItem *Item, int Dim) {
  float Scale = 1.0F / (float)Dim;
  Sprite->U0 = (float)Item->X * Scale;
  Sprite->V0 = (float)Item->Y * Scale;
  Sprite->U1 = (float)(Item->X + Item->Image.Width) * Scale;
  Sprite->V1 = (float)(Item->Y + Item->Image.Height) * Scale;
  Sprite->AtlasX = Item->X;
  Sprite->AtlasY = Item->Y;
  Sprite->PixelWidth = Item->Image.Width;
  Sprite->PixelHeight = Item->Image.Height;
  Sprite->Resolved = true;
}

static void ResetToChecker(GuiSprite *Sprite, const GuiSprite *Checker) {
  *Sprite = *Checker;
}

bool GuiAtlasBuild(CoreImage *OutAtlas) {
  GuiAtlasState *State = GetGuiAtlasState();
  *OutAtlas = (CoreImage){0};

  static PackItem Items[MAX_PACK_ITEMS];
  static PackItem *Order[MAX_PACK_ITEMS];
  PackItem *White = &Items[0];
  PackItem *CheckerItem = &Items[1];
  *White = (PackItem){0};
  *CheckerItem = (PackItem){0};
  if (!MakeSolidImage(&White->Image, WHITE_SIZE, false) ||
      !MakeSolidImage(&CheckerItem->Image, CHECKER_SIZE, true)) {
    CoreImageFree(&White->Image);
    CoreImageFree(&CheckerItem->Image);
    LogError("GUI: out of memory building the atlas");
    return false;
  }

  bool Missing[MAX_GUI_SPRITES] = {false};
  int ItemCount = RESERVED_ITEMS;
  for (int Idx = 0; Idx < State->Count; Idx++) {
    PackItem *Item = &Items[ItemCount++];
    *Item = (PackItem){0};
    SpriteEntry *Entry = &State->Entries[Idx];
    if (!LoadSprite(Entry, &Item->Image, &Entry->Sprite)) {
      LogWarn("GUI: sprite '%s' not found, drawing a checker", Entry->Name);
      Missing[Idx] = true;
    }
  }

  // Reserved items lead regardless of height so they are never the ones that
  // overflow; sprites follow tallest first.
  int OrderCount = 0;
  Order[OrderCount++] = White;
  Order[OrderCount++] = CheckerItem;
  int FirstSprite = OrderCount;
  for (int Idx = RESERVED_ITEMS; Idx < ItemCount; Idx++) {
    if (Items[Idx].Image.Pixels != NULL) {
      Order[OrderCount++] = &Items[Idx];
    }
  }
  qsort((void *)&Order[FirstSprite], (size_t)(OrderCount - FirstSprite),
        sizeof(PackItem *), CompareItemHeight);

  int Dim = ATLAS_START_DIM;
  if (!PackShelves(Order, OrderCount, Dim)) {
    Dim = ATLAS_MAX_DIM;
    if (!PackShelves(Order, OrderCount, Dim)) {
      LogError("GUI: atlas overflow at %dx%d, some sprites draw as a checker",
               Dim, Dim);
    }
  }

  bool Ok = AllocImage(OutAtlas, Dim, Dim);
  if (Ok) {
    for (int Idx = 0; Idx < OrderCount; Idx++) {
      if (Order[Idx]->Placed) {
        BlitItem(OutAtlas, Order[Idx]);
      }
    }

    State->Checker = (GuiSprite){0};
    State->Checker.Scaling = GUI_SCALING_NONE;
    State->Checker.LogicalWidth = CHECKER_SIZE;
    State->Checker.LogicalHeight = CHECKER_SIZE;
    AssignUV(&State->Checker, CheckerItem, Dim);
    State->Checker.Resolved = false;
    State->WhiteU = ((float)White->X + TEXEL_CENTER) / (float)Dim;
    State->WhiteV = ((float)White->Y + TEXEL_CENTER) / (float)Dim;

    for (int Idx = 0; Idx < State->Count; Idx++) {
      const PackItem *Item = &Items[RESERVED_ITEMS + Idx];
      GuiSprite *Sprite = &State->Entries[Idx].Sprite;
      if (Missing[Idx] || !Item->Placed) {
        ResetToChecker(Sprite, &State->Checker);
      } else {
        AssignUV(Sprite, Item, Dim);
      }
    }
  } else {
    LogError("GUI: out of memory allocating the %dx%d atlas", Dim, Dim);
  }

  for (int Idx = 0; Idx < ItemCount; Idx++) {
    CoreImageFree(&Items[Idx].Image);
  }
  State->Built = true;
  LogInfo("GUI: packed %d sprites into a %dx%d atlas", State->Count, Dim, Dim);
  return Ok;
}
