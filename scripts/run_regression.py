#!/usr/bin/env python3
"""Automated end-to-end regression suite runner for MineGame."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time
from typing import Any, Callable

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
EXPECTED_WIDTH = 1280
EXPECTED_HEIGHT = 720
EXPECTED_SEED = "16412814131703521744"
# Number of "PROTOCOL_TEST: ok" lines MineGameServer --test-protocol logs; a
# mismatch means a check was silently skipped or added without updating this.
EXPECTED_PROTOCOL_CHECKS = 36
EXPECTED_ACTIVE_CHUNKS = 1331

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
        return json.load(handle)


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


# Frustum-culled chunk counts per camera pose. They depend on the generated
# terrain, so they are recorded per generator version (worldgen.generatorVersion
# in the dumps); a version with no entry fails instead of comparing against
# another generator's numbers.
EXPECTED_RENDERED: dict[int, dict[str, int]] = {
    # Only the current generator's counts are kept: every terrain change
    # replaces them, and an older version's numbers could only mislead.
    4: {
        "spawn": 357,
        "far": 33,
        "after_ticks": 33,
        "landed": 239,
        "after_walk": 334,
        "after_break": 313,
        "after_place": 303,
        "after_jump": 315,
    },
}


def expected_rendered(state: dict[str, Any], pose: str) -> int:
    version = state.get("worldgen", {}).get("generatorVersion")
    table = EXPECTED_RENDERED.get(version)
    require(
        table is not None,
        f"no expected rendered chunk counts recorded for generator version {version}",
    )
    return table[pose]


def assert_settled_chunks(state: dict[str, Any], label: str, expected_rendered: int) -> None:
    require(state.get("settled") is True, f"{label}: expected settled == true")
    require(state.get("renderDistance") == 5, f"{label}: expected renderDistance == 5")
    chunks = state.get("chunks", {})
    require(
        chunks.get("active") == EXPECTED_ACTIVE_CHUNKS,
        f"{label}: expected chunks.active == {EXPECTED_ACTIVE_CHUNKS}, got {chunks.get('active')}",
    )
    require(
        chunks.get("generated") == EXPECTED_ACTIVE_CHUNKS,
        f"{label}: expected chunks.generated == {EXPECTED_ACTIVE_CHUNKS}, got {chunks.get('generated')}",
    )
    require(chunks.get("generating") == 0, f"{label}: expected chunks.generating == 0")
    require(chunks.get("dirty") == 0, f"{label}: expected chunks.dirty == 0")
    require(chunks.get("meshing") == 0, f"{label}: expected chunks.meshing == 0")
    require(chunks.get("meshUploadPending") is False, f"{label}: expected meshUploadPending == false")
    require(
        chunks.get("rendered") == expected_rendered,
        f"{label}: expected chunks.rendered == {expected_rendered}, got {chunks.get('rendered')}",
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


def verify_smoke(out_dir: Path, run_log: str) -> None:
    require("Noclip activated" in run_log, "smoke: missing 'Noclip activated' in run.log")
    require("Tp to X:0.0 Y:80.0 Z:0.0" in run_log, "smoke: missing teleport confirmation in run.log")
    require("X:0.0 Y:80.0 Z:0.0" in run_log, "smoke: missing /pos output in run.log")
    require(
        f"World Seed: {EXPECTED_SEED}" in run_log,
        f"smoke: expected 'World Seed: {EXPECTED_SEED}' in run.log",
    )


def verify_spawn_overview(out_dir: Path, _run_log: str) -> None:
    normal_png = assert_png(out_dir / "spawn.png")
    wireframe_png = assert_png(out_dir / "spawn_wireframe.png")
    require(
        normal_png != wireframe_png,
        "spawn_overview: spawn.png and spawn_wireframe.png are byte-identical",
    )

    spawn = load_json(out_dir / "spawn.json")
    assert_settled_chunks(spawn, "spawn.json", expected_rendered=expected_rendered(spawn, "spawn"))
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


def verify_loaded(out_dir: Path, _run_log: str) -> None:
    assert_png(out_dir / "far.png")

    far = load_json(out_dir / "far.json")
    after_ticks = load_json(out_dir / "after_ticks.json")

    assert_settled_chunks(far, "far.json", expected_rendered=expected_rendered(far, "far"))
    assert_settled_chunks(after_ticks, "after_ticks.json", expected_rendered=expected_rendered(after_ticks, "after_ticks"))
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


def verify_interact(out_dir: Path, _run_log: str) -> None:
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
    assert_settled_chunks(landed, "landed.json", expected_rendered=expected_rendered(landed, "landed"))
    assert_idle_profiler(landed, "landed.json")
    require(landed["player"]["grounded"] is True, "landed.json: expected player.grounded == true")
    require(landed["player"]["noclip"] is False, "landed.json: expected player.noclip == false")
    assert_vec3(landed["player"]["position"], sx, surface + 0.5, sz, "landed.json player.position")
    assert_vec3(landed["player"]["velocity"], 0.0, 0.0, 0.0, "landed.json player.velocity", tol=1e-4)
    require(landed["targetBlock"]["hit"] is True, "landed.json: expected targetBlock.hit == true")

    # 2. Walking forward along +Z with real collision.
    assert_settled_chunks(after_walk, "after_walk.json", expected_rendered=expected_rendered(after_walk, "after_walk"))
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

    assert_settled_chunks(after_break, "after_break.json", expected_rendered=expected_rendered(after_break, "after_break"))
    require(after_break["targetBlock"]["hit"] is True, "after_break.json: expected targetBlock.hit == true")
    assert_vec3(after_break["targetBlock"]["position"], bx, by - 1.0, bz, "after_break.json targetBlock.position")
    require(
        after_break["targetBlock"]["blockId"] != 0,
        "after_break.json: expected a solid block under the broken one",
    )

    # 4. Selecting hotbar slot 3 (index 2, Stone) and placing it where the broken
    #    block was.
    assert_settled_chunks(after_place, "after_place.json", expected_rendered=expected_rendered(after_place, "after_place"))
    require(
        after_place["player"]["hotbarSlot"] == 2,
        f"after_place.json: expected hotbarSlot == 2, got {after_place['player']['hotbarSlot']}",
    )
    require(after_place["targetBlock"]["hit"] is True, "after_place.json: expected targetBlock.hit == true")
    assert_vec3(after_place["targetBlock"]["position"], bx, by, bz, "after_place.json targetBlock.position")
    require(
        after_place["targetBlock"]["blockId"] == 3,
        f"after_place.json: expected Stone (3), got {after_place['targetBlock']['blockId']}",
    )

    # 5. Jumping leaves the ground with positive vertical velocity.
    assert_settled_chunks(after_jump, "after_jump.json", expected_rendered=expected_rendered(after_jump, "after_jump"))
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
WATER_BLOCK_ID = 5


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


def verify_inventory(out_dir: Path, _run_log: str) -> None:
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
    # Water is hidden from the tabs but already in the default hotbar; an edit
    # to another slot must not drop it.
    require(hotbar[4] == WATER_BLOCK_ID, f"after_put.json: expected water kept in slot 5, got {hotbar[4]}")
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


def verify_worldgen(out_dir: Path, _run_log: str) -> None:
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

    state = load_json(out_dir / "worldgen.json")
    classes = state["worldgen"]["cubeClasses"]
    for cube_class in ("air", "solid", "mixed"):
        require(
            classes[cube_class] > 0,
            f"worldgen.json: expected some {cube_class.upper()} cubes, got {classes[cube_class]}",
        )
    cache = state["worldgen"]["columnCache"]
    require(cache["hits"] > 0, "worldgen.json: the column cache recorded no hits")


SCENARIOS: tuple[tuple[str, str, Callable[[Path, str], None]], ...] = (
    ("smoke", "scripts/smoke.mgs", verify_smoke),
    ("spawn_overview", "scripts/spawn_overview.mgs", verify_spawn_overview),
    ("loaded", "scripts/loaded.mgs", verify_loaded),
    ("interact", "scripts/interact.mgs", verify_interact),
    ("inventory", "scripts/inventory.mgs", verify_inventory),
    ("worldgen", "scripts/worldgen.mgs", verify_worldgen),
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
    ]

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
    expected_summary = (
        f"SERVER: summary ticks=100 seed={EXPECTED_SEED} settled=true "
        f"active={EXPECTED_ACTIVE_CHUNKS} generated={EXPECTED_ACTIVE_CHUNKS} "
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
    verifier: Callable[[Path, str], None],
    timeout_seconds: int,
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

    require(run_log_path.is_file(), f"{name}: missing {run_log_path}")
    require("OK: " in run_log, f"{name}: run.log did not record an 'OK:' completion line")
    require("exit 0 " in run_log, f"{name}: run.log did not record 'exit 0'")

    verifier(out_dir, run_log)
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
    args = parser.parse_args()

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

    print(f"Running regression suite with binary: {binary}")
    total_start = time.monotonic()

    sys.stdout.write(f"  -> {'headless_server':<16} ({server_binary.name} --ticks 100) ... ")
    sys.stdout.flush()
    try:
        server_elapsed = run_headless_server_scenario(
            server_binary=server_binary,
            repo_root=repo_root,
            out_root=out_root,
            timeout_seconds=args.timeout,
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

    for name, script_rel, verifier in SCENARIOS:
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
                timeout_seconds=args.timeout,
            )
            print(f"PASS ({elapsed:.2f}s)")
        except Exception as exc:  # noqa: BLE001
            print("FAIL")
            print(f"[FAIL] {name}: {exc}", file=sys.stderr)
            return 1

    total_elapsed = time.monotonic() - total_start
    print(f"All {len(SCENARIOS) + 3} regression scenarios passed in {total_elapsed:.2f}s.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
