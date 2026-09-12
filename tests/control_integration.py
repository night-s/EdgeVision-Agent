#!/usr/bin/env python3
"""Run against a started edge_agent (real or synthetic capture)."""
import json
import socket
import sys
import time
sys.path.insert(0, "tools")
from edge_client import request

def run(port):
    with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
        s.settimeout(5)
        stream = s.makefile("rb")
        # Split one command, coalesce another: order and IDs survive TCP boundaries.
        s.sendall(b'{"id":1,"cmd":"get_')
        time.sleep(.02)
        s.sendall(b'status"}\n{"id":2,"cmd":"get_status"}\n')
        assert json.loads(stream.readline())["id"] == 1
        assert json.loads(stream.readline())["id"] == 2
        for invalid in [b'[]\n', b'{bad}\n', b'{"skill_name":123}\n',
                        b'{"cmd":"set_threshold","value":1.5}\n']:
            s.sendall(invalid)
            assert json.loads(stream.readline())["status"] == "error"
        s.sendall(b'{"id":3,"cmd":"set_threshold","value":0.6}\n')
        assert json.loads(stream.readline())["code"] == 0
        s.sendall(b'{"id":3,"cmd":"set_threshold","value":0.2}\n')
        assert json.loads(stream.readline())["data"]["threshold"] > .59
    assert request("127.0.0.1", port, {"cmd": "get_status"})["data"]["threshold"] > .59
    assert request("127.0.0.1", port, {"cmd": "stop"})["code"] == 0
    assert request("127.0.0.1", port, {"cmd": "start"})["code"] == 0
    result = request("127.0.0.1", port, {"cmd": "capture"})
    assert result["status"] == "completed", result
    assert result["data"]["path"].endswith(".jpg")
    with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
        s.settimeout(3)
        s.sendall(b"x" * 9000)
        try:
            assert s.recv(1) == b""
        except ConnectionResetError:
            pass
    assert request("127.0.0.1", port, {"cmd": "get_status"})["code"] == 0
    with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
        s.settimeout(5)
        s.sendall(b'{"id":91,"cmd":"get_status"}\n{"id":92,"cmd":"capture"}\n')
        s.shutdown(socket.SHUT_WR)
        replies = [json.loads(line) for line in s.makefile("rb")]
        assert replies[0]["id"] == 91, replies
        assert replies[-1]["id"] == 92 and replies[-1]["status"] == "completed", replies
    print("fragmentation/coalescing/schema/replay/capture/oversize/reconnect/async-half-close passed")
if __name__ == "__main__":
    run(int(sys.argv[1]) if len(sys.argv) > 1 else 9000)
