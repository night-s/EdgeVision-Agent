#!/usr/bin/env python3
"""Comparable fixed-config board runs; stores raw logs, metrics and failure status."""
import argparse
import json
from pathlib import Path
import subprocess
import time

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", default="build-refactor/edge_agent")
    parser.add_argument("--config", default="configs/default.json")
    parser.add_argument("--seconds", type=int, default=60)
    parser.add_argument("--backends", nargs="+", choices=["cpu","rga"], default=["cpu","rga"])
    parser.add_argument("--synthetic", action="store_true")
    parser.add_argument("--replay-yuyv", default="")
    parser.add_argument("--float-outputs", action="store_true")
    parser.add_argument("--no-video", action="store_true")
    args = parser.parse_args()
    if args.seconds < 5:
        parser.error("--seconds must be at least 5")
    root = Path("output") / ("benchmark-" + str(time.time_ns()))
    root.mkdir(parents=True)
    summary = {}
    for backend in args.backends:
        directory = root / backend
        directory.mkdir()
        config = json.loads(Path(args.config).read_text())
        config.update(preprocess=backend, run_seconds=args.seconds,
                      synthetic=args.synthetic, replay_yuyv=args.replay_yuyv,
                      native_outputs=not args.float_outputs,
                      video=not args.no_video, output=str(directory))
        path = directory / "config.json"
        path.write_text(json.dumps(config, indent=2))
        with (directory / "run.log").open("w") as log:
            started = time.monotonic()
            try:
                result = subprocess.run([args.binary,str(path)],stdout=log,stderr=subprocess.STDOUT,
                                        timeout=args.seconds+25)
                code = result.returncode
            except subprocess.TimeoutExpired:
                code = "timeout"
            elapsed = time.monotonic()-started
        metrics = directory / "metrics.jsonl"
        records = [json.loads(line) for line in metrics.read_text().splitlines()] if metrics.exists() else []
        last = records[-1] if records else {}
        counts = last.get("counts",{})
        valid = (code == 0 and counts.get("processed",0)>0 and
                 counts.get("inference_errors",0)==0 and counts.get("video_errors",0)==0 and
                 (args.no_video or counts.get("encoded",0)>0))
        # Final record includes shutdown; use cumulative deltas across interior samples.
        steady = records[1:-1]
        rates = {}
        if len(steady)>1:
            a,b = steady[0],steady[-1]
            dt = b["uptime_s"]-a["uptime_s"]
            rates = {key:(b["counts"].get(key,0)-a["counts"].get(key,0))/dt
                     for key in ("captured","processed","encoded")}
        summary[backend] = {"valid":valid,"exit_code":code,"wall_seconds":elapsed,
                            "steady_rates":rates,"last_metrics":last or None}
    (root/"summary.json").write_text(json.dumps(summary,indent=2))
    print(json.dumps({"directory":str(root),"summary":summary},indent=2))
    return 0 if all(s["valid"] for s in summary.values()) else 1

if __name__ == "__main__":
    raise SystemExit(main())
