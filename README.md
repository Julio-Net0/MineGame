# MineGame: Voxel Engine Experiment

<p align="center">
  <img src="images/README/game.gif" alt="MineGame Logo" width="600">
</p>

A minimalist Minecraft-inspired voxel engine built from scratch in **C17** using the **Raylib** library. This project focuses on learning game engine fundamentals, memory management, and clean code practices.

## 🚀 Roadmap to Beta 3
Goal: Transform the optimized rendering engine into a dynamic, infinite, visually polished, and persistent voxel game world.
- [X] Data Architecture Upgrade: Replace the linear chunk array with an O(1) Hash Map or 2D Spatial Grid to eliminate CPU bottlenecks during chunk lookups and recover maximum FPS.
- [ ] Texture Atlas & UV Mapping: Transition from solid vertex colors to actual block graphics by mapping a single global texture image onto the VBOs.
- [ ] Procedural Terrain Generation: Integrate Perlin or Simplex noise algorithms to generate organic landscapes, mountains, valleys, and underground caves instead of flat planes.
- [ ] Multithreaded Chunk Processing: Implement background Worker Threads to handle chunk generation and meshing in parallel, eliminating all gameplay stutters when crossing chunk borders.
- [ ] World Persistence (Save/Load): Develop a binary serialization system with RLE (Run-Length Encoding) compression to save modified chunks to disk and load them seamlessly, preserving player creations.
- [X] View Frustum Culling: Add camera-aware mathematics (Dot Product / Plane Extraction) to completely ignore and skip rendering chunks that are behind the player or outside the field of view, maximizing GPU efficiency.
- [ ] Meshing Amortization(Chunk Queue): Implement a time-sliced building queue that limits the number of meshes generated per frame. This eliminates CPU lag spikes and guarantees buttery-smooth framerates when crossing chunk boundaries at high speeds.
- [ ] Greedy Meshing: Upgrade the Face Culling algorithm to merge adjacent block faces of the same type into massive, single quads. This slashes the total vertex count by up to 80%, drastically reducing VRAM usage and GPU memory bandwidth.
- [ ] Voxel Ambient Occlusion (AO): Calculate smooth, localized shadows on block vertices based on neighboring geometry to add visual depth and that classic "voxel lighting" feel to the world.
- [ ] Translucency & Render Passes: Split chunk meshes into "Opaque" and "Transparent" passes to correctly render glass, water, and foliage without OpenGL depth-sorting bugs.

## Beta 2 ✅
Goal: Transform the engine from a free-roaming spectator into a physically-grounded game world.
- [X] **Debug Console & Command System:** Implement an in-game text terminal to execute logic functions, list loaded assets from cJSON, toggle debug overlays, etc.
- [X] **Physical Embodiment:** Replace the flying camera with a physics-aware Player entity using AABB (Axis-Aligned Bounding Box) collision detection.
- [X] **Dynamic World Management:** Implement a World Handler to manage, render, and "stitch" together multiple chunks (Mesh-culling across chunk borders).
- [X] **Player Mechanics:** Implementation of gravity, jumping, and ground-level movement (step-up/step-down logic).
- [X] **Gameplay Loop:** Basic inventory system with a HUD Hotbar and block selection (1-9 keys) to choose materials for building.
- [X] **Performance Pass:** Transition from Immediate Mode rendering to Vertex Buffer Objects (VBOs) to support larger view distances.

## Beta 1 ✅
Goal: Establish a functional "Creative Mode" foundation.
- [X] **Static World:** Render a fixed world.
- [X] **Spectator Movement:** Implementation of a 6-DOF camera to fly through the world.
- [X] **Block Placement:** Ability to add blocks to the grid in real-time.
- [X] **Block Destruction:** Ability to remove blocks from the grid.

## 🤓 Tech Stack

<p align="center">
  <img src="images/README/jira.png" alt="Jira" width="600">
</p>

* **Language:** C17
* **Graphics API:** [Raylib 5.5](https://www.raylib.com/)
* **cJSON:** [cJSON 1.7.19](https://github.com/DaveGamble/cJSON)
* **Build System:** CMake (with FetchContent for zero-install dependency management).
* **Static Analysis:** Clang-Tidy.
* **Project Management:** Jira (Agile/Kanban).

## 🔧 Getting Started

### Prerequisites

### 🐧 Linux
  - GCC or Clang 
  - CMake 
  - X11/OpenGL development headers.

#### 🪟 Windows (Native PowerShell)
  - [Scoop](https://scoop.sh/) (Recommended) to install: `gcc`, `cmake`, `ninja`.

---

### Build & Run

#### 🪟 Windows (Native PowerShell)
The easiest way is using the provided automation script which handles the Ninja generator and GCC paths:

```powershell
./BuildAndRun.ps1
```

Or you can build it manually

```powershell
cmake -S . -B build -G "Ninja" -DCMAKE_C_COMPILER=gcc
cmake --build build
./build/MineGame.exe
```

### 🐧 Linux

```powershell
mkdir build && cd build
cmake ..
make
./MineGame
```

### ❗Known Issues/Limitations
* **WSL Compatibility:** The cursor might not be disabled as expected when running this project on Windows Subsystem for Linux (WSL).

## 🤝 Credits & Acknowledgments

This project is made possible thanks to these amazing open-source libraries:

* **[raylib](https://github.com/raysan5/raylib)** - A simple and easy-to-use library to enjoy videogames programming.
    * *License:* [zlib/libpng](https://github.com/raysan5/raylib/blob/master/LICENSE)
* **[cJSON](https://github.com/DaveGamble/cJSON)** - Ultralightweight JSON parser in ANSI C.
    * *License:* [MIT](https://github.com/DaveGamble/cJSON/blob/master/LICENSE)

Special thanks to **Ramon Santamaria (@raysan5)** for creating Raylib and the community for the continuous support.
