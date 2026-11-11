#!/usr/bin/env python3
"""Explain frame drops recorded by `Launch.cmd frame-drops`.

Reads the run folder (frames.csv from --frame-timeline plus game.log) and
prints which stage, thread or resource made each slow frame late:

    python tools/analyze_frame_drops.py build_native/run/frame-drops-1234-5678
    python tools/analyze_frame_drops.py path/to/frames.csv --log path/to/game.log

Standard library only. Times are host wall-clock milliseconds.
"""
from __future__ import annotations

import argparse
import csv
import re
import statistics
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path

# ---------------------------------------------------------------- inputs

NUMERIC_INT = {"frame", "target_fps", "vsync", "world", "loops", "present_not_ok", "markers", "report",
               "page_faults", "engine_frame", "engine_parts", "engine_commands", "engine_world_draws",
               "io_reads", "draws", "image_uploads", "geometry_uploads", "surface_creates", "new_vs",
               "vs_compiles", "upload_deferred", "stall_serial"}


def read_timeline(path: Path) -> tuple[dict[str, str], list[dict[str, float]]]:
    meta: dict[str, str] = {}
    body: list[str] = []
    with path.open(newline="", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if line.startswith("#"):
                for key, value in re.findall(r"(\w+)=([^\s]+)", line):
                    meta.setdefault(key, value)
                continue
            if line.strip():
                body.append(line)
    rows: list[dict[str, float]] = []
    for raw in csv.DictReader(body):
        # A crash can leave a truncated final row: skip anything incomplete.
        if None in raw or any(field is None or field == "" for field in raw.values()):
            continue
        try:
            row = {key: int(field) if key in NUMERIC_INT else float(field) for key, field in raw.items()}
        except ValueError:
            continue
        rows.append(row)
    return meta, rows


@dataclass
class StallFrame:
    serial: int
    stream: str
    total_ms: float
    end_qpc: int | None
    groups: dict[str, dict[str, float]] = field(default_factory=dict)
    contributors: list[dict[str, str]] = field(default_factory=list)


@dataclass
class LogData:
    stalls: dict[tuple[str, int], list[StallFrame]] = field(default_factory=lambda: defaultdict(list))
    markers: list[float] = field(default_factory=list)
    report_costs: list[str] = field(default_factory=list)
    shader_compiles: list[str] = field(default_factory=list)
    texture_budget: list[str] = field(default_factory=list)
    oversized_queue_frames: int = 0
    problems: list[str] = field(default_factory=list)
    display: str = ""
    presentation: list[str] = field(default_factory=list)
    stall_enabled: bool = False


STALL_HEADER = re.compile(r"\[STALL\] frame=(\d+) stream=(\S+) thread=\d+ total_ms=([\d.]+).*?(?:end_qpc=(\d+))?$")
STALL_GROUP = re.compile(r"\[STALL\] frame=(\d+) stream=(\S+) contributors=(\S+) \(exclusive wall time[^)]*\)(.*)$")
# Function and object names may contain spaces; the field order is fixed.
CONTRIBUTOR = re.compile(
    r"\[(STALL|WAIT)\] contributor thread=(?P<thread>\d+) guest_pc=(?P<guest_pc>\S+) caller=(?P<caller>\S+) "
    r"function=(?P<function>.*?) section=(?P<section>\S+) duration_ms=(?P<duration_ms>[\d.]+) "
    r"self_ms=(?P<self_ms>[\d.]+) object_kind=(?P<object_kind>.*?) object=(?P<object>\S+) pending=(?P<pending>\d+)"
    r" frame=(?P<frame>\d+) stream=(?P<stream>\S+) contributor_group=(?P<contributor_group>\S+) "
    r"overlap_ms=(?P<overlap_ms>[\d.]+)")


def read_log(path: Path | None) -> LogData:
    data = LogData()
    if not path or not path.is_file():
        return data
    current: dict[tuple[str, int], StallFrame] = {}
    with path.open(encoding="utf-8", errors="replace") as handle:
        for line in handle:
            line = line.rstrip("\r\n")
            if "[StallProfiler] enabled" in line:
                data.stall_enabled = True
            if match := STALL_HEADER.search(line):
                serial, stream = int(match[1]), match[2]
                frame = StallFrame(serial, stream, float(match[3]), int(match[4]) if match[4] else None)
                data.stalls[(stream, serial)].append(frame)
                current[(stream, serial)] = frame
                continue
            if match := STALL_GROUP.search(line):
                frame = current.get((match[2], int(match[1])))
                if frame:
                    frame.groups[match[3]] = {k: float(v) for k, v in re.findall(r"(\w+)=([\d.]+)ms", match[4])}
                continue
            if match := CONTRIBUTOR.search(line):
                fields = match.groupdict()
                frame = current.get((fields["stream"], int(fields["frame"])))
                if frame:
                    fields["wait"] = "1" if match[1] == "WAIT" else "0"
                    frame.contributors.append(fields)
                continue
            if match := re.search(r"\[FrameTimeline\] marker=\d+ tMs=([\d.]+)", line):
                data.markers.append(float(match[1]))
            elif "[ReportCost]" in line:
                data.report_costs.append(line[line.index("[ReportCost]"):])
            elif "[RenderShaderCompile]" in line:
                data.shader_compiles.append(line[line.index("[RenderShaderCompile]"):])
            elif "[TextureBudget]" in line:
                data.texture_budget.append(line[line.index("[TextureBudget]"):])
            elif "[EngineQueueFrame]" in line:
                data.oversized_queue_frames += 1
            elif "[Display] output=" in line:
                data.display = line[line.index("[Display]"):]
            elif "[Presentation]" in line and len(data.presentation) < 6:
                data.presentation.append(line[line.index("[Presentation]"):])
            for tag in ("[ERROR]", "[STOP]", "[TIMEOUT]", "Present failed", "Resize failed", "[EnginePreview] "):
                if tag in line and len(data.problems) < 20 and "[EnginePreview] Experimental" not in line:
                    data.problems.append(line.strip())
                    break
    return data


# ---------------------------------------------------------------- analysis

def budget_ms(row: dict[str, float], fallback: float) -> float:
    target = row.get("target_fps", 0)
    return 1000.0 / target if target else fallback


def slow_threshold(budget: float) -> float:
    return max(budget * 1.5, budget + 4.0)


def value(row: dict[str, float], key: str) -> float:
    number = row.get(key, 0.0)
    return float(number) if number is not None else 0.0


@dataclass
class Verdict:
    stage: str
    cause: str
    detail: str


def classify(row: dict[str, float], budget: float) -> Verdict:
    stages = {
        "engine": value(row, "take_ms"),
        "render": value(row, "render_ms") + value(row, "post_ms"),
        "present": value(row, "present_ms"),
        "pacer": value(row, "pacer_ms"),
        "tail": value(row, "tail_ms"),
        "messages": value(row, "pump_ms"),
    }
    stage = max(stages, key=stages.get)
    gpu = value(row, "gpu_ms")
    # The engine and display threads overlap. When the engine frame alone is
    # about as long as the frame interval and longer than the display thread's
    # own work, the engine sets the pace even if the display thread spent most
    # of the interval rendering the previous frame (the GPU span then tracks
    # the interval and would otherwise read as gpu-bound).
    wall = value(row, "engine_wall_ms")
    display_busy = stages["render"] + stages["present"] + stages["tail"] + stages["messages"]
    if wall > 0 and wall >= 0.85 * value(row, "interval_ms") and wall > display_busy + 1.0:
        stage = "engine"
    if stage == "engine":
        wall, cpu = value(row, "engine_wall_ms"), value(row, "engine_cpu_ms")
        io = value(row, "engine_io_ms")
        if wall > 0 and io >= 0.3 * wall:
            return Verdict(stage, "engine: file reads", f"engine thread spent {io:.1f} of {wall:.1f} ms in guest file reads "
                           f"({int(value(row, 'io_reads'))} reads, {value(row, 'io_kib'):.0f} KiB)")
        if wall > 0 and cpu > 0:
            ratio = cpu / wall
            if ratio >= 0.75:
                return Verdict(stage, "engine: CPU-bound", f"engine frame took {wall:.1f} ms with {cpu:.1f} ms CPU "
                               f"({int(value(row, 'engine_commands'))} commands, {int(value(row, 'engine_world_draws'))} world draws)")
            return Verdict(stage, "engine: blocked/waiting", f"engine frame took {wall:.1f} ms but used only {cpu:.1f} ms CPU "
                           f"(waits, locks or descheduling; see guest [STALL] contributors)")
        if wall > 0:
            return Verdict(stage, "engine-bound", f"engine frame took {wall:.1f} ms "
                           f"({int(value(row, 'engine_world_draws'))} world draws; no per-frame CPU time recorded)")
        return Verdict(stage, "engine: frame late", f"display waited {stages['engine']:.1f} ms for the engine")
    if stage == "render":
        render = stages["render"]
        uploads = (value(row, "image_ms") + value(row, "geometry_ms") + value(row, "surface_ms") +
                   value(row, "decode_ms") + value(row, "transient_ms"))
        if value(row, "vs_compiles") > 0:
            return Verdict(stage, "render: shader compile", f"{int(value(row, 'vs_compiles'))} vertex shader compile(s) "
                           f"(cache miss) in a {render:.1f} ms render")
        if value(row, "new_vs") > 0:
            return Verdict(stage, "render: shader creation", f"{int(value(row, 'new_vs'))} new vertex shader(s) loaded from cache")
        if uploads >= 0.4 * render:
            return Verdict(stage, "render: resource uploads",
                           f"{uploads:.1f} of {render:.1f} ms uploading: images {int(value(row, 'image_uploads'))}/"
                           f"{value(row, 'image_kib'):.0f} KiB {value(row, 'image_ms'):.1f} ms, geometry "
                           f"{int(value(row, 'geometry_uploads'))}/{value(row, 'geometry_kib'):.0f} KiB "
                           f"{value(row, 'geometry_ms'):.1f} ms, decode {value(row, 'decode_ms'):.1f} ms, surfaces "
                           f"{int(value(row, 'surface_creates'))} {value(row, 'surface_ms'):.1f} ms")
        cpu = value(row, "display_cpu_ms")
        busy = render + value(row, "pump_ms") + value(row, "tail_ms")
        if cpu >= 0 and busy > 16 and cpu < 0.5 * busy:
            return Verdict(stage, "render: thread stalled", f"render took {render:.1f} ms but the display thread used "
                           f"{cpu:.1f} ms CPU (blocked in driver or descheduled)")
        if gpu >= 0 and value(row, "gpu_idle_before_ms") >= 0 and value(row, "gpu_idle_before_ms") < 0.5 and gpu >= budget:
            return Verdict(stage, "gpu-bound", f"GPU span {gpu:.1f} ms with no idle gap; render calls blocked on the GPU")
        return Verdict(stage, "render: submission CPU", f"{render:.1f} ms submitting {int(value(row, 'draws'))} draws")
    if stage == "present":
        if gpu >= 0.8 * budget:
            return Verdict(stage, "gpu-bound", f"Present blocked {stages['present']:.1f} ms; GPU span {gpu:.1f} ms")
        if value(row, "vsync"):
            return Verdict(stage, "present: vsync", f"Present waited {stages['present']:.1f} ms for vertical blank")
        return Verdict(stage, "present: blocked", f"Present blocked {stages['present']:.1f} ms with GPU span "
                       f"{gpu:.1f} ms (driver/compositor queue)")
    if stage == "tail":
        if value(row, "report"):
            return Verdict(stage, "host: 5 s report", f"{stages['tail']:.1f} ms after Present in the periodic log report")
        if value(row, "image_uploads") > 0:
            return Verdict(stage, "host: texture preload", f"{stages['tail']:.1f} ms after Present; "
                           f"{int(value(row, 'image_uploads'))} image upload(s) {value(row, 'image_ms'):.1f} ms")
        return Verdict(stage, "host: post-present work", f"{stages['tail']:.1f} ms between Present and the next loop")
    if stage == "pacer":
        return Verdict(stage, "frame limiter", f"frame limiter slept {stages['pacer']:.1f} ms (timer wake-up late)")
    return Verdict(stage, "window messages", f"{stages['messages']:.1f} ms handling window messages/settings saves")


def percentile(sorted_values: list[float], fraction: float) -> float:
    if not sorted_values:
        return 0.0
    return sorted_values[int((len(sorted_values) - 1) * fraction)]


def qpc_to_ms(meta: dict[str, str], qpc: int | None) -> float | None:
    try:
        origin, frequency = int(meta["origin_qpc"]), int(meta["qpc_frequency"])
    except (KeyError, ValueError):
        return None
    if qpc is None or not frequency:
        return None
    return (qpc - origin) * 1000.0 / frequency


def guest_stall_near(log: LogData, meta: dict[str, str], when_ms: float, tolerance: float = 3.0) -> StallFrame | None:
    best, best_gap = None, tolerance
    for (stream, _), frames in log.stalls.items():
        if stream != "guest":
            continue
        for frame in frames:
            at = qpc_to_ms(meta, frame.end_qpc)
            if at is not None and abs(at - when_ms) <= best_gap:
                best, best_gap = frame, abs(at - when_ms)
    return best


def display_stall(log: LogData, serial: int) -> StallFrame | None:
    frames = log.stalls.get(("display", serial)) if serial else None
    return frames[-1] if frames else None


def describe_contributors(frame: StallFrame, limit: int = 3) -> list[str]:
    out = []
    for group in ("frame_thread", "workers"):
        totals = frame.groups.get(group)
        if totals:
            top = sorted(totals.items(), key=lambda item: -item[1])[:3]
            out.append(f"{group}: " + ", ".join(f"{name} {ms:.1f}ms" for name, ms in top if ms > 0.05))
        picks = [c for c in frame.contributors if c.get("contributor_group") == group][:limit]
        for c in picks:
            out.append(f"  {'WAIT' if c.get('wait') == '1' else 'busy'} {c.get('function', '?')} "
                       f"[{c.get('section', '?')}] overlap {c.get('overlap_ms', '?')} ms"
                       + (f" pc={c['guest_pc']}" if c.get("guest_pc", "0x00000000") != "0x00000000" else "")
                       + (f" object={c['object_kind']}" if c.get("object_kind", "unavailable") != "unavailable" else ""))
    return out


def analyze(meta: dict[str, str], rows: list[dict[str, float]], log: LogData, top: int) -> str:
    out: list[str] = []
    say = out.append
    if not rows:
        return "No frame rows were recorded. Was the game closed before gameplay started?\n"
    world = [r for r in rows if value(r, "world")]
    population = world or rows
    intervals = sorted(value(r, "interval_ms") for r in population)
    median = statistics.median(intervals)
    targets = Counter(int(value(r, "target_fps")) for r in rows)
    target = targets.most_common(1)[0][0]
    fallback = median if not target else 1000.0 / target
    duration = (value(rows[-1], "t_ms") - value(rows[0], "t_ms")) / 1000.0

    say("FRAME-DROP ANALYSIS")
    say("=" * 72)
    say(f"Run: {duration:.1f} s, {len(rows)} frames ({len(world)} gameplay). Target "
        f"{'uncapped' if not target else f'{target} FPS'}, vsync={'on' if targets and any(value(r, 'vsync') for r in rows) else 'off'}.")
    if log.display:
        say(log.display)
    gpu_values = sorted(value(r, "gpu_ms") for r in population if value(r, "gpu_ms") >= 0)
    say(f"Frame time (gameplay): median {median:.2f} ms ({1000 / median if median else 0:.1f} FPS), "
        f"p95 {percentile(intervals, .95):.2f}, p99 {percentile(intervals, .99):.2f}, max {intervals[-1]:.2f} ms.")
    if gpu_values:
        say(f"GPU span per frame: median {statistics.median(gpu_values):.2f} ms, p95 {percentile(gpu_values, .95):.2f} ms "
            f"(budget {fallback:.2f} ms).")
    cpu_values = [value(r, "engine_cpu_ms") for r in population if value(r, "engine_cpu_ms") >= 0]
    wall_values = [value(r, "engine_wall_ms") for r in population if value(r, "engine_wall_ms") > 0]
    if cpu_values and wall_values:
        say(f"Engine frame: median wall {statistics.median(wall_values):.2f} ms, CPU {statistics.median(cpu_values):.2f} ms.")
    say("")

    slow = []
    for row in population:
        budget = budget_ms(row, fallback)
        if value(row, "interval_ms") > slow_threshold(budget):
            slow.append((row, budget, classify(row, budget)))
    excess_total = sum(value(r, "interval_ms") - b for r, b, _ in slow)
    say(f"Slow frames: {len(slow)} of {len(population)} ({100.0 * len(slow) / len(population):.2f}%), "
        f"{excess_total:.0f} ms lost beyond budget. Threshold: {slow_threshold(fallback):.1f} ms.")
    if not slow:
        say("No gameplay frame exceeded the slow-frame threshold.")
        if log.markers:
            say(f"{len(log.markers)} F9 marker(s) were pressed; see the per-marker section below.")
    else:
        by_cause: dict[str, list[tuple[dict[str, float], float]]] = defaultdict(list)
        for row, budget, verdict in slow:
            by_cause[verdict.cause].append((row, budget))
        say("")
        say("Causes (ranked by time lost):")
        ranked = sorted(by_cause.items(), key=lambda item: -sum(value(r, "interval_ms") - b for r, b in item[1]))
        for cause, items in ranked:
            lost = sum(value(r, "interval_ms") - b for r, b in items)
            worst = max(value(r, "interval_ms") for r, _ in items)
            say(f"  {cause:<26} {len(items):5d} frames  {lost:8.0f} ms lost  ({100 * lost / excess_total:5.1f}%)  worst {worst:.1f} ms")
        lead, lead_items = ranked[0]
        lead_share = 100 * sum(value(r, "interval_ms") - b for r, b in lead_items) / excess_total
        say(f"Verdict: {lead} accounts for {lead_share:.0f}% of the time lost to slow frames.")

    # Bursts: slow frames within 0.5 s of each other.
    bursts: list[list[tuple[dict[str, float], float, Verdict]]] = []
    for item in slow:
        if bursts and value(item[0], "t_ms") - value(bursts[-1][-1][0], "t_ms") <= 500:
            bursts[-1].append(item)
        else:
            bursts.append([item])
    if bursts:
        say("")
        say(f"Episodes ({len(bursts)}; slow frames closer than 0.5 s are grouped):")
        biggest = sorted(bursts, key=lambda b: -sum(value(r, "interval_ms") - bud for r, bud, _ in b))[:12]
        for burst in sorted(biggest, key=lambda b: value(b[0][0], "t_ms")):
            start, end = value(burst[0][0], "t_ms") / 1000, value(burst[-1][0], "t_ms") / 1000
            causes = Counter(v.cause for _, _, v in burst).most_common(2)
            worst = max(value(r, "interval_ms") for r, _, _ in burst)
            say(f"  {start:8.2f}-{end:8.2f} s  {len(burst):4d} slow  worst {worst:7.1f} ms  "
                + ", ".join(f"{c} x{n}" for c, n in causes))

    # Periodic hitches (for example a timer or a periodic report).
    if len(slow) >= 6:
        times = [value(r, "t_ms") for r, _, _ in slow]
        gaps = [b - a for a, b in zip(times, times[1:]) if b - a > 200]
        bins = Counter(round(g / 100) for g in gaps)
        if bins:
            period, count = bins.most_common(1)[0]
            if count >= 4 and count >= 0.3 * len(gaps):
                say("")
                say(f"Periodic pattern: {count} of {len(gaps)} gaps between hitches are ~{period / 10:.1f} s.")

    # Worst frames with full detail.
    if slow:
        say("")
        say(f"Worst {min(top, len(slow))} frames:")
        for row, budget, verdict in sorted(slow, key=lambda item: -value(item[0], "interval_ms"))[:top]:
            say(f"- t={value(row, 't_ms') / 1000:8.3f} s  frame {int(value(row, 'frame'))}  "
                f"{value(row, 'interval_ms'):.1f} ms  -> {verdict.cause}")
            say(f"    {verdict.detail}")
            say(f"    stages: engine-wait {value(row, 'take_ms'):.1f}  render {value(row, 'render_ms'):.1f}  "
                f"post {value(row, 'post_ms'):.1f}  pacer {value(row, 'pacer_ms'):.1f}  present {value(row, 'present_ms'):.1f}  "
                f"tail {value(row, 'tail_ms'):.1f}  msgs {value(row, 'pump_ms'):.1f}  gpu {value(row, 'gpu_ms'):.1f}  "
                f"loops {int(value(row, 'loops'))}")
            extra = []
            if value(row, "page_faults") > 5000:
                extra.append(f"{int(value(row, 'page_faults'))} page faults")
            if value(row, "io_reads"):
                extra.append(f"file reads {int(value(row, 'io_reads'))} ({value(row, 'io_ms'):.1f} ms total, "
                             f"engine thread {value(row, 'engine_io_ms'):.1f} ms)")
            if value(row, "process_cpu_ms") > 0 and value(row, "interval_ms") > 0:
                extra.append(f"process CPU {value(row, 'process_cpu_ms') / value(row, 'interval_ms'):.1f} cores")
            if value(row, "upload_deferred"):
                extra.append(f"{int(value(row, 'upload_deferred'))} uploads deferred by budget")
            if extra:
                say("    also: " + "; ".join(extra))
            stall = display_stall(log, int(value(row, "stall_serial")))
            if stall:
                for text in describe_contributors(stall):
                    say(f"    display-thread stall report: {text}")
            if verdict.stage == "engine" and value(row, "engine_boundary_ms") > 0:
                guest = guest_stall_near(log, meta, value(row, "engine_boundary_ms"))
                if guest:
                    say(f"    engine-thread stall report ({guest.total_ms:.1f} ms):")
                    for text in describe_contributors(guest):
                        say(f"      {text}")

    # Contributors across all slow frames.
    if slow and log.stalls:
        totals: dict[tuple[str, str], float] = defaultdict(float)
        for row, _, verdict in slow:
            frames = [display_stall(log, int(value(row, "stall_serial")))]
            if verdict.stage == "engine" and value(row, "engine_boundary_ms") > 0:
                frames.append(guest_stall_near(log, meta, value(row, "engine_boundary_ms")))
            for frame in filter(None, frames):
                for c in frame.contributors:
                    try:
                        totals[(frame.stream, c.get("function", "?"))] += float(c.get("overlap_ms", 0))
                    except ValueError:
                        pass
        if totals:
            say("")
            say("Top stall-report contributors across slow frames (overlap ms):")
            for (stream, function), ms in sorted(totals.items(), key=lambda item: -item[1])[:15]:
                say(f"  {ms:9.1f}  {stream:<8} {function}")

    if log.markers:
        say("")
        say("F9 markers (worst frame in the 3 s before each press):")
        for mark in log.markers:
            window = [r for r in rows if mark - 3000 <= value(r, "t_ms") <= mark + 100]
            if not window:
                say(f"  {mark / 1000:8.2f} s  no frames recorded")
                continue
            worst = max(window, key=lambda r: value(r, "interval_ms"))
            budget = budget_ms(worst, fallback)
            if value(worst, "interval_ms") <= slow_threshold(budget):
                say(f"  {mark / 1000:8.2f} s  no slow frame; worst {value(worst, 'interval_ms'):.1f} ms "
                    f"(a felt hitch here may be uneven pacing or input, not a long frame)")
                continue
            verdict = classify(worst, budget)
            say(f"  {mark / 1000:8.2f} s  worst {value(worst, 'interval_ms'):.1f} ms at {value(worst, 't_ms') / 1000:.2f} s"
                f" -> {verdict.cause}")

    notes = []
    if log.shader_compiles:
        notes.append(f"{len(log.shader_compiles)} slow shader compile line(s), e.g. {log.shader_compiles[0]}")
    if log.oversized_queue_frames:
        notes.append(f"{log.oversized_queue_frames} oversized engine queue frame report(s) ([EngineQueueFrame])")
    if len(log.texture_budget) > 1:
        notes.append(f"texture budget changed {len(log.texture_budget) - 1} time(s): {log.texture_budget[-1]}")
    if log.report_costs:
        notes.append(f"{len(log.report_costs)} costly 5 s report(s), e.g. {log.report_costs[0]}")
    non_ok = sum(int(value(r, "present_not_ok")) for r in rows)
    if non_ok:
        notes.append(f"{non_ok} Present call(s) returned occluded/other status (window hidden or minimized?)")
    if not log.stall_enabled:
        notes.append("stall profiler output not found; thread attribution is limited to the CSV")
    if meta.get("gpu_timing") == "0":
        notes.append("GPU timestamp queries were unavailable on this device")
    if notes or log.problems:
        say("")
        say("Notes:")
        for note in notes:
            say(f"  - {note}")
        for problem in log.problems[:8]:
            say(f"  ! {problem}")
    return "\n".join(out) + "\n"


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("path", type=Path, help="frame-drops run folder, or a frames.csv file")
    parser.add_argument("--log", type=Path, help="game.log (default: beside the CSV)")
    parser.add_argument("--top", type=int, default=15, help="number of worst frames to detail")
    args = parser.parse_args(argv)
    csv_path = args.path / "frames.csv" if args.path.is_dir() else args.path
    if not csv_path.is_file():
        print(f"No frame timeline at {csv_path}", file=sys.stderr)
        return 2
    log_path = args.log or csv_path.with_name("game.log")
    meta, rows = read_timeline(csv_path)
    report = analyze(meta, rows, read_log(log_path), args.top)
    sys.stdout.write(report)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
