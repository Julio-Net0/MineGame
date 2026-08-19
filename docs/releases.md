# Releases

Every push to `master` is built for Linux and Windows by GitHub Actions and published automatically. There are two kinds of download, and the difference matters.

## Nightly build vs. versioned releases

| | **Nightly build** | **`v*` releases** |
|---|---|---|
| Tag | `nightly`, always the same | `v0.3-beta`, `v0.4-beta`, … |
| Contents | the latest `master` commit | a version the maintainer chose to publish |
| Replaced? | yes, on every push to `master` | never |
| Marked | pre-release | normal release |
| Use it when | you want the newest work and can live with rough edges | you want something stable to come back to |

The nightly release is deleted and recreated on each successful build, always under the same tag. That keeps its download links identical forever, so they can be bookmarked or linked from anywhere:

- `https://github.com/Julio-Net0/MineGame/releases/download/nightly/MineGame-linux-x86_64.zip`
- `https://github.com/Julio-Net0/MineGame/releases/download/nightly/MineGame-windows-x86_64.zip`

It is flagged as a pre-release on purpose: GitHub then keeps its **Latest release** badge pointing at the newest `v*` tag, so a first-time visitor lands on a curated build rather than on whatever was committed an hour ago.

Recreating rather than updating in place also means an asset whose name changed does not linger: whatever is attached to `nightly` is exactly what the last successful build produced.

## What is in an archive, and how to run it

Each archive holds a single top-level folder containing the executable and its `assets/` directory:

```
MineGame-linux-x86_64/
├── MineGame          (or MineGame.exe on Windows)
└── assets/
    ├── atlas/
    ├── biomes/
    ├── blocks/
    ├── prefabs/
    ├── shaders/
    └── biome_params.json
```

**Launch the executable from inside the extracted folder.** The engine loads its assets through paths relative to the working directory, so starting it from somewhere else leaves it unable to find its blocks, biomes, prefabs and shaders.

### Windows

Self-contained. The binary is statically linked against the MinGW runtime, so it needs no compiler DLLs and no redistributable. The build fails deliberately if it ever picks up a dependency on `libgcc`, `libwinpthread` or `libstdc++`, since those would be missing on a player's machine.

### Linux

The archive carries raylib inside the binary, but graphics and audio come from the system, as they do for any native Linux game. You need:

- **glibc 2.39 or newer** — Ubuntu 24.04+, Debian 13+, Fedora 40+, or a rolling distribution. This floor comes from the pinned `ubuntu-24.04` machine the release is built on.
- **X11 and OpenGL runtime libraries**, which any desktop install already has.

If the binary refuses to start with a message mentioning `GLIBC_2.39`, your distribution is older than the build machine. Building from source is the way around it.

## Cutting a release

Publishing a version is a single push. There is no build to run by hand and no file to upload.

1. Make sure `master` is green — the nightly release for the commit you are about to tag must exist.
2. Tag the commit, following the naming already in the history (`v0.1-beta`, `v0.2-beta`, `v0.3-beta`):

   ```bash
   git tag v0.4-beta
   git push origin v0.4-beta
   ```

3. The pipeline rebuilds both platforms from that exact commit, runs the Clang-Tidy gate again, and creates the release.

The release notes are assembled automatically: download and platform instructions on top, followed by the list of commits since the previous release.

### Verifying afterwards

- The release appears without the **Pre-release** badge, with both `.zip` files attached.
- **Latest release** now points at the new tag.
- The `Nightly build` release is untouched, with the same assets and the same URLs it had before.

### Re-running a tag

Deliberately impossible. Publishing a tag that already has a release fails instead of overwriting it, because people may already have downloaded the old assets. To correct a broken release, delete the release and its tag from the GitHub interface first, then push the tag again.

## When a build fails

Nothing is published unless all three build jobs succeed, so **a failed run leaves the existing releases exactly as they were**. There is no half-published state to clean up — fix the problem and push again.

| Failing job | What it usually means |
|---|---|
| **Build (Linux)** | Code that only compiles on Windows, or a missing system development package for raylib. The dependency list lives in the workflow's `Install build dependencies` step. |
| **Build (Windows)** | A MinGW toolchain problem, or the `Verify no toolchain DLL dependencies` step catching a binary that would need a DLL players do not have. |
| **Clang-Tidy** | A static-analysis diagnostic in the project's own sources. The log prints only the real diagnostics. `NOLINT` suppressions are not allowed — the finding has to be fixed. |
| **Publish nightly / Publish tagged release** | Almost always a permissions or tag-state problem rather than a code problem. |

A common surprise: the Clang-Tidy gate analyses the POSIX branches of platform-specific code (`#else` blocks behind `#ifdef _WIN32`) that a Windows compiler never sees. Code can be red in CI while building cleanly on a Windows workstation.

### Removing a bad release

Delete the release and its tag from the GitHub Releases page. Deleting the `nightly` release needs no follow-up — the next push to `master` recreates it.

## How it is wired

A single workflow, [`.github/workflows/ci.yml`](../.github/workflows/ci.yml):

```
push to master ─┬─► Build (Linux)   ─┐
                ├─► Build (Windows) ─┼─► Publish nightly        (pre-release, tag "nightly")
                └─► Clang-Tidy      ─┘

push v* tag ────┬─► Build (Linux)   ─┐
                ├─► Build (Windows) ─┼─► Publish tagged release (permanent)
                └─► Clang-Tidy      ─┘
```

Details worth knowing:

- **Commits that only touch documentation do not trigger a build.** Tag pushes ignore that filter, so tagging a documentation commit still produces a release.
- **Only the publishing jobs can write to the repository.** The build and lint jobs run read-only.
- **A newer push cancels the run in flight** for the same branch, so two quick pushes do not race to replace the nightly release.
- **The runner images are pinned**, not floating. Raising them is a deliberate edit, because the Linux image decides the glibc floor of every released binary.
