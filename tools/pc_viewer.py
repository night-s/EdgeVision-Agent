#!/usr/bin/env python3
"""RTSP live view + exact source-frame detection pane; Windows/Linux, OpenCV 4.10."""
import argparse
import json
import os
from pathlib import Path
import threading
import time

os.environ.setdefault("OPENCV_FFMPEG_CAPTURE_OPTIONS", "rtsp_transport;tcp")
import cv2
import numpy as np
from edge_client import request


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default="192.168.94.126")
    ap.add_argument("--port", type=int, default=9000)
    ap.add_argument("--rtsp-port", type=int, default=8554)
    ap.add_argument("--source", help="Override RTSP URL or play a local recording; overlay disabled for files")
    ap.add_argument("--file-fps", type=float, default=0, help="Override local-file playback FPS; raw H264 has no capture timeline")
    ap.add_argument("--headless", action="store_true", help="Decode without a desktop window")
    ap.add_argument("--seconds", type=float, default=0)
    ap.add_argument("--output", default="pc-output")
    ap.add_argument("--reconnect-every", type=float, default=0)
    ap.add_argument("--reconnect-count", type=int, default=10)
    ap.add_argument("--record-every", type=float, default=0)
    args = ap.parse_args()
    out = Path(args.output)
    out.mkdir(parents=True, exist_ok=True)
    if os.name == "nt":
        import ctypes
        ctypes.windll.kernel32.SetThreadExecutionState(0x80000001)
    stop = threading.Event()
    lock = threading.Lock()
    state = dict(frame=None, preview=None, metrics={}, frames=0, reconnects=0,
                 read_errors=0, control_errors=0, last_frame=0., video_error="", control_error="")
    local_file = bool(args.source and not args.source.startswith("rtsp://"))
    url = args.source or f"rtsp://{args.host}:{args.rtsp_port}/live"
    started = time.monotonic()
    def command(cmd):
        response = request(args.host, args.port, {"cmd": cmd}, timeout=3)
        if response["code"] != 0:
            raise RuntimeError(response.get("reason", response))
        return response.get("data", {})
    def decode():
        next_reconnect = time.monotonic() + args.reconnect_every
        while not stop.is_set():
            cap = cv2.VideoCapture(url, cv2.CAP_FFMPEG, [
                cv2.CAP_PROP_OPEN_TIMEOUT_MSEC, 5000, cv2.CAP_PROP_READ_TIMEOUT_MSEC, 5000])
            try:
                if not cap.isOpened():
                    with lock: state["video_error"] = "cannot open video"
                    stop.wait(1)
                    continue
                file_fps = args.file_fps or cap.get(cv2.CAP_PROP_FPS) or 25
                if file_fps <= 0 or file_fps > 240: file_fps = 25
                next_file_frame = time.monotonic()
                while not stop.is_set():
                    ok, frame = cap.read()
                    if not ok:
                        if local_file:
                            stop.set()
                        else:
                            with lock:
                                state["read_errors"] += 1
                                state["video_error"] = "video read failed"
                        break
                    with lock:
                        state["frame"] = frame
                        state["frames"] += 1
                        state["last_frame"] = time.monotonic()
                        state["video_error"] = ""
                        reconnects = state["reconnects"]
                    if local_file and not args.headless:
                        next_file_frame += 1 / file_fps
                        stop.wait(max(0, next_file_frame - time.monotonic()))
                    if args.reconnect_every and reconnects < args.reconnect_count and time.monotonic() >= next_reconnect:
                        with lock: state["reconnects"] += 1
                        next_reconnect = time.monotonic() + args.reconnect_every
                        break
            except Exception as exc:
                with lock: state["video_error"] = str(exc)
            finally:
                cap.release()
            stop.wait(.1)
    def controls():
        next_record = time.monotonic() + args.record_every
        while not stop.is_set():
            try:
                metrics = command("get_metrics")
                with lock:
                    state["metrics"] = metrics
                    state["control_error"] = ""
                if not args.headless:
                    try:
                        preview = command("get_preview")
                        preview["received_at"] = time.monotonic()
                        with lock: state["preview"] = preview
                    except RuntimeError:
                        with lock: state["preview"] = None
                if args.record_every and time.monotonic() >= next_record:
                    command("record_event")
                    next_record = time.monotonic() + args.record_every
            except Exception as exc:
                with lock:
                    state["control_errors"] += 1
                    state["control_error"] = str(exc)
            stop.wait(1 if args.headless else .25)
    threads = [threading.Thread(target=decode, daemon=True)]
    if not args.source: threads.append(threading.Thread(target=controls, daemon=True))
    for thread in threads: thread.start()
    last_log = 0.
    last_tick = time.monotonic()
    observed_seconds = 0.
    clock_gaps = 0
    metrics_seen = []
    def label(img, text, y, color=(230, 230, 230)):
        cv2.putText(img, text, (12, y), cv2.FONT_HERSHEY_SIMPLEX, .52, color, 1, cv2.LINE_AA)
    try:
        with (out / "pc-metrics.jsonl").open("w", encoding="utf-8") as log:
            while not stop.is_set():
                now = time.monotonic()
                tick = now - last_tick
                last_tick = now
                if tick > 15:
                    clock_gaps += 1
                else:
                    observed_seconds += tick
                if args.seconds and now - started >= args.seconds: break
                with lock: snapshot = dict(state)
                m = snapshot["metrics"]
                if now - last_log >= 5:
                    record = {k: v for k, v in snapshot.items() if k not in ("frame", "preview")}
                    record["elapsed_s"] = now - started
                    log.write(json.dumps(record) + "\n")
                    log.flush()
                    if m: metrics_seen.append(m)
                    last_log = now
                if not args.headless:
                    canvas = np.zeros((650, 1280, 3), np.uint8)
                    if snapshot["frame"] is not None:
                        canvas[40:520, :640] = cv2.resize(snapshot["frame"], (640, 480))
                    preview = snapshot["preview"]
                    label(canvas, "LIVE RTSP (independent video clock)", 25)
                    label(canvas[:, 640:], "MATCHED DETECTION FRAME (not the RTSP frame)", 25)
                    if preview:
                        image = cv2.imdecode(np.frombuffer(bytes.fromhex(preview["jpeg_hex"]), np.uint8), cv2.IMREAD_COLOR)
                        if image is not None:
                            image = cv2.resize(image, (640, 480))
                            age = preview["age_ms"] + (now - preview["received_at"]) * 1000
                            if age < 500:
                                for det in preview["detections"]:
                                    x, y, w, h = det["box"]
                                    sx, sy = 640 / preview["source_width"], 480 / preview["source_height"]
                                    a, b = (int(x*sx), int(y*sy)), (int((x+w)*sx), int((y+h)*sy))
                                    cv2.rectangle(image, a, b, (0, 220, 90), 2)
                                    name = "person" if det["class_id"] == 0 else str(det["class_id"])
                                    cv2.putText(image, f"{name} {det['confidence']:.2f}", (a[0], max(18,a[1]-5)),
                                                cv2.FONT_HERSHEY_SIMPLEX, .55, (0,220,90), 1)
                            canvas[40:520, 640:] = image
                            label(canvas[:,640:], f"seq={preview['sequence']} age~{age:.0f}ms " +
                                  ("STALE: boxes hidden" if age >= 500 else "same-frame boxes"), 545)
                    else:
                        label(canvas[:,640:], "No fresh detection frame", 545)
                    p = m.get("pipeline", {})
                    health = p.get("inference_health", {}).get("state", "unknown")
                    p95 = m.get("stages_ms", {}).get("capture_to_detection", {}).get("p95", 0)
                    label(canvas, f"Capture {m.get('capture_fps',0):.1f}  Process {m.get('process_fps',0):.1f}  "
                          f"Encode {m.get('encode_fps',0):.1f} FPS  P95 {p95:.1f} ms", 575)
                    recording = p.get("recorder", {}).get("current", {}).get("state", "unknown")
                    label(canvas, f"Inference {health}  dropped {p.get('inference_dropped',0)}  recording {recording}  "
                          f"video age {now-snapshot['last_frame']:.1f}s", 600)
                    label(canvas, "Q/Esc quit | R record event | S snapshot | " +
                          snapshot["video_error"] + " " + snapshot["control_error"], 630)
                    cv2.imshow("EdgeVision", canvas)
                    key = cv2.waitKey(20) & 255
                    if key in (27, ord("q")): break
                    if key in (ord("r"), ord("s")) and not args.source:
                        try: command("record_event" if key == ord("r") else "capture")
                        except Exception as exc: print(exc)
                else:
                    stop.wait(.1)
    finally:
        stop.set()
        for thread in threads: thread.join(7)
        cv2.destroyAllWindows()
        if os.name == "nt":
            ctypes.windll.kernel32.SetThreadExecutionState(0x80000000)
        with lock:
            report = {k:v for k,v in state.items() if k not in ("frame","preview")}
        report.update(duration_s=time.monotonic()-started, workers_stopped=all(not t.is_alive() for t in threads),
                      samples=len(metrics_seen), source=url,
                      observed_seconds=observed_seconds, clock_gaps=clock_gaps)
        steady = metrics_seen[12:] or metrics_seen
        if steady:
            report["steady_rss_start_mb"] = steady[0]["rss_mb"]
            report["steady_rss_end_mb"] = steady[-1]["rss_mb"]
            report["steady_rss_max_mb"] = max(m["rss_mb"] for m in steady)
            report["fd_start"] = steady[0].get("fd_count")
            report["fd_end"] = steady[-1].get("fd_count")
        expected_reconnects = min(args.reconnect_count, int(args.seconds / args.reconnect_every)) if args.seconds and args.reconnect_every else 0
        report["passed"] = bool(report["frames"]) and report["workers_stopped"] and not clock_gaps and report["read_errors"] == 0 and report["control_errors"] == 0
        if args.seconds and not local_file:
            report["passed"] = report["passed"] and observed_seconds >= args.seconds - 1 and report["reconnects"] >= expected_reconnects
        (out / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        print(json.dumps({k:report[k] for k in ("duration_s","frames","reconnects","read_errors","control_errors","workers_stopped")},indent=2))
    return 0 if report["passed"] else 1

if __name__ == "__main__":
    raise SystemExit(main())
