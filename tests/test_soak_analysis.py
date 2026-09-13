#!/usr/bin/env python3
"""Deterministic tests for rejecting interrupted, frozen and leaking sessions."""
import copy
from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
from analyze_soak import analyze

pc=dict(observed_seconds=20,clock_gaps=0,frames=400,read_errors=0,control_errors=0,
        reconnects=0,workers_stopped=True,duration_s=20)
rows=[dict(elapsed_s=t,frames=t*20,video_error="",metrics={"rss_mb":50,"fd_count":40}) for t in range(0,21,5)]
board=dict(returncode=0,graceful_shutdown_marker=True,last_metrics={"uptime_s":21},
           recording_decode=[{"returncode":0}])
assert analyze(pc,rows,board,20,0)["passed"]
sleep=copy.deepcopy(pc);sleep.update(duration_s=40000,observed_seconds=5,clock_gaps=1)
assert not analyze(sleep,rows,board,20,0)["passed"]
frozen=copy.deepcopy(rows)
for r in frozen: r["frames"]=1
assert not analyze(pc,frozen,board,20,0)["checks"]["decode_progress"]
leak=copy.deepcopy(rows);leak[-1]["metrics"]["fd_count"]=60
assert not analyze(pc,leak,board,20,0)["checks"]["fd_growth_within_4_budget"]
assert not analyze(pc,rows,{},20,0)["passed"]
print("Soak acceptance guards passed")
