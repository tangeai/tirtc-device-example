#!/usr/bin/env python3
"""Capture one or more ESP32 UART log ports without toggling reset lines."""

from __future__ import annotations

import argparse
import json
import time
from datetime import datetime, timezone
from pathlib import Path

import serial


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")


def open_without_reset(port: str, baudrate: int) -> serial.Serial:
    device = serial.Serial()
    device.port = port
    device.baudrate = baudrate
    device.timeout = 0.02
    device.write_timeout = 1
    device.dtr = False
    device.rts = False
    device.open()
    return device


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", action="append", required=True)
    parser.add_argument("--baudrate", type=int, default=115200)
    parser.add_argument("--duration", type=float, default=30.0)
    parser.add_argument("--artifact-dir", type=Path, required=True)
    args = parser.parse_args()

    args.artifact_dir.mkdir(parents=True, exist_ok=True)
    devices: dict[str, serial.Serial] = {}
    raw_files = {}
    buffers: dict[str, bytearray] = {}
    events_path = args.artifact_dir / "uart-events.jsonl"

    try:
        for port in args.port:
            devices[port] = open_without_reset(port, args.baudrate)
            raw_files[port] = (args.artifact_dir / f"{port}.raw.log").open("wb")
            buffers[port] = bytearray()

        deadline = time.monotonic() + args.duration
        with events_path.open("w", encoding="utf-8", newline="\n") as events:
            while time.monotonic() < deadline:
                had_data = False
                for port, device in devices.items():
                    data = device.read(max(1, device.in_waiting))
                    if not data:
                        continue
                    had_data = True
                    raw_files[port].write(data)
                    buffers[port].extend(data)
                    while b"\n" in buffers[port]:
                        raw_line, _, remainder = buffers[port].partition(b"\n")
                        buffers[port] = bytearray(remainder)
                        event = {
                            "time": utc_now(),
                            "port": port,
                            "text": raw_line.rstrip(b"\r").decode(
                                "utf-8", errors="replace"
                            ),
                        }
                        events.write(json.dumps(event, ensure_ascii=False) + "\n")
                        events.flush()
                if not had_data:
                    time.sleep(0.01)
    finally:
        for raw_file in raw_files.values():
            raw_file.close()
        for device in devices.values():
            device.close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
