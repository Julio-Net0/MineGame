#!/usr/bin/env python3
"""Automated end-to-end regression suite runner for MineGame."""

from __future__ import annotations

import argparse
import json
import math
import re
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time
import zlib
from typing import Any, Callable

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
EXPECTED_WIDTH = 1280
EXPECTED_HEIGHT = 720
EXPECTED_SEED = "16412814131703521744"
# Number of "PROTOCOL_TEST: ok" lines MineGameServer --test-protocol logs; a
# mismatch means a check was silently skipped or added without updating this.
EXPECTED_PROTOCOL_CHECKS = 38
# Buffers per dense pool slab; keep in sync with src/world/dense_pool.h.
DENSE_SLAB_CAPACITY = 512

# Mirror of the engine's chunk loader, used to derive expected active chunk
# counts. Keep in sync with src/world/world_limits.h (render distance limits,
# VERTICAL_RENDER_RADIUS) and ComputeLayerWindow / BuildLoadVolume in
# src/world/world.c. WORLD_LO/HI_CHUNK_Y are the chunk layers holding the
# bedrock band's lowest Y and the generation ceiling for the default strata;
# SEA_CHUNK_Y is the chunk layer holding SEA_LEVEL.
CHUNK_SIZE = 16
MIN_RENDER_DISTANCE = 2
DEFAULT_RENDER_DISTANCE = 8
MAX_RENDER_DISTANCE = 32
VERTICAL_RENDER_RADIUS = 8
DEPTH_SHIFT_PER_LAYER = 1
MIN_UP_LAYERS = 2
SEA_CHUNK_Y = 0
WORLD_LO_CHUNK_Y = -33
WORLD_HI_CHUNK_Y = 40


def disc_columns(render_distance: int) -> int:
    r = render_distance
    return sum(1 for dx in range(-r, r + 1) for dz in range(-r, r + 1) if dx * dx + dz * dz <= r * r)


def chunk_layer(position_y: float) -> int:
    return math.floor(float(position_y) / CHUNK_SIZE)


def load_layers(center_y: int, render_distance: int) -> list[int]:
    """Active chunk layers for a load centre layer, as ComputeLayerWindow and
    BuildLoadVolume produce them."""
    vertical = min(render_distance, VERTICAL_RENDER_RADIUS)
    depth = min((SEA_CHUNK_Y - 1) - center_y, vertical)
    shift = max(0, min(depth * DEPTH_SHIFT_PER_LAYER, vertical - MIN_UP_LAYERS))
    lo = center_y - (vertical + shift)
    hi = center_y + (vertical - shift)

    # Slide off the ceiling, then the floor, each stopping one layer short of
    # the centre so [center_y - 1, center_y + 1] always stays inside.
    if hi > WORLD_HI_CHUNK_Y:
        move = min(hi - WORLD_HI_CHUNK_Y, (hi - center_y) - 1)
        lo -= move
        hi -= move
    if lo < WORLD_LO_CHUNK_Y:
        move = min(WORLD_LO_CHUNK_Y - lo, (center_y - lo) - 1)
        lo += move
        hi += move

    layers = set(range(max(lo, WORLD_LO_CHUNK_Y), min(hi, WORLD_HI_CHUNK_Y) + 1))
    layers.update((center_y - 1, center_y, center_y + 1))
    return sorted(layers)


def load_window(center_y: int, render_distance: int) -> tuple[int, int, int]:
    """(lo, hi, layers) as the state dump's loadWindow reports them."""
    layers = load_layers(center_y, render_distance)
    return layers[0], layers[-1], len(layers)


def expected_active(position_y: float, render_distance: int) -> int:
    return disc_columns(render_distance) * len(load_layers(chunk_layer(position_y), render_distance))


def effective_render_distance(render_distance_arg: int | None) -> int:
    return DEFAULT_RENDER_DISTANCE if render_distance_arg is None else render_distance_arg


def render_distance_flag(render_distance_arg: int | None) -> list[str]:
    """Forwarded only when given, so a default run checks the engine default."""
    return [] if render_distance_arg is None else ["--render-distance", str(render_distance_arg)]


def self_check_loader_mirror() -> None:
    """Checks the mirror against the cylindrical-chunk-loading spec scenarios."""
    require(disc_columns(8) == 197, f"loader mirror: disc_columns(8) == {disc_columns(8)}, expected 197")
    require(disc_columns(16) == 797, f"loader mirror: disc_columns(16) == {disc_columns(16)}, expected 797")
    require(disc_columns(32) == 3209, f"loader mirror: disc_columns(32) == {disc_columns(32)}, expected 3209")
    window_cases = (
        (4, 8, list(range(-4, 13))),
        (-1, 8, list(range(-9, 8))),
        (-8, 8, list(range(-22, -5))),
        (-32, 8, list(range(-33, -16))),
        (38, 8, list(range(24, 41))),
        (60, 8, [59, 60, 61]),
    )
    for center_y, r, expected in window_cases:
        actual = load_layers(center_y, r)
        require(
            actual == expected,
            f"loader mirror: layers for Cy={center_y} R={r} are {actual}, expected {expected}",
        )
    for r in range(MIN_RENDER_DISTANCE, MAX_RENDER_DISTANCE + 1):
        budget = 2 * min(r, VERTICAL_RENDER_RADIUS) + 1
        for center_y in range(-40, 71):
            layers = load_layers(center_y, r)
            require(
                len(layers) <= budget,
                f"loader mirror: Cy={center_y} R={r} has {len(layers)} layers, budget {budget}",
            )
            require(
                all(y in layers for y in (center_y - 1, center_y, center_y + 1)),
                f"loader mirror: Cy={center_y} R={r} misses a layer around the centre",
            )
            # Two layers above the centre exist only when both are in the world.
            if r == 3 and WORLD_LO_CHUNK_Y <= center_y and center_y + 2 <= WORLD_HI_CHUNK_Y:
                above = sum(1 for y in layers if y > center_y)
                require(above >= 2, f"loader mirror: Cy={center_y} R=3 keeps {above} layers above the centre")

PROFILER_SCOPES = (
    "frame",
    "Mesh build",
    "Mesh snap",
    "Mesh upload",
    "World draw",
    "World update",
    "Frame setup",
    "UI build",
    "UI submit",
    "Present",
    "Player",
    "Worker gen",
)

IDLE_ZERO_SCOPES = (
    "Mesh build",
    "Mesh snap",
    "Mesh upload",
    "World update",
    "Worker gen",
)


class RegressionFailure(Exception):
    """Raised when a scenario assertion fails."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RegressionFailure(message)


def approx_equal(actual: float, expected: float, tol: float = 1e-3) -> bool:
    return abs(float(actual) - float(expected)) <= tol


def assert_vec3(node: dict[str, Any], x: float, y: float, z: float, label: str, tol: float = 1e-2) -> None:
    require(
        approx_equal(node["x"], x, tol)
        and approx_equal(node["y"], y, tol)
        and approx_equal(node["z"], z, tol),
        f"{label}: expected ({x}, {y}, {z}), got ({node['x']}, {node['y']}, {node['z']})",
    )


def load_json(path: Path) -> dict[str, Any]:
    require(path.is_file(), f"missing state dump: {path}")
    with path.open("r", encoding="utf-8") as handle:
        state = json.load(handle)
    if isinstance(state.get("chunks"), dict) and "uniformAir" in state["chunks"]:
        assert_chunk_storage(state, path.name)
    return state


def assert_chunk_storage(state: dict[str, Any], label: str) -> None:
    """Storage breakdown invariants of the sparse chunk representation.

    Every active chunk is exactly one of uniform air, uniform non-air or dense,
    and each dense chunk holds one pool buffer. While generation is in flight a
    worker may be between leasing a buffer and flagging the chunk dense, so the
    lease count is compared only once no chunk is generating.
    """
    chunks = state["chunks"]
    active = chunks.get("active")
    air = chunks.get("uniformAir")
    solid = chunks.get("uniformSolid")
    dense = chunks.get("dense")
    used = chunks.get("densePoolUsed")
    capacity = chunks.get("densePoolCapacity")
    slabs = chunks.get("densePoolSlabs")
    require(
        air + solid + dense == active,
        f"{label}: expected uniformAir + uniformSolid + dense == active, got {air} + {solid} + {dense} != {active}",
    )
    require(0 <= dense <= active, f"{label}: expected 0 <= dense <= active, got {dense} / {active}")
    require(
        capacity == slabs * DENSE_SLAB_CAPACITY and 0 <= used <= capacity,
        f"{label}: expected densePoolUsed {used} within capacity {capacity} of {slabs} slabs",
    )
    if chunks.get("generating") == 0:
        require(used == dense, f"{label}: expected densePoolUsed == dense, got {used} != {dense}")


def assert_png(path: Path, width: int = EXPECTED_WIDTH, height: int = EXPECTED_HEIGHT) -> bytes:
    require(path.is_file(), f"missing screenshot: {path}")
    data = path.read_bytes()
    require(len(data) > 1024, f"screenshot is unexpectedly small ({len(data)} bytes): {path}")
    require(data[:8] == PNG_SIGNATURE, f"invalid PNG signature in {path}")
    require(data[12:16] == b"IHDR", f"missing IHDR chunk in {path}")
    img_w, img_h = struct.unpack(">II", data[16:24])
    require(
        img_w == width and img_h == height,
        f"{path.name}: expected {width}x{height}, got {img_w}x{img_h}",
    )
    return data


def decode_png_rgba(path: Path) -> tuple[int, int, int, bytes]:
    """Width, height, bytes per pixel and unfiltered pixel rows of an 8-bit PNG."""
    data = path.read_bytes()
    pos = len(PNG_SIGNATURE)
    width = height = channels = 0
    idat = bytearray()
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, height, depth, color = struct.unpack(">IIBB", body[:10])
            require(depth == 8, f"{path.name}: unsupported PNG bit depth {depth}")
            channels = {0: 1, 2: 3, 4: 2, 6: 4}[color]
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length
    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    out = bytearray()
    prev = bytearray(stride)
    offset = 0
    for _ in range(height):
        kind = raw[offset]
        line = bytearray(raw[offset + 1:offset + 1 + stride])
        offset += 1 + stride
        for x in range(stride):
            left = line[x - channels] if x >= channels else 0
            up = prev[x]
            if kind == 1:
                line[x] = (line[x] + left) & 0xFF
            elif kind == 2:
                line[x] = (line[x] + up) & 0xFF
            elif kind == 3:
                line[x] = (line[x] + ((left + up) >> 1)) & 0xFF
            elif kind == 4:
                corner = prev[x - channels] if x >= channels else 0
                guess = left + up - corner
                dl, du, dc = abs(guess - left), abs(guess - up), abs(guess - corner)
                pred = left if dl <= du and dl <= dc else (up if du <= dc else corner)
                line[x] = (line[x] + pred) & 0xFF
        out += line
        prev = line
    return width, height, channels, bytes(out)


def differing_pixels(a: Path, b: Path) -> list[tuple[int, int]]:
    """Pixel coordinates where two same-sized PNGs differ."""
    if a.read_bytes() == b.read_bytes():
        return []
    wa, ha, ca, pa = decode_png_rgba(a)
    wb, hb, cb, pb = decode_png_rgba(b)
    require((wa, ha, ca) == (wb, hb, cb), f"{a.name} and {b.name} differ in size or format")
    if pa == pb:
        return []
    return [
        ((k // ca) % wa, (k // ca) // wa)
        for k in range(0, len(pa), ca)
        if pa[k:k + ca] != pb[k:k + ca]
    ]


# Golden-image parity. A golden suite is captured from a reference binary on one
# machine and compared against later builds on the same machine and driver, so
# a renderer swap can be proven pixel-identical. Screenshots that differ between
# the reference runs themselves (the F3 timing figures, for example) are kept
# comparable outside the tiles that changed; everything else must match exactly.
GOLDEN_MANIFEST = "manifest.json"
GOLDEN_TILE = 16
# Unstable tiles are grown by this many tiles in every direction: a figure that
# happened to repeat across the reference runs sits next to one that did not.
GOLDEN_TILE_DILATION = 1
GOLDEN_DIFF_SUFFIX = "_golden_diff.png"


def scenario_pngs(out_dir: Path) -> list[Path]:
    return sorted(
        p for p in out_dir.rglob("*.png") if not p.name.endswith(GOLDEN_DIFF_SUFFIX)
    )


def unstable_regions(diff: list[tuple[int, int]], width: int, height: int) -> list[list[int]]:
    """Dilated GOLDEN_TILE tiles covering the differing pixels, merged into
    horizontal runs per tile row, as [x0, y0, x1, y1) boxes."""
    cols = (width + GOLDEN_TILE - 1) // GOLDEN_TILE
    rows = (height + GOLDEN_TILE - 1) // GOLDEN_TILE
    tiles: set[tuple[int, int]] = set()
    for x, y in diff:
        tx, ty = x // GOLDEN_TILE, y // GOLDEN_TILE
        for dy in range(-GOLDEN_TILE_DILATION, GOLDEN_TILE_DILATION + 1):
            for dx in range(-GOLDEN_TILE_DILATION, GOLDEN_TILE_DILATION + 1):
                if 0 <= tx + dx < cols and 0 <= ty + dy < rows:
                    tiles.add((tx + dx, ty + dy))
    regions: list[list[int]] = []
    for ty in range(rows):
        tx = 0
        while tx < cols:
            if (tx, ty) not in tiles:
                tx += 1
                continue
            start = tx
            while tx < cols and (tx, ty) in tiles:
                tx += 1
            regions.append([
                start * GOLDEN_TILE,
                ty * GOLDEN_TILE,
                min(tx * GOLDEN_TILE, width),
                min((ty + 1) * GOLDEN_TILE, height),
            ])
    return regions


def inside_regions(x: int, y: int, regions: list[list[int]]) -> bool:
    return any(x0 <= x < x1 and y0 <= y < y1 for x0, y0, x1, y1 in regions)


def png_dimensions(path: Path) -> tuple[int, int]:
    data = path.read_bytes()[:24]
    require(data[:8] == PNG_SIGNATURE, f"invalid PNG signature in {path}")
    return struct.unpack(">II", data[16:24])


def write_rgb_png(path: Path, width: int, height: int, rgb: bytes) -> None:
    def chunk(kind: bytes, body: bytes) -> bytes:
        crc = zlib.crc32(kind + body) & 0xFFFFFFFF
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", crc)

    stride = width * 3
    raw = b"".join(b"\x00" + rgb[row * stride:(row + 1) * stride] for row in range(height))
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    path.write_bytes(
        PNG_SIGNATURE + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    )


def write_golden_diff(actual: Path, failing: list[tuple[int, int]]) -> Path:
    """The actual image dimmed to a third, with every failing pixel in red."""
    width, height, channels, pixels = decode_png_rgba(actual)
    rgb = bytearray(width * height * 3)
    for i in range(width * height):
        src = pixels[i * channels:i * channels + 3] if channels >= 3 else pixels[i * channels:i * channels + 1] * 3
        rgb[i * 3:i * 3 + 3] = bytes(c // 3 for c in src)
    for x, y in failing:
        k = (y * width + x) * 3
        rgb[k:k + 3] = b"\xff\x00\x00"
    out = actual.with_name(actual.stem + GOLDEN_DIFF_SUFFIX)
    write_rgb_png(out, width, height, bytes(rgb))
    return out


def read_gl_renderer(out_dir: Path) -> str:
    """The GL renderer string the state dumps report, or "" if none does."""
    for path in sorted(out_dir.rglob("*.json")):
        try:
            state = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        render = state.get("render") if isinstance(state, dict) else None
        if isinstance(render, dict) and render.get("glRenderer"):
            return str(render["glRenderer"])
    return ""


def mesh_signatures(run_dir: Path) -> dict[Path, str | None]:
    """The meshing variant each screenshot of a run was taken against.

    Chunk meshing is asynchronous: whether a border chunk was meshed before or
    after its neighbour arrived or left can change how its faces merge, so two
    runs of one script can draw a few pixels differently with an identical
    renderer. The mesh vertex totals of the nearest state dump identify that
    variant: the dump with the screenshot's own name, otherwise the last one
    written before it, otherwise the first one after it. None when the scenario
    dumps no state.
    """
    dumps: list[tuple[float, Path, str]] = []
    for path in run_dir.rglob("*.json"):
        try:
            state = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        if isinstance(state, dict) and isinstance(state.get("meshVertices"), dict):
            dumps.append((path.stat().st_mtime, path, json.dumps(state["meshVertices"], sort_keys=True)))
    dumps.sort()
    by_stem = {path.with_suffix(""): sig for _, path, sig in dumps}
    signatures: dict[Path, str | None] = {}
    for png in scenario_pngs(run_dir):
        sig = by_stem.get(png.with_suffix(""))
        if sig is None and dumps:
            written = png.stat().st_mtime
            before = [d for d in dumps if d[0] <= written]
            sig = (before[-1] if before else dumps[0])[2]
        signatures[png] = sig
    return signatures


class GoldenCapture:
    """Accumulates one render distance's golden images and manifest."""

    def __init__(self, root: Path, render_distance: int, binary: Path, runs: int) -> None:
        self.dir = root / f"r{render_distance}"
        self.manifest: dict[str, Any] = {
            "binary": str(binary),
            "renderDistance": render_distance,
            "runs": runs,
            "tile": GOLDEN_TILE,
            "glRenderer": "",
            "images": {},
        }
        if self.dir.exists():
            shutil.rmtree(self.dir)
        self.dir.mkdir(parents=True)

    def record(self, name: str, run_dirs: list[Path]) -> tuple[int, int, int]:
        """Stores one golden image per meshing variant seen across the runs.

        Returns (images, extra variants, images with unstable regions).
        """
        first = run_dirs[0]
        if not self.manifest["glRenderer"]:
            self.manifest["glRenderer"] = read_gl_renderer(first)
        run_sigs = [mesh_signatures(run) for run in run_dirs]
        images = extra_variants = unstable = 0
        for png in scenario_pngs(first):
            rel = png.relative_to(first)
            copies = [run / rel for run in run_dirs]
            for copy in copies:
                require(copy.is_file(), f"golden capture: {name}/{rel} missing from a reference run")
            sigs = [run_sig.get(copy) for run_sig, copy in zip(run_sigs, copies)]
            width, height = png_dimensions(png)

            # Runs that drew against the same meshes must agree; whatever still
            # differs between them is timing, kept comparable outside its tiles.
            variants: list[dict[str, Any]] = []
            for sig in dict.fromkeys(sigs):
                members = [c for c, s in zip(copies, sigs) if s == sig]
                diff: set[tuple[int, int]] = set()
                for other in members[1:]:
                    diff.update(differing_pixels(members[0], other))
                index = len(variants)
                rel_file = rel if index == 0 else rel.with_name(f"{rel.stem}.v{index}{rel.suffix}")
                dest = self.dir / name / rel_file
                dest.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(members[0], dest)
                variants.append({
                    "signature": sig,
                    "file": f"{name}/{rel_file.as_posix()}",
                    "runs": len(members),
                    "unstable": unstable_regions(sorted(diff), width, height) if diff else [],
                })
            all_diff: set[tuple[int, int]] = set()
            for other in copies[1:]:
                all_diff.update(differing_pixels(png, other))

            self.manifest["images"][f"{name}/{rel.as_posix()}"] = {
                "width": width,
                "height": height,
                # Fallback for a run whose meshing variant no reference run had.
                "unstable": unstable_regions(sorted(all_diff), width, height) if all_diff else [],
                "variants": variants,
            }
            images += 1
            extra_variants += len(variants) - 1
            unstable += 1 if any(v["unstable"] for v in variants) else 0
        return images, extra_variants, unstable

    def write(self) -> Path:
        path = self.dir / GOLDEN_MANIFEST
        path.write_text(json.dumps(self.manifest, indent=2) + "\n", encoding="utf-8")
        return path


class GoldenComparison:
    """Compares a run's screenshots against one render distance's golden images."""

    def __init__(self, root: Path, render_distance: int) -> None:
        self.dir = root / f"r{render_distance}"
        manifest_path = self.dir / GOLDEN_MANIFEST
        require(manifest_path.is_file(), f"golden: no manifest at {manifest_path}; capture one first")
        self.manifest: dict[str, Any] = json.loads(manifest_path.read_text(encoding="utf-8"))
        self.renderer_checked = False

    def compare(self, name: str, out_dir: Path) -> tuple[int, int, int]:
        """Raises on any mismatch.

        Returns (images compared, images matched against their own meshing
        variant, images that fell back to the all-runs unstable regions).
        """
        # Checked on the first scenario that dumps state; some take only screenshots.
        actual_renderer = "" if self.renderer_checked else read_gl_renderer(out_dir)
        if actual_renderer:
            expected = self.manifest.get("glRenderer", "")
            require(
                actual_renderer == expected,
                f"golden: captured on renderer '{expected}', this run reports '{actual_renderer}'",
            )
            self.renderer_checked = True

        images: dict[str, Any] = self.manifest["images"]
        expected_keys = {k for k in images if k.split("/", 1)[0] == name}
        actual_pngs = {f"{name}/{p.relative_to(out_dir).as_posix()}": p for p in scenario_pngs(out_dir)}
        missing = sorted(expected_keys - actual_pngs.keys())
        extra = sorted(actual_pngs.keys() - expected_keys)
        require(not missing, f"golden: {name} did not produce {', '.join(missing)}")
        require(not extra, f"golden: {name} produced screenshots with no golden image: {', '.join(extra)}")

        signatures = mesh_signatures(out_dir)
        matched = fallback = 0
        for key, actual in sorted(actual_pngs.items()):
            entry = images[key]
            width, height = png_dimensions(actual)
            require(
                (width, height) == (entry["width"], entry["height"]),
                f"golden: {key} is {width}x{height}, golden is {entry['width']}x{entry['height']}",
            )
            sig = signatures.get(actual)
            variant = next((v for v in entry["variants"] if v["signature"] == sig), None)
            if variant is not None:
                golden = self.dir / variant["file"]
                regions = variant["unstable"]
                matched += 1
            else:
                golden = self.dir / entry["variants"][0]["file"]
                regions = entry["unstable"]
                fallback += 1
            require(golden.is_file(), f"golden: missing golden image {golden}")
            failing = [
                (x, y) for x, y in differing_pixels(golden, actual) if not inside_regions(x, y, regions)
            ]
            if failing:
                diff_path = write_golden_diff(actual, failing)
                cause = (
                    f"against its meshing variant {golden.name}"
                    if variant is not None
                    else f"in a meshing variant no reference run had ({sig})"
                )
                raise RegressionFailure(
                    f"golden: {key} differs from the golden image in {len(failing)} pixel(s) "
                    f"{cause} (diff: {diff_path})"
                )
        return len(actual_pngs), matched, fallback


# Rendered chunk counts per camera pose, with occlusion culling on (the engine
# default). They depend on the generated terrain, on the loaded cylinder and on
# which chunks are reachable from the camera, so they are recorded per generator
# version (worldgen.generatorVersion in the dumps) and then per render distance;
# a missing entry fails instead of comparing against other numbers. The
# frustum-only counts the same poses had before occlusion culling are kept in
# the trailing comments for reference. A scripted run steps one tick per frame,
# so the camera sits exactly on the last tick's position and the counts are
# exact: there is no frame-timing variation left to tolerate.
EXPECTED_RENDERED: dict[int, dict[int, dict[str, int]]] = {
    # Only the current generator's counts are kept: every terrain change
    # replaces them, and an older version's numbers could only mislead.
    6: {
        8: {
            "spawn": 394,  # 618 frustum-only
            "far": 205,  # 380
            "after_ticks": 205,  # 380
            "landed": 269,  # 594
            "after_walk": 221,  # 577
            "after_break": 212,  # 803
            "after_place": 212,  # 792
            "after_jump": 212,  # 805
        },
        16: {
            "spawn": 464,  # 690 frustum-only
            "far": 505,  # 982
            "after_ticks": 505,  # 982
            "landed": 416,  # 764
            "after_walk": 296,  # 750
            "after_break": 291,  # 994
            "after_place": 291,  # 970
            "after_jump": 291,  # 996
        },
    },
}


def lookup_versioned(
    table: dict[int, dict[int, Any]], state: dict[str, Any], render_distance: int, what: str
) -> Any:
    version = state.get("worldgen", {}).get("generatorVersion")
    entry = table.get(version, {}).get(render_distance)
    require(
        entry is not None,
        f"no expected {what} recorded for generator version {version} at render distance {render_distance}",
    )
    return entry


def expected_rendered(state: dict[str, Any], render_distance: int, pose: str) -> int:
    return lookup_versioned(EXPECTED_RENDERED, state, render_distance, "rendered chunk counts")[pose]


def assert_load_window(state: dict[str, Any], label: str, render_distance: int) -> None:
    window = state.get("loadWindow")
    if window is None:
        return
    center_y = chunk_layer(state["player"]["position"]["y"])
    lo, hi, layers = load_window(center_y, render_distance)
    actual = (window.get("lo"), window.get("hi"), window.get("layers"))
    require(
        actual == (lo, hi, layers),
        f"{label}: expected loadWindow (lo, hi, layers) == {(lo, hi, layers)} for chunk layer {center_y}, got {actual}",
    )


def assert_occlusion_counts(state: dict[str, Any], label: str) -> None:
    chunks = state.get("chunks", {})
    if "occluded" not in chunks:
        return
    occluded = chunks["occluded"]
    visible = chunks.get("cull", {}).get("visible")
    require(
        visible is not None and 0 <= occluded <= visible,
        f"{label}: expected 0 <= chunks.occluded <= chunks.cull.visible, got {occluded} and {visible}",
    )
    if chunks.get("occlusion") is False:
        require(occluded == 0, f"{label}: occlusion is off but chunks.occluded == {occluded}")


def assert_settled_chunks(
    state: dict[str, Any], label: str, render_distance: int, pose: str | None = None
) -> None:
    """Settled-world checks; pose selects the rendered count, None skips it."""
    require(state.get("settled") is True, f"{label}: expected settled == true")
    require(
        state.get("renderDistance") == render_distance,
        f"{label}: expected renderDistance == {render_distance}, got {state.get('renderDistance')}",
    )
    active = expected_active(state["player"]["position"]["y"], render_distance)
    chunks = state.get("chunks", {})
    require(
        chunks.get("active") == active,
        f"{label}: expected chunks.active == {active}, got {chunks.get('active')}",
    )
    require(
        chunks.get("generated") == active,
        f"{label}: expected chunks.generated == {active}, got {chunks.get('generated')}",
    )
    require(chunks.get("generating") == 0, f"{label}: expected chunks.generating == 0")
    require(chunks.get("dirty") == 0, f"{label}: expected chunks.dirty == 0")
    require(chunks.get("meshing") == 0, f"{label}: expected chunks.meshing == 0")
    require(chunks.get("meshUploadPending") is False, f"{label}: expected meshUploadPending == false")
    assert_load_window(state, label, render_distance)
    assert_occlusion_counts(state, label)
    if pose is not None:
        rendered = expected_rendered(state, render_distance, pose)
        if isinstance(rendered, tuple):
            require(
                chunks.get("rendered") in rendered,
                f"{label}: expected chunks.rendered in {rendered}, got {chunks.get('rendered')}",
            )
        else:
            require(
                chunks.get("rendered") == rendered,
                f"{label}: expected chunks.rendered == {rendered}, got {chunks.get('rendered')}",
            )


def assert_idle_profiler(state: dict[str, Any], label: str) -> None:
    profiler = state.get("profiler")
    if profiler is None:
        return

    for scope in PROFILER_SCOPES:
        require(scope in profiler, f"{label}: missing profiler scope '{scope}'")
        scope_node = profiler[scope]
        for metric in ("averageUs", "maxUs", "onePercentLowUs"):
            require(metric in scope_node, f"{label}: scope '{scope}' missing '{metric}'")
            require(scope_node[metric] >= 0, f"{label}: scope '{scope}' has negative '{metric}'")

    for scope in IDLE_ZERO_SCOPES:
        avg_us = profiler[scope]["averageUs"]
        require(
            avg_us == 0,
            f"{label}: expected '{scope}.averageUs' == 0 at rest, got {avg_us}",
        )

    if "solidSamples" in profiler:
        require(profiler["solidSamples"] > 0, f"{label}: expected solidSamples > 0")
        overdraw = float(profiler.get("solidOverdraw", 0.0))
        require(0.05 < overdraw < 3.0, f"{label}: unexpected solidOverdraw {overdraw}")


def verify_smoke(out_dir: Path, run_log: str, render_distance: int) -> None:
    require("Noclip activated" in run_log, "smoke: missing 'Noclip activated' in run.log")
    require("Tp to X:0.0 Y:80.0 Z:0.0" in run_log, "smoke: missing teleport confirmation in run.log")
    require("X:0.0 Y:80.0 Z:0.0" in run_log, "smoke: missing /pos output in run.log")
    require(
        f"World Seed: {EXPECTED_SEED}" in run_log,
        f"smoke: expected 'World Seed: {EXPECTED_SEED}' in run.log",
    )


def verify_spawn_overview(out_dir: Path, _run_log: str, render_distance: int) -> None:
    normal_png = assert_png(out_dir / "spawn.png")
    wireframe_png = assert_png(out_dir / "spawn_wireframe.png")
    require(
        normal_png != wireframe_png,
        "spawn_overview: spawn.png and spawn_wireframe.png are byte-identical",
    )

    spawn = load_json(out_dir / "spawn.json")
    assert_settled_chunks(spawn, "spawn.json", render_distance, "spawn")
    assert_idle_profiler(spawn, "spawn.json")

    # The script teleports 40 blocks above the spawn feet position.
    player = spawn["player"]
    origin = spawn["worldgen"]["spawn"]
    require(player["noclip"] is True, "spawn.json: expected player.noclip == true")
    assert_vec3(
        player["position"],
        float(origin["x"]),
        float(origin["feetY"]) + 40.0,
        float(origin["z"]),
        "spawn.json player.position",
    )


def verify_loaded(out_dir: Path, _run_log: str, render_distance: int) -> None:
    assert_png(out_dir / "far.png")

    far = load_json(out_dir / "far.json")
    after_ticks = load_json(out_dir / "after_ticks.json")

    assert_settled_chunks(far, "far.json", render_distance, "far")
    assert_settled_chunks(after_ticks, "after_ticks.json", render_distance, "after_ticks")
    assert_idle_profiler(far, "far.json")
    assert_idle_profiler(after_ticks, "after_ticks.json")

    assert_vec3(far["player"]["position"], 500.0, 90.0, 500.0, "far.json player.position")
    assert_vec3(
        after_ticks["player"]["position"],
        500.0,
        90.0,
        500.0,
        "after_ticks.json player.position",
    )

    tick_delta = int(after_ticks["tick"]) - int(far["tick"])
    require(
        tick_delta >= 40,
        f"loaded: expected tick advance >= 40, got {tick_delta} ({far['tick']} -> {after_ticks['tick']})",
    )
    require(
        int(after_ticks["frame"]) > int(far["frame"]),
        "loaded: expected frame counter to advance during wait ticks 40",
    )


def verify_interact(out_dir: Path, _run_log: str, render_distance: int) -> None:
    break_png = assert_png(out_dir / "after_break.png")
    place_png = assert_png(out_dir / "after_place.png")
    require(
        break_png != place_png,
        "interact: after_break.png and after_place.png are byte-identical",
    )

    landed = load_json(out_dir / "landed.json")
    before_walk = load_json(out_dir / "before_walk.json")
    after_walk = load_json(out_dir / "after_walk.json")
    before_break = load_json(out_dir / "before_break.json")
    after_break = load_json(out_dir / "after_break.json")
    after_place = load_json(out_dir / "after_place.json")
    after_jump = load_json(out_dir / "after_jump.json")

    # Expected coordinates come from the generator, not from literals: the
    # spawn column and the surface the dump reports for it.
    spawn = landed["worldgen"]["spawn"]
    sx = float(spawn["x"])
    sz = float(spawn["z"])
    surface = float(landed["worldgen"]["surfaceY"])
    require(
        int(surface) == int(spawn["surface"]),
        f"landed.json: player column surface {surface} differs from spawn surface {spawn['surface']}",
    )

    # 1. Gravity & ground collision after falling from the spawn height. Blocks
    #    are centered on integer coordinates, so feet rest at surface + 0.5.
    assert_settled_chunks(landed, "landed.json", render_distance, "landed")
    assert_idle_profiler(landed, "landed.json")
    require(landed["player"]["grounded"] is True, "landed.json: expected player.grounded == true")
    require(landed["player"]["noclip"] is False, "landed.json: expected player.noclip == false")
    assert_vec3(landed["player"]["position"], sx, surface + 0.5, sz, "landed.json player.position")
    assert_vec3(landed["player"]["velocity"], 0.0, 0.0, 0.0, "landed.json player.velocity", tol=1e-4)
    require(landed["targetBlock"]["hit"] is True, "landed.json: expected targetBlock.hit == true")

    # 2. Walking forward along +Z with real collision.
    assert_settled_chunks(after_walk, "after_walk.json", render_distance, "after_walk")
    require(after_walk["player"]["grounded"] is True, "after_walk.json: expected player.grounded == true")
    z_before = float(before_walk["player"]["position"]["z"])
    z_after = float(after_walk["player"]["position"]["z"])
    require(
        z_after > z_before + 0.05,
        f"interact: expected forward walk to increase Z ({z_before} -> {z_after})",
    )

    # 3. Looking straight down hits either the surface block of the player's
    #    column or flora standing on it. Breaking it must expose the block one
    #    below, whichever of the two was hit.
    target = before_break["targetBlock"]
    require(target["hit"] is True, "before_break.json: expected targetBlock.hit == true")
    bx = float(target["position"]["x"])
    by = float(target["position"]["y"])
    bz = float(target["position"]["z"])
    break_surface = float(before_break["worldgen"]["surfaceY"])
    require(
        by in (break_surface, break_surface + 1.0),
        f"before_break.json: expected target at surface {break_surface} or the flora above it, got y={by}",
    )
    if by == break_surface:
        require(
            target["blockName"] == before_break["worldgen"]["surfaceBlock"],
            f"before_break.json: expected {before_break['worldgen']['surfaceBlock']}, got {target.get('blockName')}",
        )

    assert_settled_chunks(after_break, "after_break.json", render_distance, "after_break")
    require(after_break["targetBlock"]["hit"] is True, "after_break.json: expected targetBlock.hit == true")
    assert_vec3(after_break["targetBlock"]["position"], bx, by - 1.0, bz, "after_break.json targetBlock.position")
    require(
        after_break["targetBlock"]["blockId"] != 0,
        "after_break.json: expected a solid block under the broken one",
    )

    # 4. The block picked before breaking lands in the active slot (the
    #    initial hotbar is empty, so slot 1 / index 0) and is placed back where
    #    the broken block was.
    picked_id = target["blockId"]
    assert_settled_chunks(after_place, "after_place.json", render_distance, "after_place")
    require(
        after_place["player"]["hotbarSlot"] == 0,
        f"after_place.json: expected hotbarSlot == 0, got {after_place['player']['hotbarSlot']}",
    )
    require(
        after_place["player"]["hotbar"][0] == picked_id,
        f"after_place.json: expected the picked block {picked_id} in slot 1, got {after_place['player']['hotbar'][0]}",
    )
    require(after_place["targetBlock"]["hit"] is True, "after_place.json: expected targetBlock.hit == true")
    assert_vec3(after_place["targetBlock"]["position"], bx, by, bz, "after_place.json targetBlock.position")
    require(
        after_place["targetBlock"]["blockId"] == picked_id,
        f"after_place.json: expected the picked block {picked_id}, got {after_place['targetBlock']['blockId']}",
    )

    # 5. Jumping leaves the ground with positive vertical velocity.
    assert_settled_chunks(after_jump, "after_jump.json", render_distance, "after_jump")
    require(after_jump["player"]["grounded"] is False, "after_jump.json: expected player.grounded == false")
    require(
        float(after_jump["player"]["velocity"]["y"]) > 0.0,
        f"after_jump.json: expected velocity.y > 0, got {after_jump['player']['velocity']['y']}",
    )
    require(
        float(after_jump["player"]["position"]["y"]) > float(after_place["player"]["position"]["y"]),
        "after_jump.json: expected player.position.y to rise above after_place",
    )


GLASS_BLOCK_ID = 6


INVENTORY_TABS = ("building", "colored", "natural")
INVENTORY_PAGES_CAPTURED = 4
INVENTORY_ROWS_PER_PAGE = 5


def verify_inventory_tab(out_dir: Path, tab: str) -> bytes:
    state = load_json(out_dir / f"inv_{tab}.json")
    inventory = state["inventory"]
    require(inventory["tab"] == tab, f"inv_{tab}.json: expected tab {tab}, got {inventory['tab']}")
    max_row = int(inventory["maxScrollRow"])
    require(max_row > 0, f"inv_{tab}.json: expected the {tab} tab to scroll, maxScrollRow is {max_row}")
    require(
        state["render"]["missingBlockTextures"] == 0,
        f"inv_{tab}.json: {state['render']['missingBlockTextures']} block textures fell back to the checkerboard",
    )

    pages = [assert_png(out_dir / f"{tab}_p{page}.png") for page in range(1, INVENTORY_PAGES_CAPTURED + 1)]
    # Scrolling clamps at maxScrollRow, so only the first real pages must differ.
    real_pages = min(INVENTORY_PAGES_CAPTURED, -(-max_row // INVENTORY_ROWS_PER_PAGE) + 1)
    for page in range(1, real_pages):
        require(
            pages[page - 1] != pages[page],
            f"inventory: {tab}_p{page}.png and {tab}_p{page + 1}.png render identically",
        )
    return pages[0]


def verify_inventory(out_dir: Path, _run_log: str, render_distance: int) -> None:
    first_pages = [verify_inventory_tab(out_dir, tab) for tab in INVENTORY_TABS]
    require(len(set(first_pages)) == len(first_pages), "inventory: two tabs render identically")
    tooltip_png = assert_png(out_dir / "inv_tooltip.png")
    assert_png(out_dir / "after_place.png")
    require(tooltip_png != first_pages[2], "inventory: natural tab with tooltip renders like without it")

    inv_open = load_json(out_dir / "inv_open.json")
    after_put = load_json(out_dir / "after_put.json")
    inv_closed = load_json(out_dir / "inv_closed.json")
    after_place = load_json(out_dir / "after_place.json")

    inventory = inv_open["inventory"]
    require(inventory["open"] is True, "inv_open.json: expected inventory.open == true")
    require(inventory["tab"] == "building", f"inv_open.json: expected tab building, got {inventory['tab']}")

    # The pick/put edit reaches the player through the intent on the next tick.
    hotbar = after_put["player"]["hotbar"]
    require(len(hotbar) == 9, f"after_put.json: expected 9 hotbar slots, got {len(hotbar)}")
    require(hotbar[8] == GLASS_BLOCK_ID, f"after_put.json: expected glass in slot 9, got {hotbar[8]}")
    # An edit to slot 9 must leave every other slot as it was before the put.
    before = inv_open["player"]["hotbar"]
    require(
        hotbar[:8] == before[:8],
        f"after_put.json: slots 1-8 changed from {before[:8]} to {hotbar[:8]}",
    )
    require(after_put["inventory"]["carried"] == 0, "after_put.json: expected nothing carried after put")

    require(inv_closed["inventory"]["open"] is False, "inv_closed.json: expected inventory.open == false")

    target = after_place["targetBlock"]
    require(target["hit"] is True, "after_place.json: expected targetBlock.hit == true")
    require(
        target["blockId"] == GLASS_BLOCK_ID,
        f"after_place.json: expected the placed glass to be targeted, got {target['blockId']}",
    )


WORLDGEN_PREVIEW_SIZE = 256
# One pixel per Y from the bedrock floor (-516) to the generation ceiling (640).
WORLDGEN_SLICE_HEIGHT = 1157
OVERWORLD_MIN_Y = -511
# worldgen_biome_map world spawn spawn 8192 16: 512 samples per side.
BIOME_MAP_SIDE = 512
BIOME_MIN_DISTINCT = 14
BIOME_MAX_LAND_SHARE = 0.35
BIOME_MIN_FAMILY_MATCH = 0.9
# Landform modifiers shipped in assets/worldgen/shape_modifiers.json.
EXPECTED_MODIFIER_BIOMES = ("Badlands", "Stony Peaks", "Shattered Hills", "Desert")
MODIFIER_MIN_AGREEMENT = 0.95
# Ore veins shipped in assets/worldgen/filler.json; the spawn slice must show at
# least this many kinds.
ORE_MIN_KINDS = 4


def verify_worldgen(out_dir: Path, _run_log: str, render_distance: int) -> None:
    for suffix in ("height", "biome"):
        assert_png(out_dir / f"spawn_{suffix}.png", WORLDGEN_PREVIEW_SIZE, WORLDGEN_PREVIEW_SIZE)
    assert_png(out_dir / "spawn_slice.png", WORLDGEN_PREVIEW_SIZE, WORLDGEN_SLICE_HEIGHT)

    stats = load_json(out_dir / "spawn_worldgen.json")
    require(stats["bedrockFloorComplete"] is True, "worldgen: a slice column has no bedrock at Y=-516")
    require(
        stats["solidAboveCeiling"] == 0,
        f"worldgen: {stats['solidAboveCeiling']} non-air blocks at or above the generation ceiling",
    )
    require(
        stats["surfaceMin"] >= OVERWORLD_MIN_Y,
        f"worldgen: surface reaches {stats['surfaceMin']}, below the overworld floor {OVERWORLD_MIN_Y}",
    )
    require(
        stats["surfaceMax"] < stats["generationCeiling"],
        f"worldgen: surface reaches {stats['surfaceMax']}, at or above the ceiling",
    )
    require(
        any(band["fraction"] > 0.0 for band in stats["caveOpenByBand"]),
        "worldgen: no cave space opens in any Y band of the slice",
    )
    require(stats["lavaBlocks"] > 0, "worldgen: the slice holds no lava")
    require(stats["caveWaterBlocks"] > 0, "worldgen: the slice holds no flooded cave water")
    require(stats["deepslateBlocks"] > 0, "worldgen: the slice holds no deepslate")
    ore_kinds = [name for name, count in stats["oreBlocks"].items() if count > 0]
    require(
        len(ore_kinds) >= ORE_MIN_KINDS,
        f"worldgen: the slice holds {len(ore_kinds)} ore kinds ({', '.join(ore_kinds)}), expected at least {ORE_MIN_KINDS}",
    )
    require(stats["structureBlocks"] > 0, "worldgen: no structure block was placed in the slice cubes")
    require(stats["floraBlocks"] > 0, "worldgen: no flora block was placed in the slice cubes")

    assert_png(out_dir / "world_biomes.png", BIOME_MAP_SIDE, BIOME_MAP_SIDE)
    coverage = load_json(out_dir / "world_biomes.json")
    require(
        coverage["distinctBiomes"] >= BIOME_MIN_DISTINCT,
        f"worldgen: biome map shows {coverage['distinctBiomes']} biomes, expected at least {BIOME_MIN_DISTINCT}",
    )
    largest = max(coverage["biomes"], key=lambda biome: biome["landFraction"])
    require(
        largest["landFraction"] <= BIOME_MAX_LAND_SHARE,
        f"worldgen: {largest['name']} covers {largest['landFraction']:.3f} of the land, above {BIOME_MAX_LAND_SHARE}",
    )
    require(
        coverage["oceanWetFraction"] >= BIOME_MIN_FAMILY_MATCH,
        f"worldgen: only {coverage['oceanWetFraction']:.3f} of ocean-biome cells lie below sea level",
    )
    require(
        coverage["peakHighFraction"] >= BIOME_MIN_FAMILY_MATCH,
        f"worldgen: only {coverage['peakHighFraction']:.3f} of peak-biome cells lie above sea level + 64",
    )
    modifiers = {modifier["biome"]: modifier for modifier in coverage["modifiers"]}
    for biome in EXPECTED_MODIFIER_BIOMES:
        modifier = modifiers.get(biome)
        require(modifier is not None, f"worldgen: no landform modifier for {biome}")
        require(
            modifier["positiveFraction"] > 0.0 and modifier["fullFraction"] > 0.0,
            f"worldgen: the {biome} landform never reaches full weight on the biome map",
        )
        require(
            modifier["biomeAgreement"] >= MODIFIER_MIN_AGREEMENT,
            f"worldgen: only {modifier['biomeAgreement']:.3f} of full-weight {biome} landform cells are {biome}",
        )

    state = load_json(out_dir / "worldgen.json")
    classes = state["worldgen"]["cubeClasses"]
    for cube_class in ("air", "solid", "mixed"):
        require(
            classes[cube_class] > 0,
            f"worldgen.json: expected some {cube_class.upper()} cubes, got {classes[cube_class]}",
        )
    cache = state["worldgen"]["columnCache"]
    require(cache["hits"] > 0, "worldgen.json: the column cache recorded no hits")


# Upper bound on translucent vertices over open ocean (ocean_down.json), per
# generator version and render distance. Recorded as the calibrated count: a
# lower value (better merging) passes, a higher one means translucent faces
# stopped merging.
EXPECTED_TRANSLUCENT_VERTICES_MAX: dict[int, dict[int, int]] = {
    6: {
        # Known engine non-determinism: the nominal count is 7208, but runs
        # occasionally report 7220 or 7240 (+3 to +8 quads). The cause is
        # the asynchronous order in which neighbours at the cylinder's outer edge
        # are evicted and remeshed after the /tp. The ceiling admits these small
        # timing-dependent boundary variations; a real loss of translucent merging
        # jumps to >300,000 vertices.
        8: 7260,
        # Nominal 138672; runs report up to 138704 for the same reason as R=8.
        16: 138736,
    },
}


def verify_translucent(out_dir: Path, _run_log: str, render_distance: int) -> None:
    pngs = {
        name: assert_png(out_dir / f"{name}.png")
        for name in ("ocean_down", "ocean_shallow", "ocean_wireframe", "glass_water")
    }
    require(
        pngs["ocean_down"] != pngs["ocean_wireframe"],
        "translucent: ocean_down.png and ocean_wireframe.png are byte-identical",
    )

    for name in ("ocean_down", "glass_water"):
        state = load_json(out_dir / f"{name}.json")
        assert_settled_chunks(state, f"{name}.json", render_distance)
        translucent = state["meshVertices"]["translucent"]
        require(translucent > 0, f"{name}.json: expected meshVertices.translucent > 0, got {translucent}")
        if name == "ocean_down":
            ceiling = lookup_versioned(
                EXPECTED_TRANSLUCENT_VERTICES_MAX, state, render_distance, "translucent vertex ceiling"
            )
            require(
                translucent <= ceiling,
                f"{name}.json: meshVertices.translucent {translucent} exceeds the recorded ceiling {ceiling}",
            )


DEEP_POSES = (("deep_60", -60.0), ("deep_120", -120.0), ("deep_500", -500.0))


def verify_deep(out_dir: Path, _run_log: str, render_distance: int) -> None:
    for name, y in DEEP_POSES:
        assert_png(out_dir / f"{name}.png")
        state = load_json(out_dir / f"{name}.json")
        assert_vec3(state["player"]["position"], 0.0, y, 0.0, f"{name}.json player.position")
        assert_settled_chunks(state, f"{name}.json", render_distance)
        require("loadWindow" in state, f"{name}.json: missing loadWindow")


# Poses of scripts/occlusion.mgs, each captured with occlusion on and off.
OCCLUSION_POSES = ("surface", "sky", "hillside", "cave_80", "cave_150", "buried")
OCCLUSION_CAVE_POSES = ("cave_80", "cave_150")
# The buried camera sees out through back faces, so occlusion must stand down.
OCCLUSION_FALLBACK_POSES = ("buried",)
# A chunk occlusion removes can show through a rasterisation crack between two
# nearer quads (a T-junction of greedy faces); those few isolated pixels are the
# only difference occlusion is allowed to make.
OCCLUSION_MAX_CRACK_PIXELS = 16


def verify_occlusion(out_dir: Path, _run_log: str, render_distance: int) -> None:
    for pose in OCCLUSION_POSES:
        on_png = out_dir / f"{pose}_on.png"
        off_png = out_dir / f"{pose}_off.png"
        assert_png(on_png)
        assert_png(off_png)
        diff = differing_pixels(on_png, off_png)
        differing = set(diff)
        clustered = [
            (x, y) for x, y in diff
            if {(x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)} & differing
        ]
        require(
            len(diff) <= OCCLUSION_MAX_CRACK_PIXELS and not clustered,
            f"occlusion {pose}: on/off screenshots differ in {len(diff)} pixels "
            f"({len(clustered)} adjacent to another); first {diff[:5]}",
        )

        on = load_json(out_dir / f"{pose}_on.json")
        off = load_json(out_dir / f"{pose}_off.json")
        for label, state in ((f"{pose}_on.json", on), (f"{pose}_off.json", off)):
            assert_settled_chunks(state, label, render_distance)
        on_chunks = on["chunks"]
        off_chunks = off["chunks"]
        require(on_chunks.get("occlusion") is True, f"{pose}_on.json: expected chunks.occlusion == true")
        require(off_chunks.get("occlusion") is False, f"{pose}_off.json: expected chunks.occlusion == false")
        require(off_chunks.get("occluded") == 0, f"{pose}_off.json: expected chunks.occluded == 0")
        require(
            on_chunks["cull"]["visible"] == off_chunks["cull"]["visible"],
            f"occlusion {pose}: cull.visible differs on/off "
            f"({on_chunks['cull']['visible']} vs {off_chunks['cull']['visible']})",
        )
        require(
            on_chunks["rendered"] <= off_chunks["rendered"],
            f"occlusion {pose}: rendered rose with occlusion on "
            f"({on_chunks['rendered']} vs {off_chunks['rendered']})",
        )

        fallback = on_chunks["cull"].get("occlusionFallback")
        expect_fallback = pose in OCCLUSION_FALLBACK_POSES
        require(
            fallback is expect_fallback,
            f"{pose}_on.json: expected occlusionFallback == {expect_fallback}, got {fallback}",
        )
        if pose in OCCLUSION_CAVE_POSES:
            require(
                on_chunks["occluded"] > 0,
                f"{pose}_on.json: expected chunks.occluded > 0 inside a cave, got {on_chunks['occluded']}",
            )


Verifier = Callable[[Path, str, int], None]

# (name, script, verifier, minimum --script-timeout in seconds)
SCENARIOS: tuple[tuple[str, str, Verifier, int], ...] = (
    ("smoke", "scripts/smoke.mgs", verify_smoke, 0),
    ("spawn_overview", "scripts/spawn_overview.mgs", verify_spawn_overview, 0),
    ("loaded", "scripts/loaded.mgs", verify_loaded, 0),
    ("interact", "scripts/interact.mgs", verify_interact, 0),
    ("inventory", "scripts/inventory.mgs", verify_inventory, 0),
    ("worldgen", "scripts/worldgen.mgs", verify_worldgen, 0),
    ("translucent", "scripts/translucent.mgs", verify_translucent, 300),
    ("deep", "scripts/deep.mgs", verify_deep, 600),
    ("occlusion", "scripts/occlusion.mgs", verify_occlusion, 600),
)


def find_default_binary(repo_root: Path) -> Path:
    candidates = (
        repo_root / "build" / "MineGame.exe",
        repo_root / "build" / "MineGame",
        repo_root / "dist" / "MineGame-linux-x86_64" / "MineGame",
        repo_root / "dist" / "MineGame-windows-x86_64" / "MineGame.exe",
    )
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise FileNotFoundError(
        "could not locate MineGame binary; pass --binary <path> or build the project first"
    )


def find_server_binary(client_binary: Path, repo_root: Path) -> Path:
    sibling_names = ("MineGameServer.exe", "MineGameServer")
    for name in sibling_names:
        sibling = client_binary.parent / name
        if sibling.is_file():
            return sibling
    candidates = (
        repo_root / "build" / "MineGameServer.exe",
        repo_root / "build" / "MineGameServer",
        repo_root / "dist" / "MineGame-linux-x86_64" / "MineGameServer",
        repo_root / "dist" / "MineGame-windows-x86_64" / "MineGameServer.exe",
    )
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise FileNotFoundError(
        "could not locate MineGameServer binary; build the project first"
    )


def run_headless_server_scenario(
    server_binary: Path,
    repo_root: Path,
    out_root: Path,
    timeout_seconds: int,
    render_distance_arg: int | None,
) -> float:
    out_dir = out_root / "headless_server"
    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    cmd = [
        str(server_binary.resolve()),
        "--ticks",
        "100",
        "--seed",
        EXPECTED_SEED,
        "--read-only",
        *render_distance_flag(render_distance_arg),
    ]
    render_distance = effective_render_distance(render_distance_arg)

    start = time.monotonic()
    completed = subprocess.run(
        cmd,
        cwd=str(repo_root),
        capture_output=True,
        text=True,
        timeout=timeout_seconds,
        check=False,
    )
    elapsed = time.monotonic() - start

    combined_log = (completed.stdout or "") + (completed.stderr or "")
    (out_dir / "server.log").write_text(combined_log, encoding="utf-8")

    if completed.returncode != 0:
        sys.stderr.write(f"\n--- headless_server output ---\n{combined_log}\n")
        raise RegressionFailure(
            f"headless_server exited with code {completed.returncode} (expected 0)"
        )

    spawn_match = re.search(r"SERVER: spawn column \((-?\d+), (-?\d+)\) surface (-?\d+)", combined_log)
    require(spawn_match is not None, "headless_server: missing 'SERVER: spawn column' line in server.log")
    spawn_x, spawn_z, spawn_surface = (int(value) for value in spawn_match.groups())
    active = expected_active(spawn_surface + 0.5, render_distance)
    expected_summary = (
        f"SERVER: summary ticks=100 seed={EXPECTED_SEED} settled=true "
        f"active={active} generated={active} "
        f"generating=0 meshing=0 pos=({spawn_x:.1f}, {spawn_surface + 0.5:.1f}, {spawn_z:.1f}) "
        f"vel=(0.0, 0.0, 0.0) grounded=true"
    )
    require(
        expected_summary in combined_log,
        f"headless_server: missing expected summary '{expected_summary}' in server.log",
    )
    return elapsed


def run_server_selftest(
    server_binary: Path,
    repo_root: Path,
    out_root: Path,
    timeout_seconds: int,
    name: str,
    flag: str,
    marker: str,
) -> tuple[float, str]:
    """Runs MineGameServer with a self-test flag and checks its PASS/FAIL lines."""
    out_dir = out_root / name
    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    cmd = [str(server_binary.resolve()), flag, "--seed", EXPECTED_SEED, "--read-only"]

    start = time.monotonic()
    completed = subprocess.run(
        cmd,
        cwd=str(repo_root),
        capture_output=True,
        text=True,
        timeout=timeout_seconds,
        check=False,
    )
    elapsed = time.monotonic() - start

    combined_log = (completed.stdout or "") + (completed.stderr or "")
    (out_dir / "server.log").write_text(combined_log, encoding="utf-8")

    if completed.returncode != 0:
        sys.stderr.write(f"\n--- {name} output ---\n{combined_log}\n")
        raise RegressionFailure(f"{name} exited with code {completed.returncode} (expected 0)")

    require(f"{marker}: FAIL" not in combined_log, f"{name}: a check reported FAIL")
    require(f"{marker}: PASS" in combined_log, f"{name}: missing '{marker}: PASS' in server.log")
    return elapsed, combined_log


def run_server_protocol_scenario(
    server_binary: Path,
    repo_root: Path,
    out_root: Path,
    timeout_seconds: int,
) -> float:
    elapsed, combined_log = run_server_selftest(
        server_binary, repo_root, out_root, timeout_seconds,
        "server_protocol", "--test-protocol", "PROTOCOL_TEST",
    )
    passed_checks = combined_log.count("PROTOCOL_TEST: ok ")
    require(
        passed_checks == EXPECTED_PROTOCOL_CHECKS,
        f"server_protocol: expected {EXPECTED_PROTOCOL_CHECKS} passing checks, got {passed_checks}",
    )
    return elapsed


def run_server_worldgen_scenario(
    server_binary: Path,
    repo_root: Path,
    out_root: Path,
    timeout_seconds: int,
) -> float:
    elapsed, combined_log = run_server_selftest(
        server_binary, repo_root, out_root, timeout_seconds,
        "server_worldgen", "--test-worldgen", "WORLDGEN_TEST",
    )
    require(
        "WORLDGEN_TEST: ok every class exercised" in combined_log,
        "server_worldgen: the class-coverage check did not run",
    )
    return elapsed


def run_scenario(
    binary: Path,
    repo_root: Path,
    out_root: Path,
    name: str,
    script_rel: str,
    verifier: Verifier,
    timeout_seconds: int,
    render_distance_arg: int | None,
    gl_tier: int | None = None,
) -> float:
    out_dir = out_root / name
    if out_dir.exists():
        shutil.rmtree(out_dir)

    script_path = repo_root / script_rel
    cmd = [
        str(binary.resolve()),
        "--script",
        str(script_path.resolve()),
        "--script-out",
        str(out_dir.resolve()),
        "--script-timeout",
        str(timeout_seconds),
        *render_distance_flag(render_distance_arg),
        *(["--gl-tier", str(gl_tier)] if gl_tier is not None else []),
    ]

    start = time.monotonic()
    completed = subprocess.run(
        cmd,
        cwd=str(repo_root),
        capture_output=True,
        text=True,
        timeout=timeout_seconds + 30,
        check=False,
    )
    elapsed = time.monotonic() - start

    run_log_path = out_dir / "run.log"
    run_log = run_log_path.read_text(encoding="utf-8") if run_log_path.is_file() else ""

    if completed.returncode != 0:
        sys.stderr.write(f"\n--- {name} stdout ---\n{completed.stdout}\n")
        sys.stderr.write(f"--- {name} stderr ---\n{completed.stderr}\n")
        sys.stderr.write(f"--- {name} run.log ---\n{run_log}\n")
        raise RegressionFailure(
            f"scenario '{name}' exited with code {completed.returncode} (expected 0)"
        )

    require(
        "face visibility self-test passed" in completed.stderr,
        f"{name}: the engine log does not report a passing face visibility self-test",
    )
    require(run_log_path.is_file(), f"{name}: missing {run_log_path}")
    require("OK: " in run_log, f"{name}: run.log did not record an 'OK:' completion line")
    require("exit 0 " in run_log, f"{name}: run.log did not record 'exit 0'")

    verifier(out_dir, run_log, effective_render_distance(render_distance_arg))
    return elapsed


def main() -> int:
    repo_root = Path(__file__).resolve().parent.parent

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--binary",
        type=Path,
        default=None,
        help="Path to the compiled MineGame executable (auto-detected if omitted).",
    )
    parser.add_argument(
        "--out-root",
        type=Path,
        default=repo_root / "automation_out",
        help="Root directory for per-scenario output folders.",
    )
    parser.add_argument(
        "--timeout",
        type=int,
        default=180,
        help="Per-script timeout in seconds passed to --script-timeout.",
    )
    parser.add_argument(
        "--render-distance",
        type=int,
        default=None,
        help=(
            f"Render distance in [{MIN_RENDER_DISTANCE}, {MAX_RENDER_DISTANCE}] forwarded to MineGame and "
            f"MineGameServer. Omit to check the engine default ({DEFAULT_RENDER_DISTANCE})."
        ),
    )
    golden = parser.add_mutually_exclusive_group()
    golden.add_argument(
        "--golden-capture",
        type=Path,
        default=None,
        metavar="DIR",
        help=(
            "Store every scenario's screenshots as the golden suite under DIR/r<render-distance>/, "
            "classifying images that differ between reference runs as unstable."
        ),
    )
    golden.add_argument(
        "--golden",
        type=Path,
        default=None,
        metavar="DIR",
        help="Compare every scenario's screenshots against the golden suite under DIR/r<render-distance>/.",
    )
    parser.add_argument(
        "--golden-runs",
        type=int,
        default=5,
        help="Reference runs per scenario during --golden-capture (at least 2).",
    )
    parser.add_argument(
        "--gl-tier",
        type=int,
        choices=(0, 1, 2),
        default=None,
        help="Render tier cap forwarded to MineGame as --gl-tier. Omit to let the engine choose.",
    )
    args = parser.parse_args()

    if args.golden_runs < 2:
        parser.error(f"--golden-runs must be at least 2, got {args.golden_runs}")

    if args.render_distance is not None and not (
        MIN_RENDER_DISTANCE <= args.render_distance <= MAX_RENDER_DISTANCE
    ):
        parser.error(
            f"--render-distance must be in [{MIN_RENDER_DISTANCE}, {MAX_RENDER_DISTANCE}], "
            f"got {args.render_distance}"
        )

    try:
        self_check_loader_mirror()
    except RegressionFailure as exc:
        print(f"[FAIL] {exc}", file=sys.stderr)
        return 1

    try:
        binary = args.binary.resolve() if args.binary else find_default_binary(repo_root)
        server_binary = find_server_binary(binary, repo_root)
    except FileNotFoundError as exc:
        print(f"[FAIL] {exc}", file=sys.stderr)
        return 1

    if not binary.is_file():
        print(f"[FAIL] binary not found: {binary}", file=sys.stderr)
        return 1

    out_root = args.out_root.resolve()
    out_root.mkdir(parents=True, exist_ok=True)

    render_distance = effective_render_distance(args.render_distance)
    print(f"Running regression suite with binary: {binary}")
    print(
        f"Render distance {render_distance} "
        f"({'forwarded' if args.render_distance is not None else 'engine default'}); "
        f"loader mirror self-check passed"
    )
    golden_capture: GoldenCapture | None = None
    golden_compare: GoldenComparison | None = None
    try:
        if args.golden_capture is not None:
            golden_capture = GoldenCapture(
                args.golden_capture.resolve(), render_distance, binary, args.golden_runs
            )
            print(f"Capturing golden suite into {golden_capture.dir} ({args.golden_runs} runs per scenario)")
        elif args.golden is not None:
            golden_compare = GoldenComparison(args.golden.resolve(), render_distance)
            print(f"Comparing against golden suite {golden_compare.dir}")
    except RegressionFailure as exc:
        print(f"[FAIL] {exc}", file=sys.stderr)
        return 1
    if args.gl_tier is not None:
        print(f"Render tier capped at {args.gl_tier}")
    total_start = time.monotonic()

    sys.stdout.write(f"  -> {'headless_server':<16} ({server_binary.name} --ticks 100) ... ")
    sys.stdout.flush()
    try:
        server_elapsed = run_headless_server_scenario(
            server_binary=server_binary,
            repo_root=repo_root,
            out_root=out_root,
            timeout_seconds=args.timeout,
            render_distance_arg=args.render_distance,
        )
        print(f"PASS ({server_elapsed:.2f}s)")
    except Exception as exc:  # noqa: BLE001
        print("FAIL")
        print(f"[FAIL] headless_server: {exc}", file=sys.stderr)
        return 1

    sys.stdout.write(f"  -> {'server_protocol':<16} ({server_binary.name} --test-protocol) ... ")
    sys.stdout.flush()
    try:
        protocol_elapsed = run_server_protocol_scenario(
            server_binary=server_binary,
            repo_root=repo_root,
            out_root=out_root,
            timeout_seconds=args.timeout,
        )
        print(f"PASS ({protocol_elapsed:.2f}s)")
    except Exception as exc:  # noqa: BLE001
        print("FAIL")
        print(f"[FAIL] server_protocol: {exc}", file=sys.stderr)
        return 1

    sys.stdout.write(f"  -> {'server_worldgen':<16} ({server_binary.name} --test-worldgen) ... ")
    sys.stdout.flush()
    try:
        worldgen_elapsed = run_server_worldgen_scenario(
            server_binary=server_binary,
            repo_root=repo_root,
            out_root=out_root,
            timeout_seconds=args.timeout,
        )
        print(f"PASS ({worldgen_elapsed:.2f}s)")
    except Exception as exc:  # noqa: BLE001
        print("FAIL")
        print(f"[FAIL] server_worldgen: {exc}", file=sys.stderr)
        return 1

    for name, script_rel, verifier, min_timeout in SCENARIOS:
        sys.stdout.write(f"  -> {name:<16} ({script_rel}) ... ")
        sys.stdout.flush()
        try:
            elapsed = run_scenario(
                binary=binary,
                repo_root=repo_root,
                out_root=out_root,
                name=name,
                script_rel=script_rel,
                verifier=verifier,
                timeout_seconds=max(args.timeout, min_timeout),
                render_distance_arg=args.render_distance,
                gl_tier=args.gl_tier,
            )
            note = ""
            if golden_capture is not None:
                run_dirs = [out_root / name]
                for run in range(1, args.golden_runs):
                    rerun_root = out_root / "_golden_runs" / f"run{run}"
                    # The engine creates only the leaf output directory.
                    rerun_root.mkdir(parents=True, exist_ok=True)
                    elapsed += run_scenario(
                        binary=binary,
                        repo_root=repo_root,
                        out_root=rerun_root,
                        name=name,
                        script_rel=script_rel,
                        verifier=verifier,
                        timeout_seconds=max(args.timeout, min_timeout),
                        render_distance_arg=args.render_distance,
                        gl_tier=args.gl_tier,
                    )
                    run_dirs.append(rerun_root / name)
                images, variants, unstable = golden_capture.record(name, run_dirs)
                note = f", golden {images} captured, {variants} extra meshing variants, {unstable} unstable"
            elif golden_compare is not None:
                images, matched, fallback = golden_compare.compare(name, out_root / name)
                note = f", golden {images} identical ({matched} by meshing variant, {fallback} fallback)"
            print(f"PASS ({elapsed:.2f}s{note})")
        except Exception as exc:  # noqa: BLE001
            print("FAIL")
            print(f"[FAIL] {name}: {exc}", file=sys.stderr)
            return 1

    if golden_capture is not None:
        manifest_path = golden_capture.write()
        print(f"Golden manifest written to {manifest_path}")

    total_elapsed = time.monotonic() - total_start
    print(f"All {len(SCENARIOS) + 3} regression scenarios passed in {total_elapsed:.2f}s.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
