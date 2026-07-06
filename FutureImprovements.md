# MineGame: Future Improvements & Optimizations

This document serves as a repository for technical design ideas, optimizations, and structural improvements that can be implemented in the next phases of the project.

---

## 🎨 1. Graphics & Rendering

### 1.1 Alpha Cutout for Leaves (Prevent Overdraw)
* **Context:** Currently, leaf blocks (`Leaves`) are marked as `"isTransparent": true` and are rendered in the translucent pass without depth writes (`rlDisableDepthMask`). In dense forests, this causes high overdraw (the GPU processes fragment shading for multiple layers of hidden leaf faces).
* **Solution:**
  - Move leaves to the opaque render pass.
  - Implement a simple GLSL fragment shader that discards fragments where the texture pixel's alpha value is below a certain threshold (e.g., `if (texColor.a < 0.5) discard;`).
  - This allows the depth buffer writes to remain active for leaves, letting the GPU automatically cull hidden leaves at the hardware level using *Early-Z* testing.

### 1.2 Dynamic Inner-Chunk Face Sorting
* **Context:** Sorting of translucent chunks (Back-to-Front) has been implemented in [renderer.c](file:///c:/Users/jjuli/OneDrive/Documentos/Codes/C/MineGame/src/render/renderer.c), but the face rendering order within a single chunk VBO remains static (generated along X ──▶ Y ──▶ Z).
* **Solution:**
  - When the camera angle changes significantly (e.g., rotation > 45 degrees), re-sort the indices of translucent faces on the CPU based on the camera view vector.
  - Update the index buffer on the GPU using `rlUpdateMesh`.
  - Since updating only the index buffer is computationally cheap, this resolves translucent blending artifacts within the same chunk without significant CPU/GPU overhead.

### 1.3 Mipmapping & Anisotropic Filtering
* **Context:** When looking at pixel art textures from a distance, thin lines and high-contrast pixel changes cause noisy flickering (aliasing artifacts).
* **Solution:**
  - Enable Mipmap generation on the texture atlas.
  - Configure trilinear or anisotropic filtering to smooth out textures rendered at a distance.

---

## 🏃 2. Physics & Gameplay

### 2.1 Fluid Physics & Dynamics (Water)
* **Context:** Water is now non-solid (defined in [water.json](file:///c:/Users/jjuli/OneDrive/Documentos/Codes/C/MineGame/assets/blocks/water.json)), but the player falls through it with normal atmospheric gravity.
* **Solution:**
  - In the physics update loop in [player.c](file:///c:/Users/jjuli/OneDrive/Documentos/Codes/C/MineGame/src/player/player.c), detect if the player's head or feet points are inside a water block.
  - Apply reduced gravity and drag forces (velocity dampening) on both horizontal and vertical axes.
  - Enable a "swimming" state that moves the player upwards when holding the jump key (`Space`).

### 2.2 Water Surface Height
* **Context:** Water blocks currently occupy a full `1.0 x 1.0 x 1.0` volume, creating visual intersection artifacts with neighboring dry blocks.
* **Solution:**
  - Modify the chunk mesh generation in [renderer.c](file:///c:/Users/jjuli/OneDrive/Documentos/Codes/C/MineGame/src/render/renderer.c) to lower the top face (`FACE_TOP`) of water blocks slightly (e.g., to 0.9 or 0.85).
  - This creates the classic look where the water surface sits lower than the surrounding shoreline.

---

## 💻 3. Architecture & Structure

### 3.1 Decoupling Simulation Logic (Multiplayer Readiness)
* **Context:** Following the instructions in [CLAUDE.md](file:///c:/Users/jjuli/OneDrive/Documentos/Codes/C/MineGame/CLAUDE.md), game logic should be client/server friendly.
* **Solution:**
  - Completely isolate the logical `World` data structures from any Raylib graphics functions or headers.
  - Move physics collision and state updates to a fixed-tick loop (e.g., 20 ticks per second), letting the renderer run independently at maximum frame rates, interpolating positions between logic ticks.
