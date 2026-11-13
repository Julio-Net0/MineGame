#!/usr/bin/env python3
"""Render tier benchmark: runs scripts/bench_tier.mgs once per render tier on
this machine and writes a comparison table.

    python scripts/bench_tiers.py
    python scripts/bench_tiers.py --render-distance 8,16,32 --binary path/to/MineGame.exe

Each run gets its own folder under --out-root (r<distance>/tier_<tier>) with the
screenshots, state dumps, profiler captures and engine log. report.md and
summary.json in --out-root hold the comparison: per-mesh upload cost while
loading, and per-frame costs on the settled surface and underground scenes,
plus whether every tier drew the same pixels as the first one.

Exit code: 0 when every run finished and all tiers drew identical pixels, 1
when a run failed, 2 when the runs finished but a tier drew different pixels.
"""

from __future__ import annotations

import argparse
import csv
import json
import platform
import shutil
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path
from typing import Any

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))

from run_regression import differing_pixels, find_default_binary  # noqa: E402

SCRIPT = Path("scripts") / "bench_tier.mgs"
CAPTURES = ("load", "surface", "underground")
SCENES = ("surface", "underground")
DEFAULT_VARIANTS = ("default", "1", "0")


def parse_distances(text: str) -> list[int]:
    try:
        distances = [int(v) for v in text.split(",") if v.strip()]
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"render distances must be integers: {text}") from exc
    if not distances or any(d < 2 or d > 32 for d in distances):
        raise argparse.ArgumentTypeError("render distances must be in [2, 32]")
    return distances


def parse_variants(text: str) -> list[str]:
    variants = [v.strip() for v in text.split(",") if v.strip()]
    for variant in variants:
        if variant not in ("default", "0", "1", "2"):
            raise argparse.ArgumentTypeError(f"unknown tier '{variant}' (use default, 0, 1 or 2)")
    return variants


def read_csv(path: Path) -> list[dict[str, int]]:
    with path.open(newline="", encoding="utf-8") as handle:
        return [{k: int(v) for k, v in row.items()} for row in csv.DictReader(handle)]


def mean(values: list[int]) -> float:
    return sum(values) / len(values) if values else 0.0


def one_percent_low(values: list[int]) -> float:
    """Mean of the slowest 1% of frames, as the F3 overlay reports it."""
    if not values:
        return 0.0
    worst = sorted(values, reverse=True)
    return mean(worst[: max(1, len(worst) // 100)])


def summarise_load(rows: list[dict[str, int]]) -> dict[str, Any]:
    uploads = sum(r["Mesh upload n"] for r in rows)
    upload_us = sum(r["Mesh upload us"] for r in rows)
    return {
        "frames": len(rows),
        "seconds": sum(r["frame us"] for r in rows) / 1e6,
        "meshUploads": uploads,
        "usPerUpload": upload_us / uploads if uploads else 0.0,
        "worstUploadFrameUs": max((r["Mesh upload us"] for r in rows), default=0),
        "worstFrameUs": max((r["frame us"] for r in rows), default=0),
    }


def summarise_scene(rows: list[dict[str, int]]) -> dict[str, Any]:
    # The first row of a capture measures from the previous frame boundary,
    # which may predate the capture; it is dropped.
    rows = rows[1:] if len(rows) > 1 else rows
    frames = [r["frame us"] for r in rows]
    avg_frame = mean(frames)
    return {
        "frames": len(rows),
        "fps": 1e6 / avg_frame if avg_frame else 0.0,
        "frameUs": avg_frame,
        "frameOnePercentLowUs": one_percent_low(frames),
        "chunkSubmitUs": mean([r["Chunk submit us"] for r in rows]),
        "worldDrawUs": mean([r["World draw us"] for r in rows]),
        "presentUs": mean([r["Present us"] for r in rows]),
    }


def run_variant(binary: Path, repo: Path, out_dir: Path, variant: str,
                render_distance: int, timeout: int) -> dict[str, Any]:
    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.parent.mkdir(parents=True, exist_ok=True)
    exe_dir = binary.parent
    before = set(exe_dir.glob("profile_*.csv"))

    cmd = [
        str(binary), "--script", str((repo / SCRIPT).resolve()),
        "--script-out", str(out_dir.resolve()),
        "--script-timeout", str(timeout),
        "--render-distance", str(render_distance),
    ]
    if variant != "default":
        cmd += ["--gl-tier", variant]

    start = time.monotonic()
    completed = subprocess.run(cmd, cwd=str(repo), capture_output=True, text=True,
                               timeout=timeout + 60, check=False)
    elapsed = time.monotonic() - start
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "engine.log").write_text(completed.stdout + completed.stderr, encoding="utf-8")

    # Capture files are numbered by the lowest free index, so within one run
    # they sort in the order the script started them.
    new_csvs = sorted(set(exe_dir.glob("profile_*.csv")) - before)
    for name, src in zip(CAPTURES, new_csvs):
        shutil.move(str(src), str(out_dir / f"{name}.csv"))

    result: dict[str, Any] = {"variant": variant, "exitCode": completed.returncode,
                              "seconds": elapsed, "errors": []}
    if completed.returncode != 0:
        result["errors"].append(f"exit code {completed.returncode} (see {out_dir / 'engine.log'})")
    if len(new_csvs) != len(CAPTURES):
        result["errors"].append(f"expected {len(CAPTURES)} profiler captures, found {len(new_csvs)}")
        return result

    result["load"] = summarise_load(read_csv(out_dir / "load.csv"))
    for scene in SCENES:
        summary = summarise_scene(read_csv(out_dir / f"{scene}.csv"))
        state_path = out_dir / f"{scene}.json"
        if state_path.is_file():
            state = json.loads(state_path.read_text(encoding="utf-8"))
            render = state.get("render", {})
            arena = render.get("arena", {})
            summary["renderedChunks"] = state.get("chunks", {}).get("rendered")
            summary["arenaBlocks"] = arena.get("blocks")
            summary["arenaUsedMiB"] = arena.get("usedPages", 0) * 2048 / 2**20
            summary["arenaSlackMiB"] = arena.get("slackBytes", 0) / 2**20
            result["tier"] = render.get("tier")
            result["glRenderer"] = render.get("glRenderer", "")
            result["glVersion"] = render.get("glVersion", "")
        result[scene] = summary
    return result


def compare_images(out_root: Path, results: list[dict[str, Any]]) -> list[str]:
    """Every tier must draw what the first variant drew; returns the differences."""
    problems = []
    ok = [r for r in results if not r["errors"]]
    if len(ok) < 2:
        return problems
    reference = ok[0]["variant"]
    for result in ok[1:]:
        for scene in SCENES:
            a = out_root / f"tier_{reference}" / f"{scene}.png"
            b = out_root / f"tier_{result['variant']}" / f"{scene}.png"
            if not (a.is_file() and b.is_file()):
                problems.append(f"{scene}: missing screenshot for tier {reference} or {result['variant']}")
                continue
            diff = differing_pixels(a, b)
            result.setdefault("pixelDiff", {})[scene] = len(diff)
            if diff:
                problems.append(f"{scene}: tier {result['variant']} differs from tier {reference} in {len(diff)} pixel(s)")
    return problems


def label(result: dict[str, Any]) -> str:
    if result["variant"] == "default":
        return f"default (tier {result.get('tier', '?')})"
    return f"--gl-tier {result['variant']}"


def build_header(results: list[dict[str, Any]], binary: Path) -> list[str]:
    ok = [r for r in results if not r["errors"]]
    renderer = ok[0].get("glRenderer", "?") if ok else "?"
    version = ok[0].get("glVersion", "?") if ok else "?"
    return [
        "# Render tier benchmark",
        "",
        f"- Date: {datetime.now().isoformat(timespec='seconds')}",
        f"- CPU: {platform.processor() or platform.machine()} ({platform.system()} {platform.release()})",
        f"- GPU: {renderer}",
        f"- OpenGL: {version}",
        f"- Binary: `{binary}`",
        "",
    ]


def build_section(results: list[dict[str, Any]], problems: list[str], distance: int) -> str:
    ok = [r for r in results if not r["errors"]]
    lines = [
        f"## Render distance {distance}",
        "",
        "### Loading (spawn, then the first teleport)",
        "",
        "| run | tier | mesh uploads | us per upload | worst upload frame us | worst frame us | load s |",
        "|---|---|---|---|---|---|---|",
    ]
    for r in ok:
        load = r["load"]
        lines.append(f"| {label(r)} | {r.get('tier', '?')} | {load['meshUploads']} | {load['usPerUpload']:.2f} | "
                     f"{load['worstUploadFrameUs']} | {load['worstFrameUs']} | {load['seconds']:.2f} |")
    for scene in SCENES:
        lines += [
            "",
            f"### Settled {scene} (10 s capture)",
            "",
            "| run | frames | FPS | frame us | 1% low us | Chunk submit us | World draw us | Present us | rendered chunks | arena (blocks / used / slack MiB) |",
            "|---|---|---|---|---|---|---|---|---|---|",
        ]
        for r in ok:
            s = r[scene]
            arena = (f"{s.get('arenaBlocks', '?')} / {s.get('arenaUsedMiB', 0):.0f} / {s.get('arenaSlackMiB', 0):.1f}")
            lines.append(f"| {label(r)} | {s['frames']} | {s['fps']:.0f} | {s['frameUs']:.0f} | "
                         f"{s['frameOnePercentLowUs']:.0f} | {s['chunkSubmitUs']:.0f} | {s['worldDrawUs']:.0f} | "
                         f"{s['presentUs']:.0f} | {s.get('renderedChunks', '?')} | {arena} |")
    lines += ["", "### Checks", ""]
    for r in results:
        for error in r["errors"]:
            lines.append(f"- FAIL {label(r)}: {error}")
    lines += [f"- FAIL {p}" for p in problems]
    if ok and not problems and all(not r["errors"] for r in results):
        lines.append(f"- OK: every run finished and drew the same pixels as `{label(ok[0])}`.")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    repo = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", type=Path, default=None,
                        help="MineGame executable (auto-detected like run_regression.py if omitted).")
    parser.add_argument("--render-distance", type=parse_distances, default=[8, 16],
                        help="Comma-separated render distances to benchmark (default 8,16).")
    parser.add_argument("--tiers", type=parse_variants, default=list(DEFAULT_VARIANTS),
                        help="Comma-separated runs: default (driver's choice), 0, 1, 2. Default: default,1,0.")
    parser.add_argument("--out-root", type=Path, default=repo / "automation_out" / "bench",
                        help="Folder for the per-run output and the report.")
    parser.add_argument("--timeout", type=int, default=600, help="Per-run script timeout in seconds.")
    args = parser.parse_args()

    try:
        binary = (args.binary or find_default_binary(repo)).resolve()
    except FileNotFoundError as exc:
        print(f"[FAIL] {exc}", file=sys.stderr)
        return 1
    out_root = args.out_root.resolve()
    out_root.mkdir(parents=True, exist_ok=True)

    print(f"Benchmarking {binary}")
    all_results: dict[int, list[dict[str, Any]]] = {}
    sections = []
    any_failed = False
    any_differs = False
    for distance in args.render_distance:
        distance_root = out_root / f"r{distance}"
        results = []
        for variant in args.tiers:
            sys.stdout.write(f"  -> R={distance:<2} tier {variant:<8} ... ")
            sys.stdout.flush()
            result = run_variant(binary, repo, distance_root / f"tier_{variant}", variant,
                                 distance, args.timeout)
            results.append(result)
            print("FAIL: " + "; ".join(result["errors"]) if result["errors"]
                  else f"ok ({result['seconds']:.1f}s, tier {result.get('tier', '?')})")
        problems = compare_images(distance_root, results)
        any_failed = any_failed or any(r["errors"] for r in results)
        any_differs = any_differs or bool(problems)
        all_results[distance] = results
        sections.append(build_section(results, problems, distance))

    first = next(iter(all_results.values()), [])
    report = "\n".join(build_header(first, binary)) + "\n" + "\n\n".join(sections)
    (out_root / "report.md").write_text(report, encoding="utf-8")
    (out_root / "summary.json").write_text(
        json.dumps({str(d): r for d, r in all_results.items()}, indent=2) + "\n", encoding="utf-8")
    print()
    print(report)
    print(f"Report written to {out_root / 'report.md'}")

    if any_failed:
        return 1
    return 2 if any_differs else 0


if __name__ == "__main__":
    sys.exit(main())
