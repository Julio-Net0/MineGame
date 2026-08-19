# MineGame: Voxel Engine Experiment

[![CI](https://github.com/Julio-Net0/MineGame/actions/workflows/ci.yml/badge.svg)](https://github.com/Julio-Net0/MineGame/actions/workflows/ci.yml)

A minimalist Minecraft-inspired voxel engine built from scratch in **C17** using the **Raylib** library. Infinite procedural terrain, biomes, threaded chunk generation and a persistent world, written with a focus on memory management, static-analysis cleanliness and squeezing the most out of the CPU and GPU.

## ⬇️ Downloads

Every push to `master` is built for Linux and Windows automatically. Pick a permanent version, or the newest build of the day.

|  | **Latest release** | **Nightly build** |
|---|---|---|
| | [Release page](https://github.com/Julio-Net0/MineGame/releases/latest) | [Release page](https://github.com/Julio-Net0/MineGame/releases/tag/nightly) |
| 🐧 Linux | [MineGame-linux-x86_64.zip](https://github.com/Julio-Net0/MineGame/releases/latest/download/MineGame-linux-x86_64.zip) | [MineGame-linux-x86_64.zip](https://github.com/Julio-Net0/MineGame/releases/download/nightly/MineGame-linux-x86_64.zip) |
| 🪟 Windows | [MineGame-windows-x86_64.zip](https://github.com/Julio-Net0/MineGame/releases/latest/download/MineGame-windows-x86_64.zip) | [MineGame-windows-x86_64.zip](https://github.com/Julio-Net0/MineGame/releases/download/nightly/MineGame-windows-x86_64.zip) |
| | a published version, kept forever | the newest `master` commit, replaced on every push |

**Extract the archive and launch the executable from inside the extracted folder** — assets are loaded relative to the working directory. See **[Releases](docs/releases.md)** for platform requirements and the difference between the two.

## ✨ Features

**World generation.** Terrain is infinite and generated on the fly from a seed: fBm Perlin noise carves mountains, valleys and underground caves, with water filled up to sea level. A multi-noise climate field — temperature and humidity, plus a depth axis derived from the surface — selects a biome per 4×4×4 cell, so biomes are addressable in X, Y *and* Z. Every biome is a JSON file declaring its own block palette, its grass and foliage tints, and the structures and flora that grow in it.

**Structures and flora.** Trees are JSON prefabs stamped during generation, placed by a seed-deterministic hash and given a random rotation or mirror so a handful of models never look like a clone grid. Structures spanning a chunk border resolve without any cross-chunk write: each chunk independently re-derives every structure whose bounding box reaches into it and clips the stamp to its own bounds. A separate decoration pass scatters tall grass and flowers, drawn as cross-shaped alpha-cutout billboards.

**Rendering.** Chunk meshes are built with greedy meshing — adjacent faces of the same block merge into single quads, cutting vertex count by up to 80% — with per-vertex ambient occlusion for soft contact shadows, and split into opaque and translucent passes so glass and water sort correctly. Everything draws from a single texture atlas through VBOs, with view-frustum culling skipping whatever is behind the camera. Biome tint rides the vertex colour channel and is composited per texel in the shader, which lets a grass block's side fringe take the biome colour while the dirt behind it keeps its own.

**Gameplay.** A physics-driven player with AABB collision, gravity and jumping walks the world, breaks and places blocks through a DDA raycast, and picks materials from a hotbar. An in-game console (`T`) provides `/help`, `/tp`, `/pos`, `/seed`, `/save`, `/list`, `/noclip`, `/biome`, `/debug` and `/prefab` — the last of which selects a volume of blocks and exports it straight to a JSON prefab file the generator can then use. Modified chunks are serialised to disk with run-length encoding and loaded back seamlessly, so player creations survive.

**Architecture.** Simulation runs on a fixed 20 TPS tick with render interpolation, independent of framerate, while four worker threads generate and load chunks in the background and a time-sliced budget caps how many meshes are built per tick. Raylib is confined to four translation units behind platform, input, and render-backend interfaces; the world, physics, persistence and generation code uses engine-owned math types and knows nothing about the renderer — which keeps the door open for a different graphics backend and for a dedicated-server split later.

## 🤓 Tech Stack

* **Language:** C17
* **Graphics API:** [Raylib 5.5](https://www.raylib.com/) (OpenGL 3.3)
* **JSON:** [cJSON 1.7.19](https://github.com/DaveGamble/cJSON)
* **Noise:** [stb_perlin](https://github.com/nothings/stb)
* **Threading:** POSIX threads
* **Build System:** CMake (with FetchContent for zero-install dependency management)
* **Static Analysis:** Clang-Tidy, enforced as a CI gate

## 📚 Documentation

* **[Asset Formats](docs/asset-formats.md)** — the JSON the engine loads at startup: biome definitions with their flora and structure sets, climate tuning, and the block tint, side-overlay and cross-render properties.
* **[Releases](docs/releases.md)** — the nightly build versus versioned releases, what each archive contains, how to cut a release, and what a failed build means.

## 🔧 Getting Started

### Prerequisites

#### 🪟 Windows
[Scoop](https://scoop.sh/) (recommended) to install `gcc`, `cmake` and `ninja`.

#### 🐧 Linux
GCC or Clang, CMake, Ninja, and the X11/OpenGL/ALSA development headers raylib compiles against.

---

### Build & Run

Each platform has a script that configures CMake with the Ninja generator, builds, and launches the game:

#### 🪟 Windows

```powershell
./BuildAndRun.ps1
```

#### 🐧 Linux

```bash
./BuildAndRun.sh
```

Or build it manually:

#### 🪟 Windows

```powershell
cmake -S . -B build -G "Ninja" -DCMAKE_C_COMPILER=gcc
cmake --build build
./build/MineGame.exe
```

#### 🐧 Linux

```bash
cmake -S . -B build -G "Ninja" -DCMAKE_C_COMPILER=gcc
cmake --build build
./build/MineGame
```

### ❗Known Issues/Limitations
* **WSL Compatibility:** The cursor might not be disabled as expected when running this project on Windows Subsystem for Linux (WSL).

## 🤝 Credits & Acknowledgments

This project is made possible thanks to these amazing open-source libraries:

* **[raylib](https://github.com/raysan5/raylib)** - A simple and easy-to-use library to enjoy videogames programming.
    * *License:* [zlib/libpng](https://github.com/raysan5/raylib/blob/master/LICENSE)
* **[cJSON](https://github.com/DaveGamble/cJSON)** - Ultralightweight JSON parser in ANSI C.
    * *License:* [MIT](https://github.com/DaveGamble/cJSON/blob/master/LICENSE)
* **[stb_perlin](https://github.com/nothings/stb)** - A single-file C implementation of Perlin noise.
    * *License:* Public Domain / MIT (Dual-licensed)

Special thanks to **Ramon Santamaria (@raysan5)** for creating Raylib and the community for the continuous support.
