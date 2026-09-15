#!/usr/bin/env python3
"""Inspect game PCM tap WAV/CSV using only the Python standard library.

Usage: python tests/check_audio_pcm_tap.py capture.wav [capture.wav.csv]
       [--json report.json] [--max-period 256]
Defaults: discover <wav>.csv, write <wav>.analysis.json. No audio playback.
Blocks follow CSV frame_count when available, otherwise 256-frame chunks.
Clipped means finite abs(sample) >= 1; zero means every sample is exactly zero.
Hashes are byte-exact, so signed zero can hash differently despite being silent.
Periodic matches are diagnostic candidates, not proof of a playback defect.
"""

import argparse
from array import array
from collections import Counter
import csv
import hashlib
import json
import math
from pathlib import Path
import struct
import sys


def read_header(stream):
    size = stream.seek(0, 2)
    stream.seek(0)
    head = stream.read(12)
    if len(head) != 12 or head[:4] != b"RIFF" or head[8:] != b"WAVE":
        raise ValueError("Expected little-endian RIFF/WAVE")
    end = struct.unpack_from("<I", head, 4)[0] + 8
    if end > size or end < 12:
        raise ValueError("Truncated or invalid RIFF size")
    fmt = None
    data = None
    fact = None
    while stream.tell() + 8 <= end:
        chunk, length = struct.unpack("<4sI", stream.read(8))
        offset = stream.tell()
        if offset + length + (length & 1) > end:
            raise ValueError("Truncated WAV chunk")
        if chunk == b"fmt ":
            if length < 16:
                raise ValueError("Short fmt chunk")
            raw = stream.read(min(length, 40))
            tag, channels, rate, byte_rate, align, bits = struct.unpack_from("<HHIIHH", raw)
            mask = None
            valid_bits = bits
            subtype = tag
            if tag == 0xFFFE:
                if length < 40 or struct.unpack_from("<H", raw, 16)[0] < 22:
                    raise ValueError("Short WAVEFORMATEXTENSIBLE")
                valid_bits, mask = struct.unpack_from("<HI", raw, 18)
                guid = raw[24:40]
                if guid[4:] != bytes.fromhex("00001000800000aa00389b71"):
                    raise ValueError("Unrecognized WAV subtype GUID")
                subtype = struct.unpack_from("<I", guid)[0]
            if subtype != 3 or bits != 32 or valid_bits != 32:
                raise ValueError("Analyzer expects IEEE float32 WAV")
            if not (1 <= channels <= 64) or not rate or align != channels * 4 or byte_rate != rate * align:
                raise ValueError("Invalid channel/rate/block alignment")
            fmt = dict(format="WAVEFORMATEXTENSIBLE" if tag == 0xFFFE else "IEEE_FLOAT",
                       format_tag=tag, subtype="IEEE_FLOAT", channels=channels,
                       sample_rate=rate, bits_per_sample=bits, channel_mask=mask,
                       block_align=align, byte_rate=byte_rate)
        elif chunk == b"data":
            if data is not None:
                raise ValueError("Multiple data chunks are unsupported")
            data = (offset, length)
        elif chunk == b"fact" and length >= 4:
            fact = struct.unpack("<I", stream.read(4))[0]
        stream.seek(offset + length + (length & 1))
    if fmt is None or data is None:
        raise ValueError("Missing fmt or data chunk")
    if data[1] % fmt["block_align"]:
        raise ValueError("Incomplete PCM frame")
    fmt.update(data_offset=data[0], data_bytes=data[1], frame_count=data[1] // fmt["block_align"],
               fact_frames=fact, trailing_bytes=size - end)
    fmt["duration_seconds"] = fmt["frame_count"] / fmt["sample_rate"]
    return fmt


def read_metadata(path, frames):
    rows = []
    offset = 0
    with path.open(newline="", encoding="utf-8-sig") as stream:
        reader = csv.DictReader(stream)
        if not {"sequence", "qpc_us", "guest_address"}.issubset(reader.fieldnames or []):
            raise ValueError("CSV requires sequence,qpc_us,guest_address")
        for row in reader:
            if any(row.get(key) is None for key in ("sequence", "qpc_us", "guest_address")):
                raise ValueError("Incomplete CSV metadata row")
            count = int(row.get("frame_count") or min(256, frames - offset))
            start = int(row.get("frame_offset") or offset)
            if start != offset or not 1 <= count <= 256 or offset + count > frames:
                raise ValueError("CSV frame ranges must cover WAV contiguously, at most 256 frames per row")
            address_text = row["guest_address"].strip()
            address = int(address_text, 16 if address_text.lower().startswith("0x") else 10)
            item = dict(sequence=int(row["sequence"]), qpc_us=int(row["qpc_us"]),
                        guest_address=address, frame_offset=start, frame_count=count)
            if row.get("submitted_frame"):
                item["submitted_frame"] = int(row["submitted_frame"])
            rows.append(item)
            offset += count
    if offset != frames:
        raise ValueError("CSV frame counts do not match WAV")
    return rows


class Delta:
    def __init__(self):
        self.count = 0
        self.total = 0.0
        self.squares = 0.0
        self.maximum = 0.0

    def add(self, a, b):
        if math.isfinite(a) and math.isfinite(b):
            d = abs(a - b)
            self.count += 1
            self.total += d
            self.squares += d * d
            self.maximum = max(self.maximum, d)

    def result(self):
        return dict(count=self.count, mean_abs=self.total / self.count if self.count else None,
                    rms=math.sqrt(self.squares / self.count) if self.count else None,
                    max_abs=self.maximum if self.count else None)

    def merge(self, other):
        self.count += other.count
        self.total += other.total
        self.squares += other.squares
        self.maximum = max(self.maximum, other.maximum)


def analyze(wav_path, csv_path=None, max_period=256):
    with wav_path.open("rb") as stream:
        fmt = read_header(stream)
        frames, channels = fmt["frame_count"], fmt["channels"]
        metadata = read_metadata(csv_path, frames) if csv_path else None
        ranges = metadata if metadata is not None else [
            dict(frame_offset=i, frame_count=min(256, frames - i)) for i in range(0, frames, 256)]
        stream.seek(fmt["data_offset"])
        blocks = []
        first_hash = {}
        within, boundary = Delta(), Delta()
        previous = None
        totals = Counter(dict.fromkeys(("nonfinite_samples", "clipped_samples", "zero_samples",
                                      "silent_blocks", "nonfinite_blocks", "clipped_blocks",
                                      "repeated_blocks", "repeated_nonsilent_blocks"), 0))
        for index, block_range in enumerate(ranges):
            count = block_range["frame_count"]
            raw = stream.read(count * channels * 4)
            if len(raw) != count * channels * 4:
                raise ValueError("PCM data truncated during analysis")
            samples = array("f")
            samples.frombytes(raw)
            if sys.byteorder != "little":
                samples.byteswap()
            finite = [value for value in samples if math.isfinite(value)]
            nonfinite = len(samples) - len(finite)
            clipped = sum(abs(value) >= 1.0 for value in finite)
            zeros = sum(value == 0.0 for value in finite)
            silent = zeros == len(samples)
            digest = hashlib.sha256(raw).hexdigest()
            repeat_of = first_hash.get(digest)
            first_hash.setdefault(digest, index)
            local_within, local_boundary = Delta(), Delta()
            for pos in range(channels, len(samples)):
                local_within.add(samples[pos], samples[pos - channels])
            if previous is not None:
                for channel in range(channels):
                    local_boundary.add(samples[channel], previous[channel])
            within.merge(local_within)
            boundary.merge(local_boundary)
            previous = samples[-channels:]
            block = dict(index=index, **block_range, sha256=digest, nonfinite_samples=nonfinite,
                         clipped_samples=clipped, zero_samples=zeros, silent=silent,
                         repeat_of=repeat_of, peak_abs=max(map(abs, finite), default=None),
                         within_block_delta=local_within.result(), boundary_delta=local_boundary.result())
            blocks.append(block)
            totals.update(nonfinite_samples=nonfinite, clipped_samples=clipped, zero_samples=zeros,
                          silent_blocks=int(silent), nonfinite_blocks=int(nonfinite > 0),
                          clipped_blocks=int(clipped > 0), repeated_blocks=int(repeat_of is not None),
                          repeated_nonsilent_blocks=int(repeat_of is not None and not silent))

    periods = []
    for lag in range(1, min(max_period, len(blocks) - 1) + 1):
        matches = silence_matches = eligible = longest = run = 0
        for i in range(lag, len(blocks)):
            a, b = blocks[i], blocks[i - lag]
            equal_size = a["frame_count"] == b["frame_count"]
            if equal_size and not a["silent"] and not b["silent"]:
                eligible += 1
            equal = equal_size and a["sha256"] == b["sha256"]
            if equal and a["silent"]:
                silence_matches += 1
            if equal and not a["silent"]:
                matches += 1
                run += 1
                longest = max(longest, run)
            else:
                run = 0
        if matches or silence_matches:
            periods.append(dict(lag_blocks=lag, nonsilent_matches=matches, silence_matches=silence_matches,
                                eligible_nonsilent_pairs=eligible,
                                nonsilent_match_fraction=matches / eligible if eligible else None,
                                longest_nonsilent_match_run=longest))
    periods.sort(key=lambda p: (-p["nonsilent_matches"], p["lag_blocks"]))
    warnings = []
    if fmt["fact_frames"] is not None and fmt["fact_frames"] != frames:
        warnings.append("fact frame count differs from data frame count")
    if (channels, fmt["sample_rate"], fmt["channel_mask"]) != (6, 48000, 0x3F):
        warnings.append("Format differs from tap contract: 6 channels, 48000 Hz, mask 0x3F")
    timing = None
    if metadata is not None:
        gaps = [metadata[i]["qpc_us"] - metadata[i - 1]["qpc_us"] for i in range(1, len(metadata))]
        timing = dict(rows=len(metadata), sequence_discontinuities=sum(
            metadata[i]["sequence"] != metadata[i - 1]["sequence"] + 1 for i in range(1, len(metadata))),
            qpc_backwards=sum(gap < 0 for gap in gaps),
            qpc_gap_us_min=min(gaps, default=None), qpc_gap_us_max=max(gaps, default=None),
            qpc_gap_us_mean=sum(gaps) / len(gaps) if gaps else None)
    w, b = within.result(), boundary.result()
    ratio = b["mean_abs"] / w["mean_abs"] if w["mean_abs"] and b["mean_abs"] is not None else None
    return dict(wav=str(wav_path), csv=str(csv_path) if csv_path else None, format=fmt,
                summary=dict(blocks=len(blocks), **totals), within_block_delta=w, boundary_delta=b,
                boundary_to_within_mean_ratio=ratio, metadata=timing,
                periodic_repeats=periods, blocks=blocks, warnings=warnings,
                definitions=dict(clipped="finite abs(sample) >= 1.0; not necessarily a device clip",
                                 silent="all samples exactly zero, including signed zero",
                                 delta="absolute difference between consecutive frames in the same channel; nonfinite pairs excluded",
                                 periodic="byte-exact matches at each tested block lag; silence counted separately"))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("wav", type=Path)
    parser.add_argument("csv", nargs="?", type=Path)
    parser.add_argument("--json", type=Path, dest="output")
    parser.add_argument("--max-period", type=int, default=256, help="Maximum block lag to inspect (default 256)")
    args = parser.parse_args()
    if args.max_period < 1:
        parser.error("--max-period must be positive")
    csv_path = args.csv
    if csv_path is None:
        candidate = Path(str(args.wav) + ".csv")
        csv_path = candidate if candidate.exists() else None
    output = args.output or Path(str(args.wav) + ".analysis.json")
    try:
        if output.resolve() in {args.wav.resolve(), csv_path.resolve() if csv_path else args.wav.resolve()}:
            raise ValueError("JSON output must not replace input WAV or CSV")
        result = analyze(args.wav, csv_path, args.max_period)
        output.write_text(json.dumps(result, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    except (OSError, ValueError, KeyError, struct.error) as error:
        parser.exit(2, f"PCM analysis failed: {error}\n")
    fmt, summary = result["format"], result["summary"]
    print(f"{fmt['format']} float32: {fmt['sample_rate']} Hz, {fmt['channels']} channels, "
          f"{fmt['frame_count']} frames ({fmt['duration_seconds']:.3f}s), {summary['blocks']} blocks")
    print(f"Nonfinite={summary.get('nonfinite_samples', 0)} clipped={summary.get('clipped_samples', 0)} "
          f"silent_blocks={summary.get('silent_blocks', 0)} repeated_nonsilent={summary.get('repeated_nonsilent_blocks', 0)}")
    print(f"Delta mean: within={result['within_block_delta']['mean_abs']} "
          f"boundary={result['boundary_delta']['mean_abs']} ratio={result['boundary_to_within_mean_ratio']}")
    candidates = [p for p in result["periodic_repeats"] if p["nonsilent_matches"]][:5]
    print("Repeated block lags: " + (", ".join(f"{p['lag_blocks']} ({p['nonsilent_matches']} matches)" for p in candidates) or "none beyond silence"))
    for warning in result["warnings"]:
        print(f"Warning: {warning}")
    print(f"JSON: {output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
