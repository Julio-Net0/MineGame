#ifndef PREFAB_CAPTURE_H
#define PREFAB_CAPTURE_H

#include "core/vecmath.h"
#include <stdbool.h>

struct World;

// Read the axis-aligned block volume bounded inclusively by CornerA and CornerB
// (world block coordinates, order-independent) and export it as a JSON prefab
// file under the prefab assets directory named "<Name>.json". The minimum corner
// maps to local origin (0,0,0); air blocks are omitted so the output stays
// sparse. When Offset is non-null it must lie inside the volume and is written
// as the prefab's local "origin" stamp anchor. Returns true on success. On
// failure returns false and writes a short reason into OutError (when OutError
// is non-null and ErrorSize > 0).
bool CaptureSelectionToPrefab(struct World *World, Vec3 CornerA, Vec3 CornerB,
                              const Vec3 *Offset, const char *Name,
                              char *OutError, int ErrorSize);

#endif
