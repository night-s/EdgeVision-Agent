#!/usr/bin/env python3
"""Combine PC decode observations and board shutdown/decode evidence."""
import argparse
import csv
import json
from pathlib import Path


def analyze(pc, rows, board, seconds=3600, reconnects=10):
    observed = [r for r in rows if r.get("metrics")]
    steady = [r for r in observed if r["elapsed_s"] >= min(60, seconds / 10)]
    steady = steady or observed
    last_frames = -1
    progress_at = 0.
    stalled = []
    gaps = []
    previous = None
    for row in observed:
        elapsed = row["elapsed_s"]
        if previous is not None and elapsed - previous > 10:
            gaps.append([previous, elapsed])
        previous = elapsed
        if row["frames"] > last_frames:
            progress_at, last_frames = elapsed, row["frames"]
        if elapsed - progress_at > 10:
            stalled.append(elapsed)
    first = steady[0]["metrics"] if steady else {}
    last = steady[-1]["metrics"] if steady else {}
    rss_start, rss_end = first.get("rss_mb", 0), last.get("rss_mb", 0)
    fd_start, fd_end = first.get("fd_count", 0), last.get("fd_count", 0)
    decode = board.get("recording_decode", [])
    checks = {
        "effective_observation_duration": pc.get("observed_seconds", 0) >= seconds - 1,
        "no_suspend_or_clock_gap": pc.get("clock_gaps", 1) == 0 and not gaps,
        "sufficient_samples": len(observed) >= int(seconds / 5) - 3,
        "decode_progress": pc.get("frames", 0) > 0 and not stalled,
        "no_observed_video_errors": pc.get("read_errors", 1) == 0 and all(not r.get("video_error") for r in steady),
        "control_available": pc.get("control_errors", 1) == 0,
        "planned_reconnections": pc.get("reconnects", 0) >= reconnects,
        "pc_workers_stopped": pc.get("workers_stopped", False),
        "board_runtime": board.get("last_metrics", {}).get("uptime_s", 0) >= seconds,
        "board_graceful_exit": board.get("returncode", -1) == 0 and board.get("graceful_shutdown_marker", False),
        "recordings_decodable": bool(decode) and all(r.get("returncode") == 0 for r in decode),
        "rss_growth_within_16mb_budget": bool(steady) and rss_end - rss_start <= 16,
        "fd_growth_within_4_budget": bool(steady) and fd_end - fd_start <= 4,
    }
    return {
        "passed": all(checks.values()), "checks": checks,
        "source_commit": board.get("source_commit"), "binary_sha256": board.get("binary_sha256"),
        "config": board.get("config"), "environment": board.get("environment"),
        "pc_observed_seconds": pc.get("observed_seconds"), "pc_wall_seconds": pc.get("duration_s"),
        "decoded_frames": pc.get("frames"), "reconnections": pc.get("reconnects"),
        "samples": len(observed), "sample_gaps": gaps, "stalled_sample_times": stalled,
        "rss_start_mb": rss_start, "rss_end_mb": rss_end,
        "rss_peak_mb": max((r["metrics"].get("rss_mb", 0) for r in steady), default=0),
        "fd_start": fd_start, "fd_end": fd_end,
        "fd_peak": max((r["metrics"].get("fd_count", 0) for r in steady), default=0),
        "shutdown_s": board.get("shutdown_s"), "recording_decode": decode,
        "last_counts": board.get("last_metrics", {}).get("counts", {}),
        "scope": "One measured run. Memory/FD thresholds are acceptance budgets, not proof of absence of leaks.",
    }


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--pc",required=True,help="PC report.json")
    ap.add_argument("--samples",required=True,help="PC pc-metrics.jsonl")
    ap.add_argument("--board",required=True,help="Board board-report.json")
    ap.add_argument("--output",required=True)
    args=ap.parse_args()
    pc=json.loads(Path(args.pc).read_text(encoding="utf-8-sig"))
    board=json.loads(Path(args.board).read_text(encoding="utf-8-sig"))
    rows=[json.loads(line) for line in Path(args.samples).read_text(encoding="utf-8-sig").splitlines() if line.strip()]
    result=analyze(pc,rows,board)
    output=Path(args.output);output.parent.mkdir(parents=True,exist_ok=True)
    output.write_text(json.dumps(result,indent=2),encoding="utf-8")
    with output.with_suffix(".csv").open("w",newline="",encoding="utf-8") as stream:
        fields=["elapsed_s","board_uptime_s","decoded_frames","reconnects","read_errors","control_errors",
                "captured","processed","encoded","rss_mb","fd_count","capture_fps","process_fps","encode_fps",
                "inference_dropped","video_dropped","driver_sequence_gaps","inference_health","recording_state"]
        writer=csv.DictWriter(stream,fieldnames=fields);writer.writeheader()
        for r in rows:
            m=r.get("metrics",{});p=m.get("pipeline",{});c=m.get("counts",{})
            writer.writerow(dict(elapsed_s=r["elapsed_s"],board_uptime_s=m.get("uptime_s"),
                decoded_frames=r["frames"],reconnects=r["reconnects"],read_errors=r["read_errors"],
                control_errors=r["control_errors"],captured=c.get("captured"),processed=c.get("processed"),
                encoded=c.get("encoded"),rss_mb=m.get("rss_mb"),fd_count=m.get("fd_count"),
                capture_fps=m.get("capture_fps"),process_fps=m.get("process_fps"),encode_fps=m.get("encode_fps"),
                inference_dropped=p.get("inference_dropped"),video_dropped=p.get("video_dropped"),
                driver_sequence_gaps=c.get("camera_driver_sequence_gaps",0),
                inference_health=p.get("inference_health",{}).get("state"),
                recording_state=p.get("recorder",{}).get("current",{}).get("state")))
    print(json.dumps(result,indent=2))
    return 0 if result["passed"] else 1

if __name__ == "__main__":
    raise SystemExit(main())
