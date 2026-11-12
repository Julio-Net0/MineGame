#ifndef WORLD_LIMITS_H
#define WORLD_LIMITS_H

// The active set is a horizontal disc of columns (dx^2 + dz^2 <= R^2) times a
// vertical window of at most 2 * VERTICAL_RENDER_RADIUS + 1 layers around the
// load centre, clamped to the world's generated range. Capacity therefore grows
// with R^2 rather than R^3.
#define MIN_RENDER_DISTANCE 2
#define DEFAULT_RENDER_DISTANCE 8
#define MAX_RENDER_DISTANCE 32
#define VERTICAL_RENDER_RADIUS 8
// Farthest an active layer can lie from the load centre's layer. The window
// holds 2 * VERTICAL_RENDER_RADIUS + 1 layers and always contains the layer
// above the centre, so at most 2 * VERTICAL_RENDER_RADIUS - 1 lie below it.
#define MAX_LOAD_LAYER_REACH ((2 * VERTICAL_RENDER_RADIUS) - 1)

// Lattice points with dx^2 + dz^2 <= MAX_RENDER_DISTANCE^2. A literal because
// C17 cannot count them at compile time; the column table builder never
// exceeds it.
#define MAX_LOADED_COLUMNS 3209
#define MAX_ACTIVE_CHUNKS \
  (MAX_LOADED_COLUMNS * ((2 * VERTICAL_RENDER_RADIUS) + 1))

#endif
