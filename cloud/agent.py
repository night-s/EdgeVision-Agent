#!/usr/bin/env python3
"""Optional natural-language control adapter. Requires trusted network access."""
import json
import logging
import os
from pathlib import Path
import sys
from http.server import HTTPServer, BaseHTTPRequestHandler
import requests

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from edge_client import request as edge_request

KEY = os.environ.get("DEEPSEEK_API_KEY", "")
MODEL = os.environ.get("DEEPSEEK_MODEL", "deepseek-chat")
EDGE_HOST = os.environ.get("EDGE_HOST", "127.0.0.1")
EDGE_PORT = int(os.environ.get("EDGE_PORT", "9000"))
SKILLS = {"CaptureSkill": "capture", "QuerySkill": "get_status"}
SYSTEM = """将用户指令解析成一行JSON。只支持：
CaptureSkill：抓拍当前照片。
QuerySkill：查询设备状态。
不支持的请求（包括录像）返回none。
格式：{"skill_name":"CaptureSkill","reason":"用户要求抓拍"}。"""

def call_deepseek(prompt):
    if not KEY:
        raise ValueError("DEEPSEEK_API_KEY not configured")
    response = requests.post("https://api.deepseek.com/v1/chat/completions",
        headers={"Authorization": "Bearer " + KEY},
        json={"model": MODEL, "messages": [{"role": "system", "content": SYSTEM},
              {"role": "user", "content": prompt}], "temperature": 0.1, "max_tokens": 128},
        timeout=15)
    response.raise_for_status()
    content = response.json()["choices"][0]["message"]["content"].strip()
    if content.startswith("```"):
        content = "\n".join(line for line in content.splitlines() if not line.startswith("```"))
    command = json.loads(content)
    if not isinstance(command, dict) or command.get("skill_name") not in (*SKILLS, "none"):
        raise ValueError("unsupported model command")
    return command

class Handler(BaseHTTPRequestHandler):
    def respond(self, status, body):
        data = json.dumps(body, ensure_ascii=False).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path != "/api/health":
            return self.respond(404, {"status": "error", "reason": "not found"})
        try:
            reply = edge_request(EDGE_HOST, EDGE_PORT, {"cmd": "get_status"}, timeout=3)
            self.respond(200, {"status": "ok", "deepseek_configured": bool(KEY), "edge": reply})
        except (OSError, ValueError) as error:
            self.respond(503, {"status": "error", "reason": str(error)})

    def do_POST(self):
        if self.path not in ("/api/command", "/api/query"):
            return self.respond(404, {"status": "error", "reason": "not found"})
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if not 0 < length <= 16384:
                return self.respond(413, {"status": "error", "reason": "invalid body size"})
            self.connection.settimeout(5)
            body = json.loads(self.rfile.read(length))
            if not isinstance(body, dict):
                raise ValueError("body must be an object")
            if self.path == "/api/query":
                reply = edge_request(EDGE_HOST, EDGE_PORT, {"cmd": "get_status"})
                return self.respond(200 if reply["code"] == 0 else 502, reply)
            prompt = body.get("prompt")
            if not isinstance(prompt, str) or not prompt.strip() or len(prompt) > 4096:
                raise ValueError("prompt must be a nonempty string up to 4096 characters")
            command = call_deepseek(prompt)
            if command["skill_name"] == "none":
                return self.respond(422, {"status": "error", "command": command})
            reply = edge_request(EDGE_HOST, EDGE_PORT, {"cmd": SKILLS[command["skill_name"]]})
            self.respond(200 if reply["code"] == 0 else 502,
                         {"status": reply["status"], "command": command, "edge_reply": reply})
        except (ValueError, KeyError, IndexError, TypeError) as error:
            self.respond(400, {"status": "error", "reason": str(error)})
        except (OSError, requests.RequestException) as error:
            self.respond(502, {"status": "error", "reason": str(error)})

def main():
    logging.basicConfig(level=logging.INFO)
    server = HTTPServer((os.environ.get("LISTEN_HOST", "127.0.0.1"),
                         int(os.environ.get("LISTEN_PORT", "8000"))), Handler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()

if __name__ == "__main__":
    main()
