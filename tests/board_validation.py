#!/usr/bin/env python3
"""Bounded, self-cleaning board tests. Real camera, NPU, MPP and GStreamer required."""
import argparse
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from edge_client import request

def wait_for(callback, timeout=15):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        try:
            result = callback()
            if result:
                return result
        except (OSError, ValueError) as error:
            last = error
        time.sleep(.15)
    raise AssertionError("condition timed out: " + str(last))

def run(args):
    os.chdir(ROOT)
    out = ROOT / "output" / ("acceptance-" + str(time.time_ns()))
    out.mkdir(parents=True)
    config = json.loads(Path("configs/default.json").read_text())
    alias = out / "camera"
    # Start with a missing camera; recovery is exercised without unbinding hardware.
    config.update(device=str(alias), output=str(out), port=args.port,
                  rtsp_port=args.rtsp_port, run_seconds=0,
                  infer_delay_ms=150, event_tail=2, event_pre=1)
    path = out / "config.json"
    path.write_text(json.dumps(config, indent=2))
    report = {"config":config, "checks":[], "note":"No physical USB unplug or external PC latency measurement."}
    player = None
    def call(cmd, **fields):
        return request("127.0.0.1", args.port, dict(cmd=cmd, **fields), timeout=3)
    with (out / "run.log").open("w") as log:
        agent = subprocess.Popen([args.binary, str(path)], stdout=log, stderr=subprocess.STDOUT)
        try:
            wait_for(lambda: call("get_status")["data"]["capture_state"] == "recovering")
            report["checks"].append("missing camera leaves control service available")
            alias.symlink_to(args.camera)
            wait_for(lambda: call("get_status")["data"]["capture_state"] == "running")
            wait_for(lambda: call("get_detections")["data"]["sequence"] > 0)
            report["checks"].append("camera appears and capture recovers")
            baseline_fd = len(list(Path("/proc", str(agent.pid), "fd").iterdir()))
            subprocess.run([sys.executable, "tests/control_integration.py", str(args.port)], check=True, timeout=30)
            report["checks"].append("control protocol regression")
            call("stop")
            time.sleep(.6)
            assert call("get_detections")["data"]["sequence"] == 0
            report["checks"].append("stop invalidates in-flight detections")
            call("start")
            wait_for(lambda: call("get_detections")["data"]["sequence"] > 0)
            # Half-close a synchronous request; the response must still arrive.
            with socket.create_connection(("127.0.0.1", args.port), timeout=3) as s:
                s.sendall(b'{"id":999,"cmd":"get_status"}\n')
                s.shutdown(socket.SHUT_WR)
                assert json.loads(s.makefile("rb").readline())["id"] == 999
            report["checks"].append("TCP half-close preserves response")
            for _ in range(40):
                call("get_status")
            # Slow reader and malformed stream may be dropped, without service failure.
            with socket.create_connection(("127.0.0.1", args.port), timeout=3) as slow:
                slow.sendall(b'{"cmd":"get_metrics"}\n' * 100)
                time.sleep(.3)
                assert call("get_status")["code"] == 0
            report["checks"].append("slow control client cannot block service")
            for _ in range(2):
                assert call("restart_camera")["status"] == "accepted"
                time.sleep(.5)
                wait_for(lambda: call("get_status")["data"]["capture_state"] == "running")
                wait_for(lambda: call("get_detections")["data"]["sequence"] > 0)
            report["checks"].append("camera restart and resume")
            # Two real RTSP PLAY/TEARDOWN cycles, including decode.
            for i in range(2):
                with (out / ("rtsp-%d.log" % i)).open("w") as play_log:
                    player = subprocess.Popen([
                        "gst-launch-1.0", "-q", "rtspsrc",
                        "location=rtsp://127.0.0.1:%d/live" % args.rtsp_port,
                        "protocols=tcp", "latency=100", "!", "rtph264depay", "!",
                        "h264parse", "!", "avdec_h264", "!",
                        "fakesink", "num-buffers=45", "sync=false"
                    ], stdout=play_log, stderr=subprocess.STDOUT,
                        env=dict(os.environ, GIO_USE_PROXY_RESOLVER="dummy"))
                    assert player.wait(timeout=18) == 0, "RTSP decode failed"
                    player = None
            report["checks"].append("RTSP decode and reconnect")
            call("stop")  # Isolate the manual event from live scene retriggers.
            time.sleep(.4)
            assert call("record_event")["status"] == "accepted"
            manifests = wait_for(lambda: [
                p for p in (out/"events").glob("*.json")
                if json.loads(p.read_text()).get("complete")], timeout=12)
            h264 = manifests[0].with_suffix("")
            with (out / "record-decode.log").open("w") as decode_log:
                subprocess.run(["gst-launch-1.0","-q","filesrc","location="+str(h264),
                    "!","h264parse","!","avdec_h264","!","fakesink","sync=false"],
                    stdout=decode_log, stderr=subprocess.STDOUT, check=True, timeout=15)
            report["checks"].append("event recording completes and decodes")
            # Measure counter differences, independent of timer/client sampling windows.
            call("start")
            time.sleep(.5)
            first = call("get_metrics")["data"]
            time.sleep(5)
            last = call("get_metrics")["data"]
            seconds = last["uptime_s"] - first["uptime_s"]
            rates = {name:(last["counts"].get(name,0)-first["counts"].get(name,0))/seconds
                     for name in ("captured","processed","encoded")}
            assert rates["captured"] > 2 and rates["encoded"] >= rates["captured"]*.8, rates
            assert rates["processed"] < rates["encoded"]*.8, rates
            assert last["pipeline"]["queue_high_watermark"] <= config["queue"]
            report["stress_rates"] = rates
            report["checks"].append("slow inference leaves video running with bounded queue")
            report["fd_growth"] = len(list(Path("/proc",str(agent.pid),"fd").iterdir())) - baseline_fd
            report["last_metrics"] = last
            assert report["fd_growth"] < 12, report["fd_growth"]
            report["checks"].append("no per-connection FD accumulation")
        except Exception as error:
            report["failure"] = repr(error)
            raise
        finally:
            if player is not None:
                player.terminate()
                try: player.wait(timeout=4)
                except subprocess.TimeoutExpired: player.kill(); player.wait()
            started = time.monotonic()
            agent.send_signal(signal.SIGINT)
            try:
                code = agent.wait(timeout=12)
                report["exit_code"] = code
                report["shutdown_seconds"] = time.monotonic()-started
            except subprocess.TimeoutExpired:
                agent.kill(); agent.wait()
                report["failure"] = "shutdown timeout"
            (out/"report.json").write_text(json.dumps(report, indent=2))
            print("Validation report:", out/"report.json", flush=True)
    assert "failure" not in report, report
    assert report["exit_code"] == 0, report
    print(json.dumps(report,indent=2))

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary",default="build-refactor/edge_agent")
    parser.add_argument("--camera",default="/dev/video10")
    parser.add_argument("--port",type=int,default=19000)
    parser.add_argument("--rtsp-port",type=int,default=18554)
    run(parser.parse_args())
