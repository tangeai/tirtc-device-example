#!/usr/bin/env python3
"""Read-only host dashboard for a Junzheng G32S10X device."""

from __future__ import annotations

import argparse
import ipaddress
import json
import re
import sys
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse


READY_IP_RE = re.compile(r"\[screen_debug\] ready ip=(\d+\.\d+\.\d+\.\d+)")
COM_PORT_RE = re.compile(r"COM\d+", re.IGNORECASE)
MAX_LOG_BYTES = 1024 * 1024


def usable_ipv4(value: str) -> bool:
    try:
        address = ipaddress.ip_address(value)
    except ValueError:
        return False
    return address.version == 4 and not address.is_loopback and not address.is_unspecified


class DeviceContext:
    def __init__(
        self, building_dir: Path, device_ip: str | None, device_port: int, com_port: str
    ) -> None:
        if device_ip and not usable_ipv4(device_ip):
            raise ValueError(f"Invalid non-loopback device IPv4 address: {device_ip}")
        if not COM_PORT_RE.fullmatch(com_port):
            raise ValueError(f"Invalid COM port: {com_port}")
        self.building_dir = building_dir
        self.explicit_device_ip = device_ip
        self.device_ip = self.explicit_device_ip
        self.device_port = device_port
        self.com_port = com_port.upper()
        self.lock = threading.Lock()

    def latest_serial_log(self) -> Path | None:
        logs = list(self.building_dir.glob(f"g32_{self.com_port}_*.log"))
        return max(logs, key=lambda item: item.stat().st_mtime, default=None)

    def discover_device_ip(self) -> str | None:
        if self.explicit_device_ip:
            return self.explicit_device_ip
        logs = sorted(
            self.building_dir.glob(f"g32_{self.com_port}_*.log"),
            key=lambda item: item.stat().st_mtime,
            reverse=True,
        )
        for log in logs:
            try:
                text = log.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            for match in reversed(READY_IP_RE.findall(text)):
                if usable_ipv4(match):
                    with self.lock:
                        self.device_ip = match
                    return match
        return None

    def current_device_ip(self) -> str | None:
        with self.lock:
            current = self.device_ip
        return current or self.discover_device_ip()

    def fetch_device(self, path: str, timeout: float = 5.0) -> tuple[bytes, str]:
        device_ip = self.current_device_ip()
        if not device_ip:
            raise RuntimeError("No non-loopback G32 device address is available")
        url = f"http://{device_ip}:{self.device_port}{path}"
        request = urllib.request.Request(url, headers={"Cache-Control": "no-cache"})
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return response.read(), response.headers.get_content_type()

    def read_log_tail(self, line_limit: int) -> dict[str, object]:
        log = self.latest_serial_log()
        if log is None:
            return {"file": None, "updated_at": None, "lines": []}
        try:
            with log.open("rb") as stream:
                stream.seek(0, 2)
                size = stream.tell()
                start = max(0, size - MAX_LOG_BYTES)
                stream.seek(start)
                payload = stream.read()
            if start:
                split = payload.find(b"\n")
                payload = payload[split + 1 :] if split >= 0 else b""
            lines = payload.decode("utf-8", errors="replace").splitlines()[-line_limit:]
            stat = log.stat()
            return {
                "file": log.name,
                "updated_at": time.strftime("%Y-%m-%dT%H:%M:%S%z", time.localtime(stat.st_mtime)),
                "size": stat.st_size,
                "lines": lines,
            }
        except OSError as exc:
            return {"file": log.name, "updated_at": None, "lines": [], "error": str(exc)}


class ConsoleHandler(BaseHTTPRequestHandler):
    server_version = "G32DeviceConsole/1.0"

    @property
    def context(self) -> DeviceContext:
        return self.server.context  # type: ignore[attr-defined]

    @property
    def web_root(self) -> Path:
        return self.server.web_root  # type: ignore[attr-defined]

    def log_message(self, fmt: str, *args: object) -> None:
        sys.stdout.write("[%s] %s\n" % (self.log_date_time_string(), fmt % args))
        sys.stdout.flush()

    def send_bytes(self, status: int, content_type: str, payload: bytes) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(payload)

    def send_json(self, status: int, payload: dict[str, object]) -> None:
        data = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        self.send_bytes(status, "application/json; charset=utf-8", data)

    def send_static(self, name: str, content_type: str) -> None:
        path = self.web_root / name
        try:
            payload = path.read_bytes()
        except OSError:
            self.send_json(404, {"ok": False, "error": "Static asset not found"})
            return
        self.send_bytes(200, content_type, payload)

    def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler contract
        parsed = urlparse(self.path)
        path = parsed.path
        if path in ("/", "/index.html"):
            self.send_static("index.html", "text/html; charset=utf-8")
            return
        if path == "/app.js":
            self.send_static("app.js", "text/javascript; charset=utf-8")
            return
        if path == "/styles.css":
            self.send_static("styles.css", "text/css; charset=utf-8")
            return
        if path == "/favicon.ico":
            self.send_bytes(204, "image/x-icon", b"")
            return
        if path == "/api/health":
            self.send_json(
                200,
                {
                    "ok": True,
                    "service": "g32-device-console",
                    "device_ip": self.context.current_device_ip(),
                    "device_port": self.context.device_port,
                    "com_port": self.context.com_port,
                },
            )
            return
        if path == "/api/logs":
            query = parse_qs(parsed.query)
            try:
                line_limit = int(query.get("tail", ["500"])[0])
            except ValueError:
                line_limit = 500
            line_limit = max(20, min(line_limit, 2000))
            payload = self.context.read_log_tail(line_limit)
            payload["ok"] = True
            self.send_json(200, payload)
            return
        if path == "/api/device/status":
            try:
                payload, _ = self.context.fetch_device("/api/status", timeout=3.0)
                status = json.loads(payload.decode("utf-8"))
                status["console_ok"] = True
                self.send_json(200, status)
            except (OSError, RuntimeError, ValueError, urllib.error.URLError) as exc:
                self.send_json(502, {"ok": False, "error": str(exc)})
            return
        if path == "/api/device/screen.bmp":
            try:
                payload, content_type = self.context.fetch_device("/screen.bmp", timeout=8.0)
                if len(payload) < 54 or payload[:2] != b"BM":
                    raise ValueError("Device returned an invalid BMP frame")
                self.send_bytes(200, content_type or "image/bmp", payload)
            except (OSError, RuntimeError, ValueError, urllib.error.URLError) as exc:
                self.send_json(502, {"ok": False, "error": str(exc)})
            return
        self.send_json(404, {"ok": False, "error": "Not found"})


class ConsoleServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, address: tuple[str, int], context: DeviceContext, web_root: Path) -> None:
        super().__init__(address, ConsoleHandler)
        self.context = context
        self.web_root = web_root


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8787)
    parser.add_argument("--device-ip")
    parser.add_argument("--device-port", type=int, default=8080)
    parser.add_argument("--com-port", default="COM39")
    parser.add_argument("--building-dir", type=Path, required=True)
    parser.add_argument("--web-root", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    building_dir = args.building_dir.resolve()
    web_root = args.web_root.resolve()
    building_dir.mkdir(parents=True, exist_ok=True)
    if not web_root.is_dir():
        raise SystemExit(f"Web root does not exist: {web_root}")
    context = DeviceContext(building_dir, args.device_ip, args.device_port, args.com_port)
    server = ConsoleServer((args.host, args.port), context, web_root)
    print(
        f"G32_DEVICE_CONSOLE_READY url=http://{args.host}:{args.port}/ "
        f"device={context.current_device_ip() or 'waiting'}",
        flush=True,
    )
    try:
        server.serve_forever(poll_interval=0.25)
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
