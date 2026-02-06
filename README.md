# MineGame: Voxel Engine Experiment

<p align="center">
  <img src="images/README/raylib.png" alt="MineGame Logo" width="600">
</p>

A minimalist Minecraft-inspired voxel engine built from scratch in **C17** using the **Raylib** library. This project focuses on learning game engine fundamentals, memory management, and clean code practices.

## 🚀 Roadmap to Beta 1
Goal: Establish a functional "Creative Mode" foundation.
- [ ] **Static World:** Render a fixed world composed of a single block type.
- [X] **Spectator Movement:** Implementation of a 6-DOF camera to fly through the world.
- [ ] **Block Placement:** Ability to add blocks to the grid in real-time.
- [ ] **Block Destruction:** Ability to remove blocks from the grid.

## 🛠️ Tech Stack

<p align="center">
  <img src="images/README/jira.png" alt="Jira" width="600">
</p>

* **Language:** C17
* **Graphics API:** [Raylib 5.5](https://www.raylib.com/)
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
