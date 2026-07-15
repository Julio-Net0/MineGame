#ifndef CORE_NOISE_H
#define CORE_NOISE_H

// Renderer-free noise facility owned by the engine.
//
// The vendored stb_perlin implementation is instantiated inside noise.c with
// internal linkage, so it cannot collide with the copy Raylib exports from its
// own image-generation code. That is what lets the simulation layer sample noise
// without linking the renderer: world/chunk.c and world/biome.c call through
// here and never include a noise implementation header themselves.
//
// Adding a noise variant means extending this header, not including
// third_party/stb_perlin.h elsewhere — the vendored symbols are not visible
// outside noise.c by design.

// Fractal Brownian motion over 3D Perlin noise.
//
// This mirrors the vendored fBm contract exactly and returns its value
// unmodified: octaves are summed WITHOUT normalization, so the output range
// grows with Octaves and Gain rather than staying in [-1, 1]. Callers are
// expected to reshape it themselves (terrain scales it by a height variance;
// biome sampling divides by the amplitude sum). Normalizing here would silently
// change every world that already exists.
float NoiseFbm3(float X, float Y, float Z, float Lacunarity, float Gain,
                int Octaves);

#endif
