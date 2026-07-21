#!/usr/bin/env python3
"""
EdgeVision-Agent Cloud Microservice
====================================
桥梁服务：对接 DeepSeek API，将自然语言指令转换为 JSON RPC 并下发至边缘端。

API:
  GET  /api/health  → 健康检查
  POST /api/command → 解析自然语言 → DeepSeek API → TCP 下发边缘端
  POST /api/query   → 直接调用 DeepSeek API（不下发边缘）

环境变量:
  DEEPSEEK_API_KEY  (必填)
  DEEPSEEK_MODEL    (默认 deepseek-chat)
  EDGE_HOST         (默认 127.0.0.1)
  EDGE_PORT         (默认 9000)
  LISTEN_PORT       (默认 8000)
"""

import os
import sys
import json
import time
import socket
import logging
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse

import requests

# ── 日志 ──
logging.basicConfig(
    level=logging.INFO,
    format="[%(asctime)s] %(levelname)s %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger("cloud-agent")

# ── 配置 ──
DEEPSEEK_API_KEY = os.environ.get("DEEPSEEK_API_KEY", "")
DEEPSEEK_MODEL = os.environ.get("DEEPSEEK_MODEL", "deepseek-chat")
DEEPSEEK_BASE_URL = "https://api.deepseek.com/v1"

EDGE_HOST = os.environ.get("EDGE_HOST", "127.0.0.1")
EDGE_PORT = int(os.environ.get("EDGE_PORT", "9000"))
LISTEN_PORT = int(os.environ.get("LISTEN_PORT", "8000"))

DEEPSEEK_TIMEOUT = 15  # 秒

SYSTEM_PROMPT = """你是一个边缘AI设备的命令解析器。你的任务是将用户的自然语言指令转换为Json格式的命令。

可用的技能：
- CaptureSkill: 抓拍照片、录像、保存当前画面
- QuerySkill: 查询状态、询问检测结果、查看系统信息

输出必须是严格的JSON格式（只输出一行，不要包含说明文字或markdown）：
{"skill_name": "CaptureSkill", "reason": "用户要求抓拍"}
{"skill_name": "none", "reason": "未检测到有效指令"}"""


# ── DeepSeek API 调用 ──
def call_deepseek(user_prompt: str) -> dict:
    """调用 DeepSeek API，返回解析后的 JSON 命令字典。"""
    if not DEEPSEEK_API_KEY:
        return {"skill_name": "none", "reason": "DEEPSEEK_API_KEY 未设置"}

    headers = {
        "Authorization": f"Bearer {DEEPSEEK_API_KEY}",
        "Content-Type": "application/json",
    }

    payload = {
        "model": DEEPSEEK_MODEL,
        "messages": [
            {"role": "system", "content": SYSTEM_PROMPT},
            {"role": "user", "content": user_prompt},
        ],
        "temperature": 0.1,  # 低温度确保输出稳定
        "max_tokens": 128,
    }

    try:
        resp = requests.post(
            f"{DEEPSEEK_BASE_URL}/chat/completions",
            headers=headers,
            json=payload,
            timeout=DEEPSEEK_TIMEOUT,
        )
        resp.raise_for_status()
        data = resp.json()
        content = data["choices"][0]["message"]["content"].strip()
        log.info("DeepSeek response: %s", content)

        # 尝试剥离 markdown 代码块标记
        if content.startswith("```"):
            lines = content.splitlines()
            content = "\n".join(
                line for line in lines
                if not line.strip().startswith("```")
            ).strip()

        cmd = json.loads(content)
        if "skill_name" not in cmd:
            cmd["skill_name"] = "none"
        if "reason" not in cmd:
            cmd["reason"] = "解析成功"
        return cmd

    except requests.exceptions.Timeout:
        log.error("DeepSeek API timeout")
        return {"skill_name": "none", "reason": "API 请求超时"}
    except requests.exceptions.ConnectionError as e:
        log.error("DeepSeek connection error: %s", e)
        return {"skill_name": "none", "reason": f"连接失败: {e}"}
    except requests.exceptions.HTTPError as e:
        log.error("DeepSeek HTTP error: %s", e)
        return {"skill_name": "none", "reason": f"HTTP错误: {e}"}
    except (json.JSONDecodeError, KeyError, IndexError) as e:
        log.error("DeepSeek response parse error: %s", e)
        return {"skill_name": "none", "reason": f"响应解析失败: {e}"}


# ── TCP 边缘通信 ──
def send_to_edge(command: dict) -> dict:
    """通过 TCP 将 JSON RPC 命令发送至边缘端，返回边缘响应。"""
    payload = json.dumps(command, ensure_ascii=False) + "\n"
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(5.0)

    try:
        sock.connect((EDGE_HOST, EDGE_PORT))
        log.info("Sending to edge %s:%d: %s", EDGE_HOST, EDGE_PORT, payload.strip())
        sock.sendall(payload.encode("utf-8"))

        # 读取响应（新行分隔）
        buf = b""
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            buf += chunk
            if b"\n" in buf:
                break

        response_text = buf.decode("utf-8").strip()
        log.info("Edge response: %s", response_text)

        try:
            return json.loads(response_text)
        except json.JSONDecodeError:
            return {"status": "ok", "raw": response_text}

    except socket.timeout:
        log.error("TCP timeout connecting to edge")
        return {"status": "error", "reason": "边缘连接超时"}
    except ConnectionRefusedError:
        log.error("Edge connection refused on %s:%d", EDGE_HOST, EDGE_PORT)
        return {"status": "error", "reason": "边缘连接被拒绝"}
    except OSError as e:
        log.error("Socket error: %s", e)
        return {"status": "error", "reason": str(e)}
    finally:
        sock.close()


# ── HTTP 请求处理器 ──
class Handler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        log.info("%s - %s", self.client_address[0], format % args)

    def _send_json(self, status_code: int, data: dict):
        body = json.dumps(data, ensure_ascii=False).encode("utf-8")
        self.send_response(status_code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _read_body(self) -> str:
        length = int(self.headers.get("Content-Length", 0))
        return self.rfile.read(length).decode("utf-8") if length > 0 else "{}"

    # ── 路由 ──

    def do_GET(self):
        parsed = urlparse(self.path)

        if parsed.path == "/api/health":
            # 快速测试边缘是否可达
            edge_ok = False
            try:
                s = socket.create_connection((EDGE_HOST, EDGE_PORT), timeout=1.0)
                s.close()
                edge_ok = True
            except OSError:
                pass

            self._send_json(200, {
                "status": "ok",
                "service": "edge-vision-cloud",
                "deepseek_configured": bool(DEEPSEEK_API_KEY),
                "edge_connected": edge_ok,
                "edge_host": EDGE_HOST,
                "edge_port": EDGE_PORT,
            })

        else:
            self._send_json(404, {"status": "error", "reason": "not found"})

    def do_POST(self):
        parsed = urlparse(self.path)

        try:
            body = json.loads(self._read_body())
        except json.JSONDecodeError:
            self._send_json(400, {"status": "error", "reason": "invalid JSON body"})
            return

        if parsed.path == "/api/command":
            prompt = body.get("prompt", "")
            if not prompt:
                self._send_json(400, {"status": "error", "reason": "missing 'prompt' field"})
                return

            # 1. 调用 DeepSeek 解析自然语言
            command = call_deepseek(prompt)

            # 2. 如果解析出有效命令，下发到边缘端
            edge_reply = None
            if command.get("skill_name") and command["skill_name"] != "none":
                edge_reply = send_to_edge(command)

            self._send_json(200, {
                "status": "ok",
                "command": command,
                "edge_reply": edge_reply,
            })

        elif parsed.path == "/api/query":
            prompt = body.get("prompt", "")
            if not prompt:
                self._send_json(400, {"status": "error", "reason": "missing 'prompt' field"})
                return

            # 直接调用 DeepSeek，不进行结构化解析
            command = call_deepseek(prompt)
            self._send_json(200, {
                "status": "ok",
                "result": command,
            })

        else:
            self._send_json(404, {"status": "error", "reason": "not found"})


def main():
    if not DEEPSEEK_API_KEY:
        log.warning("DEEPSEEK_API_KEY 未设置 — AI 功能将不可用")

    server = HTTPServer(("0.0.0.0", LISTEN_PORT), Handler)
    log.info("Cloud Agent listening on http://0.0.0.0:%d", LISTEN_PORT)
    log.info("Edge target: %s:%d", EDGE_HOST, EDGE_PORT)

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        log.info("Shutting down...")
        server.server_close()


if __name__ == "__main__":
    main()
