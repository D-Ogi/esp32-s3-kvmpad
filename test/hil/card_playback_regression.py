# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Copyright 2026 D-Ogi. Noncommercial use only; see LICENSE.md.
# Required Notice: Copyright 2026 D-Ogi (https://github.com/D-Ogi/esp32-s3-kvmpad)
"""Verify native and prepared playback, or measure the prepared card library across modes and rates."""
import argparse
import json
from pathlib import Path
import re
import time

import kvm_serial

CASES = (
    ("kvp_640x360_5fps.mp4", "native", 15),
    ("kvp_640x360_10fps.mp4", "native", 29),
    ("kvp_640x360_15fps.mp4", "native", 44),
    ("kvp_640x360.mp4", "native", 73),
    ("test_1920x1080.h264", "auto", 73),
)
FAULT = re.compile(r"Guru Meditation|assert failed|Stack canary|CORRUPT HEAP|Rebooting\.\.\.|ESP-ROM:|BOOT_OK")


def prepared_rate_cases(spatial=True, temporal=False):
    spatial_modes = ("exact", "reduced", "large") if spatial else ()
    mp4_modes = spatial_modes + (("blend", "smart") if temporal else ())
    for shape in ("320x180", "640x360", "960x540"):
        for rate, frames in ((5, 15), (10, 29), (15, 44), (25, 73)):
            suffix = "" if rate == 25 else f"_{rate}fps"
            for mode in mp4_modes:
                yield f"kvp_{shape}{suffix}.mp4", "timed", frames, mode, rate
    for mode in spatial_modes:
        yield "test_1920x1080.h264", "timed", 73, mode, 25


def scale_comparison_cases():
    clips = (case for case in prepared_rate_cases() if case[3] == "exact" and case[0].endswith(".mp4"))
    for index, case in enumerate(clips):
        width, height = map(int, re.fullmatch(r"kvp_(\d+)x(\d+)(?:_\d+fps)?\.mp4", case[0]).groups())
        for scale in (("fit", "1:1") if index % 2 == 0 else ("1:1", "fit")):
            size = (960, 540) if scale == "fit" else (width // 2, height // 2)
            yield (*case, scale, size)


def check_picture_size(text, expected):
    pictures = re.findall(r"^CARD_HIL_PICTURE width=(\d+) height=(\d+)\s*$", text, re.MULTILINE)
    if len(pictures) != 1 or tuple(map(int, pictures[0])) != expected:
        raise ValueError(f"Expected one LCD picture size {expected}, received {pictures}")


def wait_line(port, text, pattern, seconds):
    deadline = time.monotonic() + seconds
    while True:
        clean = kvm_serial.ANSI.sub("", text)
        if FAULT.search(clean):
            raise RuntimeError("Reset or firmware fault during card playback")
        if re.search(pattern, clean, re.MULTILINE):
            return clean
        if time.monotonic() >= deadline:
            raise TimeoutError(f"Timed out waiting for {pattern}")
        text += port.read(4096).decode("utf-8", errors="replace")


def prepared_stages(text, epoch, frames):
    """Worker wall time starts after surface allocation and includes the final hold.

    Reader open covers header validation, source binding and prefill. Later fread
    refills are part of worker read time. The two prev_ready fields are portions
    of read and gate spent before the previous picture's completed refresh.
    The UI's selection-to-refresh interval can overlap the worker's wall time.
    """
    groups = {
        "reader": ("CARD_READER_IO", "open_us bind_us bind_bytes prefill_us prefill_bytes refill_us refill_bytes refill_calls"),
        "worker": ("CARD_WORKER_STAGES", "frames wall_us slot_us read_us gate_us drain_us hold_us read_prev_ready_us gate_prev_ready_us"),
        "display": ("CARD_DISPLAY_STAGES", "frames selected_to_ready_us"),
    }
    metrics = {}
    for group, (tag, names) in groups.items():
        lines = re.findall(rf"\b{tag} ([^\r\n]+)", text)
        if not lines:
            continue
        if len(lines) != 1:
            raise ValueError(f"Expected one {tag} summary")
        fields = dict(re.findall(r"(\w+)=(\S+)", lines[0]))
        if int(fields.get("epoch", -1)) != epoch or not all(fields.get(k, "").isdigit() for k in names.split()):
            raise ValueError(f"Incomplete or stale {tag} summary")
        metrics[group] = {k: int(fields[k]) for k in names.split()}
    if not metrics:
        return None  # Older engineering images report completed refresh intervals only.
    if metrics.keys() != groups.keys():
        raise ValueError("Incomplete prepared playback stage summaries")
    io, w, lcd = metrics["reader"], metrics["worker"], metrics["display"]
    if (w["frames"] != frames or lcd["frames"] != frames or
            io["bind_us"] + io["prefill_us"] > io["open_us"] or io["refill_us"] > w["read_us"] or
            sum(w[k] for k in ("slot_us", "read_us", "gate_us", "drain_us", "hold_us")) > w["wall_us"] or
            w["read_prev_ready_us"] > w["read_us"] or w["gate_prev_ready_us"] > w["gate_us"] or
            w["read_prev_ready_us"] + w["gate_prev_ready_us"] > lcd["selected_to_ready_us"] or
            lcd["selected_to_ready_us"] > w["wall_us"]):
        raise ValueError("Inconsistent prepared playback stage timings")
    return metrics


def check_result(text, name, policy, expected, mode="exact", target_fps=None):
    if FAULT.search(text):
        raise ValueError("Firmware fault in playback trace")
    rows = re.findall(r"^CARD_HIL_DONE ([^\r\n]+)", text, re.MULTILINE)
    if len(rows) != 1:
        raise ValueError("Expected one complete file result")
    fields = dict(re.findall(r"(\w+)=([^\s]+)", rows[0]))
    source = "native" if policy == "native" else "companion"
    if (fields.get("file"), fields.get("mode"), fields.get("policy"), fields.get("source"), fields.get("result")) != (
            name, mode, policy, source, "ok") or int(fields["frames"]) != expected:
        raise ValueError(f"Unexpected playback result for {name}: {rows[0]}")
    window = int(fields["window_us"])
    if window <= 0 or int(fields["epoch"]) <= 0:
        raise ValueError("Missing presentation interval or playback epoch")
    result = {"file": name, "path": source, "displayed_frames": expected, "window_us": window,
              "lcd_fps": round((expected - 1) * 1_000_000 / window, 3)}
    media_window = int(fields.get("media_window_us", 0))
    if media_window > 0:
        result["source_fps"] = round((expected - 1) * 1_000_000 / media_window, 3)
    if target_fps is not None:
        # Integer-microsecond endpoint rounding can change the span by one microsecond.
        target_us = round((expected - 1) * 1_000_000 / target_fps)
        if abs(media_window - target_us) > 1:
            raise ValueError(f"{name}: expected a {target_us} us media timeline, received {media_window} us")
        result.update(mode=mode, target_fps=target_fps, media_window_us=media_window)
    if policy == "timed":
        stages = prepared_stages(text, int(fields["epoch"]), expected)
        if stages:
            result["prepared_stages"] = stages
        counts = re.findall(r"\bCARD_VIDEO frames=(\d+) lcd_verification=skipped total=(\d+)\b", text)
        if counts != [(str(expected), str(expected))]:
            raise ValueError("Missing timed presentation count for the prepared file")
        intervals = re.findall(r"\brefresh_frames=([^\r\n]+)", text)
        if len(intervals) != 1:
            raise ValueError("Expected one LCD interval summary for this playback")
        fields = dict(re.findall(r"(\w+)=(\S+)", "refresh_frames=" + intervals[0]))
        required = ("refresh_frames", "window_us", "p50_us", "p95_us", "p99_us")
        if not all(k in fields for k in required):
            raise ValueError("Incomplete LCD interval summary")
        frames, span, median, p95, p99 = (int(fields[k]) for k in required)
        if frames != expected or span != window or not 0 < median <= p95 <= p99 <= span:
            raise ValueError("LCD interval summary does not match this playback")
        stats = {"p50": median, "p95": p95, "p99": p99, "percentile_scope": "unreported"}
        metadata = ("interval_samples", "interval_total", "max_gap_us")
        if any(k in fields for k in metadata):
            if not all(k in fields for k in metadata):
                raise ValueError("Incomplete LCD interval metadata")
            retained, total, longest = (int(fields[k]) for k in metadata)
            if total != frames - 1 or not 1 <= retained <= total or not p99 <= longest <= span:
                raise ValueError("Inconsistent LCD interval metadata")
            stats.update(percentile_scope="full" if retained == total else "last",
                         sample_count=retained, total_intervals=total, full_max=longest)
        result["lcd_interval_us"] = stats
    if policy == "auto":
        counts = re.search(r"CARD_VIDEO frames=(\d+) lcd_verified=(\d+) total=(\d+)\b", text)
        if not counts or any(int(n) != expected for n in counts.groups()):
            raise ValueError("LCD framebuffer verification was incomplete")
        line = re.search(r"CARD_BLACKOUT ([^\r\n]+)", text)
        if not line:
            raise ValueError("Missing inter-frame LCD samples")
        counters = {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", line[1])}
        if (counters["pairs"] != expected - 1 or counters["samples"] <= counters["pairs"] or
                any(counters[k] for k in ("confirmed", "inferred", "dark_at_picture", "resyncs", "longest_us"))):
            raise ValueError(f"Scanout mismatch: {counters}")
        result["scanout"] = counters
    return result


def status(port, command, pattern):
    reply, ok = kvm_serial.run_cmd(port, command, 6)
    match = re.search(pattern, reply)
    if not ok or not match:
        raise RuntimeError(f"Unexpected response to {command}")
    return match.group(1)


def panel_scanout(line):
    fields = dict(re.findall(r"(\w+)=(0x[0-9a-fA-F]+|-?\d+)", line))
    keys = ("vsync", "frames_done", "missed", "irq_mask_vsync", "irq_mask_frame")
    if any(key not in fields for key in keys):
        raise ValueError(f"Incomplete LCD statistics: {line}")
    counts = {key: int(fields[key], 0) for key in keys}
    if (counts["vsync"] <= 0 or counts["frames_done"] != counts["vsync"] or counts["missed"] != 0
            or counts["irq_mask_vsync"] != 2 or counts["irq_mask_frame"] != 2):
        raise ValueError(f"7B playback scanout check failed: {counts}")
    return counts


def run(port_name, output, rate_matrix=False, temporal_rates=False, check_scanout=True, compare_scale=False):
    if output.exists():
        raise FileExistsError(output)
    results = []
    with kvm_serial.open_port(port_name) as port:
        slot = status(port, "ota status", r"running\s+(ota_[01]) state=valid\b")
        boot = status(port, "info", r"boot_count=(\d+)\b")
        if check_scanout:
            status(port, "lcd stats", r"irq_mask_vsync=(0x2) irq_mask_frame=0x2\b")
        if compare_scale:
            cases = scale_comparison_cases()
        else:
            playback = (prepared_rate_cases(rate_matrix, temporal_rates) if rate_matrix or temporal_rates
                        else ((*case, "exact", None) for case in CASES))
            cases = ((*case, None, None) for case in playback)
        for name, policy, frames, mode, target, scale, size in cases:
            status(port, "ui shell status", r"UI_SHELL active=(yes)\b")
            if check_scanout:
                status(port, "lcd stats reset", r"(LCD\[now\][^\r\n]+)")
            command = f"ui media-card-run {name} {mode} {policy}"
            if scale == "1:1":
                command += " 1:1"
            reply, ok = kvm_serial.run_cmd(port, command, 5)
            if re.search(r"UI_MEDIA_CARD_RUN start=(?:ESP_ERR_\w+|ESP_FAIL)\b", reply):
                raise RuntimeError(f"The engineering image did not start {name}")
            try:
                if not ok or "UI_MEDIA_CARD_RUN start=ESP_OK" not in reply:
                    raise RuntimeError(f"Missing start acknowledgement for {name}")
                text = wait_line(port, reply, r"^(?:kvmpad> )?CARD_HIL_DONE [^\r\n]+\r*\n", 150)
                text = re.sub(r"(?m)^kvmpad> (?=CARD_)", "", text)
                result = check_result(text, name, policy, frames, mode, target)
                if size is not None:
                    check_picture_size(text, size)
                    result.update(scale=scale, display_width=size[0], display_height=size[1])
                if check_scanout:
                    result["panel_scanout"] = panel_scanout(status(port, "lcd stats", r"(LCD\[now\][^\r\n]+)"))
            finally:
                reply, _ = kvm_serial.run_cmd(port, "ui media-card-home", 5)
                if "UI_MEDIA_CARD_HOME status=ESP_ERR_INVALID_STATE" not in reply:
                    wait_line(port, reply, r"CARD_HIL_HOME ready=1\b", 15)
            status(port, "ui shell status", r"UI_SHELL active=(yes)\b")
            if status(port, "info", r"boot_count=(\d+)\b") != boot:
                raise RuntimeError("Boot count changed")
            results.append(result)
            source_rate = f", source {result['source_fps']} fps" if "source_fps" in result else ""
            label = f"{mode}, {scale}" if scale else mode
            print(f"{name} ({label}): {frames} LCD frames, {result['lcd_fps']} fps{source_rate}", flush=True)
    with output.open("x", encoding="utf-8") as f:
        json.dump({"slot": slot, "boot_count": int(boot), "cases": results}, f, indent=2)
        f.write("\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, help="new JSON path for the verified results")
    parser.add_argument("--port", default="COM3")
    parser.add_argument("--rate-matrix", action="store_true",
                        help="measure the 39 prepared spatial clip/mode combinations")
    parser.add_argument("--temporal-rates", action="store_true",
                        help="measure the 24 prepared MP4 temporal combinations, alone or with --rate-matrix")
    parser.add_argument("--compare-scale", action="store_true",
                        help="compare Fit and 1:1 using twelve MP4 clips and their Exact companions")
    parser.add_argument("--skip-scanout-check", action="store_true",
                        help="omit the 7B RGB frame and CPU1 interrupt checks when comparing experimental profiles")
    args = parser.parse_args()
    if args.compare_scale and (args.rate_matrix or args.temporal_rates):
        parser.error("--compare-scale is a separate paired playback suite")
    run(args.port, args.output, args.rate_matrix, args.temporal_rates, not args.skip_scanout_check, args.compare_scale)
