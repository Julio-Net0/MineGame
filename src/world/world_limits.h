#ifndef WORLD_LIMITS_H
#define WORLD_LIMITS_H

// The active set is a horizontal disc of columns (dx^2 + dz^2 <= R^2). Every
// column loads its terrain band, the layers its terrain bounds say can hold
// anything visible; the columns within NEAR_LOAD_RADIUS also load a vertical
// window of at most 2 * VERTICAL_RENDER_RADIUS + 1 layers around the load
// centre, for caves and the player's surroundings. With MAX_RENDER_DISTANCE at
// 128, terrain bands stay around ~7 layers/column (~375k chunks), so the slot
// pool is sized for banded loading + near window headroom instead of a full
// 17-layer cylinder, and TrimToSlotBudget enforces the cap on extreme relief.
#define MIN_RENDER_DISTANCE 2
#define DEFAULT_RENDER_DISTANCE 8
#define MAX_RENDER_DISTANCE 128
#define VERTICAL_RENDER_RADIUS 8
// Farthest an active layer can lie from the load centre's layer. The window
// holds 2 * VERTICAL_RENDER_RADIUS + 1 layers and always contains the layer
// above the centre, so at most 2 * VERTICAL_RENDER_RADIUS - 1 lie below it.
#define MAX_LOAD_LAYER_REACH ((2 * VERTICAL_RENDER_RADIUS) - 1)
// Columns, horizontally, around the load centre that carry the window.
#define NEAR_LOAD_RADIUS 16
// Farthest a loaded layer lies from the load centre's layer while the centre
// is inside the default world (74 layers), and the most layers one band spans.
#define MAX_VERTICAL_LOAD_REACH 80

// Lattice points with dx^2 + dz^2 <= MAX_RENDER_DISTANCE^2. A literal because
// C17 cannot count them at compile time; the column table builder never
// exceeds it.
#define MAX_LOADED_COLUMNS 51433
#define MAX_ACTIVE_CHUNKS 524288

#endif
