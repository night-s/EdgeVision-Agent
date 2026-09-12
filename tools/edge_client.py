#!/usr/bin/env python3
"""Line-delimited control client. Capture waits for completed/error, not accepted."""
import argparse
import json
import socket
import time

def request(host, port, command, timeout=10):
    command = dict(command)
    command.setdefault("id", time.time_ns())
    with socket.create_connection((host, port), timeout=timeout) as sock:
        sock.settimeout(timeout)
        sock.sendall((json.dumps(command) + "\n").encode())
        with sock.makefile("rb") as stream:
            while True:
                line = stream.readline(65538)
                if not line:
                    raise ConnectionError("edge closed before final response")
                if len(line) > 65536:
                    raise ValueError("edge response too large")
                response = json.loads(line)
                if response.get("id") != command["id"]:
                    continue
                if response.get("status") != "accepted" or command.get("cmd") in ("record_event", "restart_camera"):
                    return response

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=["get_status", "get_metrics", "get_detections",
                                          "get_events", "set_infer_delay", "capture", "start", "stop", "set_threshold", "record_event", "restart_camera"])
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9000)
    parser.add_argument("--value", type=float)
    args = parser.parse_args()
    cmd = {"cmd": args.command}
    if args.value is not None:
        cmd["value"] = int(args.value) if args.command == "set_infer_delay" else args.value
    print(json.dumps(request(args.host, args.port, cmd), ensure_ascii=False, indent=2))
