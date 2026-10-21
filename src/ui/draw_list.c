#include "ui/draw_list.h"
#include <stdlib.h>

enum {
  INITIAL_QUAD_CAPACITY = 1024,
  INITIAL_COMMAND_CAPACITY = 32
};

void UiDrawListFree(UiDrawList *List) {
  if (List == NULL) {
    return;
  }
  free(List->Quads);
  free(List->Commands);
  *List = (UiDrawList){0};
}

void UiDrawListReset(UiDrawList *List) {
  List->QuadCount = 0;
  List->CommandCount = 0;
}

// Grows Buffer until it holds at least Count + 1 elements. A null buffer is
// treated as empty whatever Capacity says, so a freed list grows again safely.
static bool EnsureRoom(void **Buffer, int *Capacity, int Count,
                       int InitialCapacity, size_t ElementSize) {
  if (*Buffer != NULL && Count < *Capacity) {
    return true;
  }
  int NewCapacity = (*Buffer != NULL && *Capacity > 0) ? (*Capacity * 2)
                                                       : InitialCapacity;
  if (NewCapacity <= Count) {
    NewCapacity = Count + 1;
  }
  void *Grown = realloc(*Buffer, (size_t)NewCapacity * ElementSize);
  if (Grown == NULL) {
    return false;
  }
  *Buffer = Grown;
  *Capacity = NewCapacity;
  return true;
}

static bool SameScissor(UiScissor Lhs, UiScissor Rhs) {
  if (Lhs.W <= 0 && Rhs.W <= 0) {
    return true;
  }
  return Lhs.X == Rhs.X && Lhs.Y == Rhs.Y && Lhs.W == Rhs.W && Lhs.H == Rhs.H;
}

bool UiDrawListPush(UiDrawList *List, UiTextureId Texture, UiScissor Scissor,
                    const UiQuad *Quad) {
  if (List->QuadCount < 0 || List->CommandCount < 0 ||
      !EnsureRoom((void **)&List->Quads, &List->QuadCapacity, List->QuadCount,
                  INITIAL_QUAD_CAPACITY, sizeof(UiQuad))) {
    return false;
  }

  bool NeedsCommand = true;
  if (List->CommandCount > 0 && List->Commands != NULL) {
    const UiCommand *Last = &List->Commands[List->CommandCount - 1];
    NeedsCommand =
        Last->Texture != Texture || !SameScissor(Last->Scissor, Scissor);
  }
  if (NeedsCommand) {
    if (!EnsureRoom((void **)&List->Commands, &List->CommandCapacity,
                    List->CommandCount, INITIAL_COMMAND_CAPACITY,
                    sizeof(UiCommand))) {
      return false;
    }
    List->Commands[List->CommandCount] =
        (UiCommand){Texture, Scissor, List->QuadCount, 0};
    List->CommandCount++;
  }

  List->Quads[List->QuadCount++] = *Quad;
  List->Commands[List->CommandCount - 1].QuadCount++;
  return true;
}
