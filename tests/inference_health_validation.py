#!/usr/bin/env python3
"""Synthetic-input board test using real RKNN and MPP, separate service ports."""
import json, os, signal, subprocess, sys, time
from pathlib import Path
root = Path(__file__).resolve().parents[1]
os.chdir(root)
sys.path.insert(0, str(root / "tools"))
from edge_client import request
out = root / "output" / ("health-validation-" + str(time.time_ns()))
out.mkdir(parents=True)
cfg = json.loads((root / "configs/default.json").read_text())
cfg.update(synthetic=True, port=19300, rtsp_port=18560, output=str(out),
           allow_fault_injection=True, infer_slow_ms=150, infer_stall_ms=800,
           infer_max_age_ms=500, event_tail=1, event_pre=1)
(out / "config.json").write_text(json.dumps(cfg, indent=2))
report = {"checks": [], "output": str(out)}
def call(cmd, **args):
    r = request("127.0.0.1", 19300, dict(cmd=cmd, **args), timeout=3)
    assert r["code"] == 0, r
    return r.get("data", {})
def until(fn, seconds=20):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        assert proc.poll() is None, "agent exited"
        try:
            if fn(): return
        except (ConnectionError, OSError): pass
        time.sleep(.1)
    raise AssertionError("condition timed out")
log = (out / "agent.log").open("w")
proc = subprocess.Popen(["build-refactor/edge_agent", str(out / "config.json")],
                        stdout=log, stderr=subprocess.STDOUT)
try:
    until(lambda: call("get_detections").get("fresh"))
    report["checks"].append("initial fresh inference")
    call("set_infer_delay", value=600)
    until(lambda: call("get_status")["inference_health"]["state"] == "degraded")
    until(lambda: not call("get_detections")["fresh"])
    report["checks"].append("degradation and expired-result filtering")
    slow = call("get_metrics")
    assert slow["counts"].get("stale_results_discarded", 0) > 0
    time.sleep(2)
    later = call("get_metrics")
    for key in ("captured", "encoded"):
        assert later["counts"][key] > slow["counts"][key] + 20, key
    report["checks"].append("capture and encoder continue during slowdown")
    call("capture")
    report["checks"].append("snapshot responds during slowdown")
    call("set_infer_delay", value=1500)
    until(lambda: call("get_status")["inference_health"]["state"] == "stalled")
    report["checks"].append("stall observable while worker active")
    call("set_infer_delay", value=0)
    until(lambda: call("get_status")["inference_health"]["state"] == "normal")
    until(lambda: call("get_detections")["fresh"])
    report["checks"].append("hysteretic recovery and fresh results")
    call("stop")
    time.sleep(.3)
    call("record_event")
    until(lambda: call("get_events")["current"].get("state") == "completed")
    report["checks"].append("queryable event completion")
    call("record_event")
    until(lambda: call("get_events")["current"].get("state") == "recording")
    call("restart_camera")
    until(lambda: any(e.get("reason") == "capture_restart" for e in call("get_events")["recent"]))
    call("record_event")
    until(lambda: call("get_events")["current"].get("state") == "completed")
    report["checks"].append("capture restart closes old recording and new event completes")
    report["final_metrics"] = call("get_metrics")
    report["passed"] = True
except Exception as exc:
    report["passed"] = False
    report["error"] = repr(exc)
finally:
    started = time.monotonic()
    if proc.poll() is None: proc.send_signal(signal.SIGINT)
    try: proc.wait(timeout=12)
    except subprocess.TimeoutExpired:
        proc.kill(); proc.wait(); report["passed"] = False
        report["shutdown_timeout"] = True
    report["shutdown_s"] = time.monotonic() - started
    report["returncode"] = proc.returncode
    if proc.returncode: report["passed"] = False
    log.close()
    (out / "report.json").write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2))
sys.exit(0 if report["passed"] else 1)
