#!/usr/bin/env python3
"""Board half of a PC+RK3568 soak; pairs with tools/pc_viewer.py --headless."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import time

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument("--seconds", type=int, default=3720)
ap.add_argument("--output", default="output/soak")
ap.add_argument("--device", required=True)
args = ap.parse_args()
root = Path(__file__).resolve().parents[1]
os.chdir(root)
out=Path(args.output).resolve()
out.mkdir(parents=True,exist_ok=True)
if subprocess.run(["pgrep","-x","edge_agent"],capture_output=True).returncode == 0:
    raise SystemExit("An agent is already running; stop it before an exclusive soak")
c=json.loads(Path("configs/default.json").read_text())
c.update(device=args.device,port=19400,rtsp_port=18580,output=str(out),run_seconds=args.seconds,
         event_pre=1,event_tail=2,record_max_seconds=30)
(out/"config.json").write_text(json.dumps(c,indent=2))
report={"started_utc":datetime.datetime.now(datetime.timezone.utc).isoformat(),"config":c,
        "source_commit":subprocess.check_output(["git","rev-parse","HEAD"],text=True).strip(),
        "binary_sha256":hashlib.sha256(Path("build-refactor/edge_agent").read_bytes()).hexdigest(),
        "environment":subprocess.check_output(["uname","-a"],text=True).strip()}
(out/"execution.json").write_text(json.dumps(report,indent=2))
log=(out/"agent.log").open("w")
proc=subprocess.Popen(["build-refactor/edge_agent",str(out/"config.json")],stdout=log,stderr=subprocess.STDOUT)
(out/"pid").write_text(str(proc.pid))
def stop(signum,frame):
    report["stop_requested_monotonic"]=time.monotonic()
    if proc.poll() is None: proc.send_signal(signal.SIGINT)
signal.signal(signal.SIGINT,stop)
signal.signal(signal.SIGTERM,stop)
start=time.monotonic()
try:
    proc.wait()
finally:
    if proc.poll() is None:
        proc.send_signal(signal.SIGINT)
        try:proc.wait(15)
        except subprocess.TimeoutExpired:proc.kill();proc.wait()
    ended=time.monotonic()
    log.close()
    report["graceful_shutdown_marker"]="[shutdown] all workers stopped" in (out/"agent.log").read_text(errors="replace")
    if "stop_requested_monotonic" in report: report["shutdown_s"]=ended-report["stop_requested_monotonic"]
    report.update(duration_s=time.monotonic()-start,returncode=proc.returncode)
    entries=[]
    metrics=out/"metrics.jsonl"
    if metrics.exists():
        entries=[json.loads(line) for line in metrics.read_text().splitlines() if line.strip()]
    report["metrics_samples"]=len(entries)
    if entries: report["first_metrics"],report["last_metrics"]=entries[0],entries[-1]
    results=[]
    for path in sorted((out/"events").glob("*.h264")):
        command=["gst-launch-1.0","-q","filesrc","location="+str(path),"!","h264parse","!","avdec_h264","!","fakesink","sync=false"]
        try:
            result=subprocess.run(command,capture_output=True,text=True,timeout=30)
            results.append(dict(file=path.name,returncode=result.returncode,error=result.stderr[-2000:]))
        except subprocess.TimeoutExpired:
            results.append(dict(file=path.name,returncode=-1,error="decode timeout"))
    report["recording_decode"]=results
    report["passed"]=proc.returncode==0 and report["graceful_shutdown_marker"] and bool(results) and all(r["returncode"]==0 for r in results)
    report["scope"]="Board execution and final decode only; pair with PC report for duration, playback and reconnect assertions."
    (out/"board-report.json").write_text(json.dumps(report,indent=2))
    print(json.dumps({"output":str(out),"duration_s":report["duration_s"],"passed":report["passed"]}))
