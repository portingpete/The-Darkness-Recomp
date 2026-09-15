"""Run the native development executable with a deadline and retain its evidence."""
import argparse
import hashlib
from datetime import datetime
import json
import math
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--game-dir", type=Path, default=root / "Darkness")
parser.add_argument("--timeout-ms", type=int, default=30000,
                    help="Diagnostic deadline in ms, 100 through 1800000 (30 minutes); 0 is rejected here, use the exe directly to disable")
parser.add_argument("--full-log", action="store_true", help="Print the entire log instead of its last 24 lines")
parser.add_argument("--trace-renderer", action="store_true", help="Capture bounded original engine render arguments")
parser.add_argument("--engine-preview", action="store_true", help="Render original text/video/color subsets and save native frame evidence")
parser.add_argument("--mute", action="store_true", help="Silence native output while audio processing continues")
parser.add_argument("--fps", type=int, default=60, help="Native frame limit, 0 uncapped (default: 60)")
parser.add_argument("--fullscreen", action="store_true", help="Borderless fullscreen on the game monitor")
parser.add_argument("--width", type=int, help="Window width / requested aspect (use with --height)")
parser.add_argument("--height", type=int, help="Window height / requested aspect (use with --width)")
parser.add_argument("--render-height", type=int, default=720, help="Internal render height, 180..2160 (default: 720)")
parser.add_argument("--fov", type=float, help="Horizontal FOV at 16:9; 0 keeps Original, otherwise 60..120")
parser.add_argument("--profile-engine", action="store_true", help="Measure inclusive CPU time at engine preparation boundaries")
parser.add_argument("--sample-engine", action="store_true", help="Sample our engine thread's native instruction addresses for profiling")
parser.add_argument("--sample-workers", action="store_true", help="Sample registered guest worker threads (diagnostic only)")
parser.add_argument("--test-start", action="store_true", help="Integration test: press Start once after the world begins")
parser.add_argument("--test-skip-intros", action="store_true", help="Integration test: send bounded Space pulses until the world begins")
parser.add_argument("--test-input", type=Path, help="Integration test: read up to 64 keyboard commands from this file "
                    "(numeric '<key> [<holdMs 1..10000>]'; bare menu keys hold 250ms, I/J/K/L hold 2000ms; "
                    "'mouse <dx> <dy>'; 'capture'; '0' inspects; an invalid line blocks later commands until fixed)")
args = parser.parse_args()
if not 100 <= args.timeout_ms <= 1800000:
    parser.error("--timeout-ms must be between 100 and 1800000")
if not 0 <= args.fps <= 1000:
    parser.error("--fps must be between 0 and 1000")
if (args.width is None) != (args.height is None):
    parser.error("--width and --height must be supplied together")
if args.width is not None and not (320 <= args.width <= 16384 and 180 <= args.height <= 16384):
    parser.error("Window width must be 320..16384 and height 180..16384")
if not 180 <= args.render_height <= 2160:
    parser.error("--render-height must be 180..2160")
if args.fov is not None and (not math.isfinite(args.fov) or (args.fov != 0 and not 60 <= args.fov <= 120)):
    parser.error("--fov must be 0 (Original) or 60..120")
directory = root / "build_native/run"
directory.mkdir(parents=True, exist_ok=True)
log = directory / (datetime.now().strftime("boot-%Y%m%d-%H%M%S-%f") + ".log")
command = [str(root / "build_native/Release/DarkRecomp.exe"), "--game-dir", str(args.game_dir.resolve()),
           "--timeout-ms", str(args.timeout_ms), "--fps", str(args.fps)]
if args.fullscreen:
    command += ["--fullscreen"]
if args.width is not None:
    command += ["--width", str(args.width), "--height", str(args.height)]
command += ["--render-height", str(args.render_height)]
if args.fov is not None:
    command += ["--fov", str(args.fov)]
if args.trace_renderer:
    command += ["--trace-renderer", str(log.with_suffix(".render"))]
if args.engine_preview:
    command += ["--engine-preview", "--preview-frame", str(log.with_suffix(".bmp"))]
if args.mute:
    command += ["--mute"]
if args.profile_engine:
    command += ["--profile-engine"]
if args.sample_engine:
    command += ["--sample-engine"]
if args.sample_workers:
    command += ["--sample-workers"]
if args.test_input: command += ["--test-input", str(args.test_input.resolve())]
if args.test_start: command += ["--test-start"]
if args.test_skip_intros: command += ["--test-skip-intros"]
with Path(command[0]).open("rb") as executable:
    executable_sha256 = hashlib.file_digest(executable, "sha256").hexdigest()
external_timeout = False
startupinfo = None
window_mode = "default"
if sys.platform == "win32":
    SW_SHOWNORMAL = 1  # Win32 documented value; subprocess exposes no such name
    startupinfo = subprocess.STARTUPINFO()
    startupinfo.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startupinfo.wShowWindow = SW_SHOWNORMAL
    window_mode = "SW_SHOWNORMAL"
with log.open("w", encoding="utf-8") as stream:
    try:
        result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT,
                                startupinfo=startupinfo, cwd=root,
                                timeout=args.timeout_ms / 1000 + 10)
        code = result.returncode
    except subprocess.TimeoutExpired:
        code = 5
        external_timeout = True
        stream.write("[TIMEOUT] Host process exceeded its external deadline.\n")
summary = {"command": command, "exit_code": code, "log": str(log), "gameplay_verified": False,
           "executable_sha256": executable_sha256, "external_timeout": external_timeout,
           "window_mode": window_mode}
if args.engine_preview:
    summary["renderer_subset"] = "original text/video plus engine world depth, stencil, motion, NDSP lighting, post-processing, resolves and frontbuffer presentation"
    summary["native_frame_capture"] = str(log.with_suffix(".bmp")) if log.with_suffix(".bmp").is_file() else None
    summary["native_video_captures"] = [str(path) for path in sorted(directory.glob(log.stem + "-video-*.bmp"))]
    summary["native_world_captures"] = [str(path) for path in sorted(directory.glob(log.stem + "-world-*.bmp"))]
    summary["native_input_captures"] = [str(path) for path in sorted(directory.glob(log.stem + "-input-*.bmp"))]
    summary["native_color_captures"] = [str(path) for path in sorted(directory.glob(log.stem + "-color-*.bmp"))]
log.with_suffix(".json").write_text(json.dumps(summary, indent=2) + "\n")
lines = log.read_text(encoding="utf-8", errors="replace").splitlines()
if not args.full_log and len(lines) > 24:
    print(f"[{len(lines) - 24} earlier lines retained in the evidence log]")
print("\n".join(lines if args.full_log else lines[-24:]))
print(f"Exit code: {code}; evidence: {log}")
sys.exit(code)
