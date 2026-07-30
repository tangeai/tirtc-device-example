#!/usr/bin/env python3
"""Small serial harness for repeatable AT and two-board regression evidence."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
import threading
import time
import uuid
from collections import deque
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable, Iterable

import serial
from serial import SerialException


FINAL_RE = re.compile(r"^(OK|ERROR:-?\d+)$")
SENSITIVE_COMMAND_ANY_RE = re.compile(
    r"(?:AT\+WIFI\s*=|AT\+AIACTION\s*=|AT\+AI\s*=\s*UPDATE\b).*",
    re.IGNORECASE,
)
DEVICE_ID_RE = re.compile(r"\bTIR[A-Z0-9]{8,}\b")
ROOM_ID_RE = re.compile(r"\bd_roomid_[A-Za-z0-9_-]+\b")
AI_SESSION_ID_RE = re.compile(r"\baivoice-[A-Za-z0-9-]+\b")
IPV4_RE = re.compile(
    r"(?<![0-9])(?:[0-9]{1,3}\.){3}[0-9]{1,3}(?![0-9])"
)
MAC_RE = re.compile(r"\b(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}\b")
SENSITIVE_METADATA_KEYS = {
    "ssid",
    "wifi_ssid",
    "caption",
    "caption_text",
    "utterance_id",
    "action_id",
    "ai_action_id",
    "action_json",
    "action_payload",
    "bind_code",
    "verification_code",
    "device_id",
    "peer_id",
    "room_id",
    "session_id",
    "ai_session_id",
    "device_key",
    "mqtt_token",
    "temp_token",
    "ai_token",
    "connection_token",
    "operation_payload",
    "payload",
    "result",
    "extra_params",
}
STRUCTURED_REDACTION_MARKERS = (
    "+STATUS:",
    "+WIFI:",
    "+BIND:",
    "+SESSION:",
    "+AI:CAPTION,",
    "+AI:ACTION,",
    "+AI:EVENT,",
    "+AI:OP,",
)
EVENT_CAPACITY = 32768


def utc_timestamp() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")


def quote_at(value: str) -> str:
    escaped = (
        value.replace("\\", "\\\\")
        .replace('"', '\\"')
        .replace("\r", "\\r")
        .replace("\n", "\\n")
        .replace("\t", "\\t")
    )
    return f'"{escaped}"'


@dataclass(frozen=True)
class AtLine:
    port: str
    transport_epoch: int
    sequence: int
    monotonic: float
    wall_time: str
    text: str


@dataclass(frozen=True)
class AtCommandResult:
    command: str
    transport_epoch: int
    before: int
    after: int
    lines: tuple[AtLine, ...]

    @property
    def request_id(self) -> int | None:
        for line in self.lines:
            fields = parse_csv_urc(line.text, "+REQUEST,")
            if fields is not None and len(fields) >= 2:
                try:
                    return int(fields[1])
                except ValueError:
                    return None
        return None


@dataclass(frozen=True)
class SystemEvent:
    transport_epoch: int
    sequence: int
    generation: int
    status: int
    name: str
    detail: str


class SerialTransportError(RuntimeError):
    """The port stopped while a command or event wait was in progress."""


class HistoryLostError(RuntimeError):
    """The requested cursor has fallen behind the retained event history."""


class RequestRejectedError(RuntimeError):
    """The AT parser accepted a command but the controller rejected its intent."""


@dataclass(frozen=True)
class RequestHandle:
    port: str
    transport_epoch: int
    app_generation: int
    request_id: int
    operation: str
    cursor: int


def parse_csv_urc(text: str, prefix: str) -> list[str] | None:
    if not text.startswith(prefix):
        return None
    fields: list[str] = []
    field: list[str] = []
    quoted = False
    escaped = False
    escape_map = {"r": "\r", "n": "\n", "t": "\t", '"': '"', "\\": "\\"}
    for character in text[len(prefix) :]:
        if escaped:
            replacement = escape_map.get(character)
            if replacement is None:
                return None
            field.append(replacement)
            escaped = False
        elif character == "\\":
            escaped = True
        elif character == '"':
            quoted = not quoted
        elif character == "," and not quoted:
            fields.append("".join(field))
            field.clear()
        else:
            field.append(character)
    if quoted or escaped:
        return None
    fields.append("".join(field))
    return fields


def replace_csv_fields(
    text: str,
    prefix: str,
    replacements: dict[int, str],
    quoted_fields: set[int],
) -> str:
    fields = parse_csv_urc(text, prefix)
    if fields is None:
        return text
    for index, replacement in replacements.items():
        if -len(fields) <= index < len(fields):
            fields[index] = replacement
    rendered = [
        quote_at(value) if index in quoted_fields else value
        for index, value in enumerate(fields)
    ]
    return prefix + ",".join(rendered)


def redact_rx(text: str) -> str:
    text = SENSITIVE_COMMAND_ANY_RE.sub("<redacted-sensitive-command>", text)
    if not text.startswith(STRUCTURED_REDACTION_MARKERS):
        marker_positions = [
            position
            for marker in STRUCTURED_REDACTION_MARKERS
            if (position := text.find(marker)) > 0
        ]
        if marker_positions:
            marker_position = min(marker_positions)
            return text[:marker_position] + redact_rx(text[marker_position:])
    if text.startswith("+STATUS:"):
        fields = parse_csv_urc(text, "+STATUS:")
        text = (
            replace_csv_fields(
                text,
                "+STATUS:",
                {7: "<redacted-ssid>", 8: "<redacted-ip>"},
                {7, 8, 9},
            )
            if fields is not None and len(fields) > 9
            else "+STATUS:<redacted-malformed>"
        )
    elif text.startswith("+WIFI:"):
        fields = parse_csv_urc(text, "+WIFI:")
        if fields is not None and len(fields) == 4:
            if fields[0] in ("0", "1"):
                replacements = {
                    2: "<redacted-ssid>",
                    3: "<redacted-ip>",
                }
                quoted = {2, 3}
            else:
                replacements = {
                    2: (
                        "<redacted-ssid>"
                        if fields[0] == "CONNECTING"
                        else "<redacted-network-detail>"
                    )
                }
                quoted = {2, 3}
            text = replace_csv_fields(
                text, "+WIFI:", replacements, quoted
            )
        else:
            text = "+WIFI:<redacted-malformed>"
    elif text.startswith("+BIND:"):
        fields = parse_csv_urc(text, "+BIND:")
        if fields:
            text = replace_csv_fields(
                text,
                "+BIND:",
                {len(fields) - 1: "<redacted-binding-detail>"},
                {len(fields) - 1},
            )
        else:
            text = "+BIND:<redacted-malformed>"
    elif text.startswith("+SESSION:"):
        fields = parse_csv_urc(text, "+SESSION:")
        text = (
            replace_csv_fields(
                text,
                "+SESSION:",
                {17: "<redacted-action-id>"},
                {11, 12, 13, 17},
            )
            if fields is not None and len(fields) > 17
            else "+SESSION:<redacted-malformed>"
        )
    elif text.startswith("+AI:CAPTION,"):
        fields = parse_csv_urc(text, "+AI:CAPTION,")
        text = (
            replace_csv_fields(
                text,
                "+AI:CAPTION,",
                {
                    5: "<redacted-utterance>",
                    7: "<redacted-caption>",
                },
                {5, 7},
            )
            if fields is not None and len(fields) > 7
            else "+AI:CAPTION,<redacted-malformed>"
        )
    elif text.startswith("+AI:ACTION,"):
        fields = parse_csv_urc(text, "+AI:ACTION,")
        text = (
            replace_csv_fields(
                text,
                "+AI:ACTION,",
                {
                    2: "<redacted-action-id>",
                    3: "<redacted-action>",
                    4: "<redacted-json>",
                },
                {2, 3, 4},
            )
            if fields is not None and len(fields) > 4
            else "+AI:ACTION,<redacted-malformed>"
        )
    elif text.startswith("+AI:EVENT,") or text.startswith("+AI:OP,"):
        prefix = "+AI:EVENT," if text.startswith("+AI:EVENT,") else "+AI:OP,"
        fields = parse_csv_urc(text, prefix)
        if fields is not None and len(fields) == 6:
            text = replace_csv_fields(
                text,
                prefix,
                {len(fields) - 1: "<redacted-json>"},
                {3, 4, 5},
            )
        else:
            text = prefix + "<redacted-malformed>"
    text = DEVICE_ID_RE.sub("<redacted-device>", text)
    text = ROOM_ID_RE.sub("<redacted-room>", text)
    text = AI_SESSION_ID_RE.sub("<redacted-ai-session>", text)
    text = IPV4_RE.sub("<redacted-ip>", text)
    return MAC_RE.sub("<redacted-mac>", text)


def redact_evidence_value(value: object, key: str = "") -> object:
    if isinstance(value, str):
        if key.casefold() in SENSITIVE_METADATA_KEYS:
            return f"<redacted-{key.casefold().replace('_', '-')}>"
        return redact_rx(value)
    if isinstance(value, list):
        return [redact_evidence_value(item, key) for item in value]
    if isinstance(value, tuple):
        return [redact_evidence_value(item, key) for item in value]
    if isinstance(value, dict):
        return {
            str(child_key): redact_evidence_value(item, str(child_key))
            for child_key, item in value.items()
        }
    return value


class EvidenceWriter:
    def __init__(self, directory: Path):
        directory.mkdir(parents=True, exist_ok=True)
        self.path = directory / "serial-events.jsonl"
        if self.path.exists():
            raise FileExistsError(
                f"evidence already exists; choose a new artifact directory: "
                f"{self.path.resolve()}"
            )
        self.run_id = str(uuid.uuid4())
        self._file = self.path.open("x", encoding="utf-8", newline="\n")
        self._lock = threading.Lock()
        self._steps: list[dict[str, object]] = []
        self._records = 0

    def record(
        self,
        kind: str,
        port: str,
        text: str,
        **metadata: object,
    ) -> None:
        item = {
            "run_id": self.run_id,
            "time": utc_timestamp(),
            "kind": kind,
            "port": port,
            "text": redact_rx(text),
        }
        item.update(
            {
                key: redact_evidence_value(value, key)
                for key, value in metadata.items()
            }
        )
        with self._lock:
            if kind in ("request", "step", "scenario"):
                self._steps.append(dict(item))
            self._file.write(json.dumps(item, ensure_ascii=True) + "\n")
            self._file.flush()
            self._records += 1

    def record_rx(self, line: AtLine) -> None:
        self.record(
            "rx",
            line.port,
            redact_rx(line.text),
            time=line.wall_time,
            transport_epoch=line.transport_epoch,
            sequence=line.sequence,
            monotonic=line.monotonic,
        )

    def close(self) -> None:
        with self._lock:
            self._file.close()

    def steps(self) -> list[dict[str, object]]:
        with self._lock:
            return [dict(item) for item in self._steps]

    def identity(self) -> dict[str, object]:
        with self._lock:
            self._file.flush()
            data = self.path.read_bytes()
            return {
                "path": str(self.path.resolve()),
                "run_id": self.run_id,
                "size": len(data),
                "records": self._records,
                "sha256": hashlib.sha256(data).hexdigest(),
            }


class AtDevice:
    def __init__(
        self,
        port: str,
        evidence: EvidenceWriter,
        baudrate: int = 115200,
        planned_restart_details: Iterable[str] = (),
    ):
        self.port = port
        self.baudrate = baudrate
        self.evidence = evidence
        self._serial: serial.Serial | None = None
        self._stop = threading.Event()
        self._events: deque[AtLine] = deque(maxlen=EVENT_CAPACITY)
        self._condition = threading.Condition()
        self._next_sequence = 1
        self._transport_epoch = 0
        self._dropped_events = 0
        self._framing_errors = 0
        self._transport_failures = 0
        self._device_urc_overflows = 0
        self._expected_restarts = 0
        self._unexpected_restarts = 0
        self._restart_protocol_errors = 0
        self._expected_restart_events: list[SystemEvent] = []
        self._boot_events: list[SystemEvent] = []
        self._planned_restart_details = list(planned_restart_details)
        self._transport_error: Exception | None = None
        self._write_lock = threading.Lock()
        self._reader: threading.Thread | None = None

    def open(self, timeout: float = 15.0) -> None:
        deadline = time.monotonic() + timeout
        last_error: Exception | None = None
        while time.monotonic() < deadline:
            try:
                connection = serial.Serial(
                    port=None,
                    baudrate=self.baudrate,
                    timeout=0.1,
                    write_timeout=2.0,
                    exclusive=True,
                )
                # Keep native USB Serial/JTAG from seeing an intentional
                # DTR/RTS reset pulse when the evidence harness attaches.
                connection.dtr = False
                connection.rts = False
                connection.port = self.port
                connection.open()
                self._serial = connection
                self._stop.clear()
                with self._condition:
                    self._transport_error = None
                    self._transport_epoch += 1
                self._reader = threading.Thread(
                    target=self._reader_loop,
                    name=f"at-reader-{self.port}",
                    daemon=True,
                )
                self._reader.start()
                self.evidence.record(
                    "transport",
                    self.port,
                    f"opened:epoch={self._transport_epoch}",
                    transport_epoch=self._transport_epoch,
                )
                return
            except (OSError, SerialException) as exc:
                last_error = exc
                time.sleep(0.25)
        raise RuntimeError(f"{self.port}: cannot open serial port: {last_error}")

    def close(self) -> None:
        had_transport = self._serial is not None or self._reader is not None
        self._stop.set()
        if self._reader is not None:
            self._reader.join(timeout=1.0)
        if self._serial is not None:
            try:
                self._serial.close()
            except SerialException:
                pass
        self._serial = None
        self._reader = None
        if had_transport:
            self.evidence.record("transport", self.port, "closed")

    def recover(self, timeout: float = 15.0) -> None:
        """Reconnect without replaying the command that observed the failure."""
        self.close()
        self.open(timeout=timeout)
        self.synchronize(timeout=timeout)
        self.send("AT+STATUS?", timeout=3.0)
        self.send("AT+SESSION?", timeout=3.0)
        self.send("AT+MEDIA?", timeout=3.0)

    def _reader_loop(self) -> None:
        assert self._serial is not None
        pending = bytearray()
        while not self._stop.is_set():
            try:
                chunk = self._serial.read(256)
            except (OSError, SerialException) as exc:
                if self._stop.is_set():
                    return
                with self._condition:
                    self._transport_failures += 1
                    self._transport_error = exc
                    self._condition.notify_all()
                self.evidence.record(
                    "transport",
                    self.port,
                    f"read-error:{exc}",
                    transport_epoch=self.transport_epoch,
                )
                return
            if not chunk:
                continue
            pending.extend(chunk)
            while True:
                split = next(
                    (index for index, byte in enumerate(pending) if byte in (10, 13)),
                    None,
                )
                if split is None:
                    if len(pending) > 8192:
                        with self._condition:
                            self._framing_errors += 1
                        del pending[:-1024]
                        self.evidence.record(
                            "framing-error",
                            self.port,
                            "unterminated input exceeded 8192 bytes",
                            transport_epoch=self.transport_epoch,
                        )
                    break
                raw = bytes(pending[:split])
                delimiter = pending[split]
                del pending[: split + 1]
                if delimiter == 13 and pending[:1] == b"\n":
                    del pending[:1]
                text = raw.decode("utf-8", errors="replace").strip()
                if not text:
                    continue
                with self._condition:
                    if text.startswith("+AT:URC_OVERFLOW,"):
                        try:
                            self._device_urc_overflows += int(
                                text.rsplit(",", 1)[1]
                            )
                        except ValueError:
                            self._device_urc_overflows += 1
                    system = parse_csv_urc(text, "+SYSTEM:")
                    if system is not None and system:
                        system_name = system[0]
                        if system_name in ("RESTARTING", "BOOTING"):
                            valid = len(system) == 4
                            try:
                                generation = int(system[1]) if valid else 0
                                status = int(system[2]) if valid else -1
                            except ValueError:
                                generation = 0
                                status = -1
                                valid = False
                            valid = valid and generation > 0 and status == 0
                            detail = system[3] if len(system) == 4 else ""
                            parsed_event = SystemEvent(
                                transport_epoch=self._transport_epoch,
                                sequence=self._next_sequence,
                                generation=generation,
                                status=status,
                                name=system_name,
                                detail=detail,
                            )
                            if system_name == "RESTARTING":
                                planned = (
                                    valid
                                    and detail in self._planned_restart_details
                                )
                                if planned:
                                    self._planned_restart_details.remove(detail)
                                if (
                                    valid
                                    and (
                                        planned
                                        or detail
                                        == "tirtc_failed_connect_transport"
                                    )
                                ):
                                    self._expected_restarts += 1
                                    self._expected_restart_events.append(
                                        parsed_event
                                    )
                                else:
                                    self._unexpected_restarts += 1
                                    if not valid:
                                        self._restart_protocol_errors += 1
                            elif valid:
                                self._boot_events.append(parsed_event)
                            else:
                                self._restart_protocol_errors += 1
                    line = AtLine(
                        self.port,
                        self._transport_epoch,
                        self._next_sequence,
                        time.monotonic(),
                        utc_timestamp(),
                        text,
                    )
                    self._next_sequence += 1
                    if len(self._events) == EVENT_CAPACITY:
                        self._dropped_events += 1
                    self._events.append(line)
                    self._condition.notify_all()
                display = redact_rx(text)
                self.evidence.record_rx(line)
                print(f"[{self.port}] {display}", flush=True)

    def cursor(self) -> int:
        with self._condition:
            return self._next_sequence - 1

    @property
    def transport_epoch(self) -> int:
        with self._condition:
            return self._transport_epoch

    def checkpoint(self) -> tuple[int, int]:
        with self._condition:
            return self._transport_epoch, self._next_sequence - 1

    def health_snapshot(self) -> dict[str, int | bool]:
        with self._condition:
            return {
                "transport_epoch": self._transport_epoch,
                "reconnects": max(0, self._transport_epoch - 1),
                "first_sequence": (
                    self._events[0].sequence if self._events else self._next_sequence
                ),
                "last_sequence": self._next_sequence - 1,
                "dropped_events": self._dropped_events,
                "framing_errors": self._framing_errors,
                "transport_failures": self._transport_failures,
                "device_urc_overflows": self._device_urc_overflows,
                "expected_restarts": self._expected_restarts,
                "unexpected_restarts": self._unexpected_restarts,
                "restart_protocol_errors": self._restart_protocol_errors,
                "pending_planned_restarts": len(
                    self._planned_restart_details
                ),
                "boot_events": len(self._boot_events),
                "transport_stopped": self._transport_error is not None,
            }

    def expected_restart_events(self) -> tuple[SystemEvent, ...]:
        with self._condition:
            return tuple(self._expected_restart_events)

    def boot_events(self) -> tuple[SystemEvent, ...]:
        with self._condition:
            return tuple(self._boot_events)

    def assert_healthy(
        self,
        *,
        expected_restarts: int = 0,
        expected_transport_failures: int = 0,
        expected_reconnects: int = 0,
    ) -> None:
        health = self.health_snapshot()
        unhealthy = (
            health["dropped_events"] != 0
            or health["framing_errors"] != 0
            or health["transport_failures"] != expected_transport_failures
            or health["device_urc_overflows"] != 0
            or health["reconnects"] != expected_reconnects
            or health["unexpected_restarts"] != 0
            or health["restart_protocol_errors"] != 0
            or health["pending_planned_restarts"] != 0
            or health["expected_restarts"] != expected_restarts
            or health["transport_stopped"]
        )
        if unhealthy:
            raise RuntimeError(f"{self.port}: evidence transport is unhealthy: {health}")

    def _lines_after_locked(self, cursor: int) -> list[AtLine]:
        if self._events and cursor < self._events[0].sequence - 1:
            raise HistoryLostError(
                f"{self.port}: cursor {cursor} predates retained history "
                f"{self._events[0].sequence}; dropped={self._dropped_events}"
            )
        return [line for line in self._events if line.sequence > cursor]

    def _raise_transport_error_locked(self) -> None:
        if self._transport_error is not None:
            raise SerialTransportError(
                f"{self.port}: serial transport stopped: "
                f"{self._transport_error}"
            )

    def _raise_epoch_changed_locked(self, expected_epoch: int) -> None:
        if self._transport_epoch != expected_epoch:
            raise SerialTransportError(
                f"{self.port}: transport epoch changed from "
                f"{expected_epoch} to {self._transport_epoch}"
            )

    def send(self, command: str, timeout: float = 5.0) -> AtCommandResult:
        if self._serial is None or not self._serial.is_open:
            raise RuntimeError(f"{self.port}: serial port is not open")
        display = redact_rx(command)
        with self._write_lock:
            expected_epoch, before = self.checkpoint()
            self.evidence.record(
                "tx",
                self.port,
                display,
                transport_epoch=expected_epoch,
                before_sequence=before,
            )
            payload = (command + "\r\n").encode("utf-8")
            try:
                self._serial.write(payload)
                self._serial.flush()
            except (OSError, SerialException) as exc:
                raise SerialTransportError(
                    f"{self.port}: failed to write {display}: {exc}"
                ) from exc

            deadline = time.monotonic() + timeout
            scanned = before
            with self._condition:
                while time.monotonic() < deadline:
                    for line in self._lines_after_locked(scanned):
                        scanned = line.sequence
                        if line.transport_epoch != expected_epoch:
                            raise SerialTransportError(
                                f"{self.port}: response crossed transport epoch"
                            )
                        if FINAL_RE.match(line.text):
                            lines = tuple(self._lines_after_locked(before))
                            self.evidence.record(
                                "final",
                                self.port,
                                line.text,
                                transport_epoch=expected_epoch,
                                before_sequence=before,
                                final_sequence=line.sequence,
                                command=display,
                            )
                            if line.text != "OK":
                                raise RuntimeError(
                                    f"{self.port}: {display} returned {line.text}"
                                )
                            return AtCommandResult(
                                command=display,
                                transport_epoch=expected_epoch,
                                before=before,
                                after=line.sequence,
                                lines=tuple(
                                    item
                                    for item in lines
                                    if item.sequence <= line.sequence
                                ),
                            )
                    self._raise_transport_error_locked()
                    self._raise_epoch_changed_locked(expected_epoch)
                    remaining = deadline - time.monotonic()
                    if remaining > 0:
                        self._condition.wait(timeout=min(0.2, remaining))
        raise TimeoutError(f"{self.port}: no final response for {display}")

    def submit_intent(
        self,
        command: str,
        operation: str,
        timeout: float = 8.0,
    ) -> RequestHandle:
        """Submit an asynchronous intent and bind it to its controller request."""
        result = self.send(command, timeout=min(timeout, 5.0))

        def accepted_or_rejected(line: AtLine) -> bool:
            accepted = parse_csv_urc(line.text, "+REQUEST,")
            if (
                accepted is not None
                and len(accepted) >= 4
                and accepted[3] == operation
            ):
                return True
            rejected = parse_csv_urc(line.text, "+ERROR:")
            return (
                rejected is not None
                and len(rejected) >= 5
                and rejected[3] == "REQUEST_REJECTED"
                and rejected[4] == operation
            )

        line = self.wait_for(
            accepted_or_rejected,
            timeout=timeout,
            description=f"{operation} request acknowledgement",
            after=result.before,
            expected_epoch=result.transport_epoch,
        )
        rejected = parse_csv_urc(line.text, "+ERROR:")
        if rejected is not None:
            raise RequestRejectedError(
                f"{self.port}: {operation} rejected with code={rejected[2]}"
            )
        accepted = parse_csv_urc(line.text, "+REQUEST,")
        if accepted is None or len(accepted) < 4:
            raise RuntimeError(f"{self.port}: malformed {operation} acknowledgement")
        try:
            generation = int(accepted[0])
            request_id = int(accepted[1])
            status = int(accepted[2])
        except ValueError as exc:
            raise RuntimeError(
                f"{self.port}: malformed numeric fields in {operation} acknowledgement"
            ) from exc
        if status != 0:
            raise RuntimeError(
                f"{self.port}: {operation} acknowledgement status={status}"
            )
        handle = RequestHandle(
            port=self.port,
            transport_epoch=line.transport_epoch,
            app_generation=generation,
            request_id=request_id,
            operation=operation,
            cursor=line.sequence,
        )
        self.evidence.record(
            "request",
            self.port,
            operation,
            transport_epoch=handle.transport_epoch,
            sequence=handle.cursor,
            app_generation=handle.app_generation,
            request_id=handle.request_id,
        )
        return handle

    def wait_for(
        self,
        predicate: Callable[[AtLine], bool],
        timeout: float,
        description: str,
        after: int | None = None,
        expected_epoch: int | None = None,
    ) -> AtLine:
        current_epoch, current_cursor = self.checkpoint()
        epoch = current_epoch if expected_epoch is None else expected_epoch
        cursor = current_cursor if after is None else after
        deadline = time.monotonic() + timeout
        with self._condition:
            while time.monotonic() < deadline:
                for line in self._lines_after_locked(cursor):
                    cursor = line.sequence
                    if line.transport_epoch != epoch:
                        raise SerialTransportError(
                            f"{self.port}: event wait crossed transport epoch"
                        )
                    if predicate(line):
                        return line
                self._raise_transport_error_locked()
                self._raise_epoch_changed_locked(epoch)
                remaining = deadline - time.monotonic()
                if remaining > 0:
                    self._condition.wait(timeout=min(0.2, remaining))
        raise TimeoutError(f"{self.port}: timed out waiting for {description}")

    def history(self, after: int = 0) -> list[AtLine]:
        with self._condition:
            return self._lines_after_locked(after)

    def synchronize(self, timeout: float = 20.0) -> None:
        """Wait until the AT parser answers, without assuming app readiness."""
        deadline = time.monotonic() + timeout
        last_error: Exception | None = None
        while time.monotonic() < deadline:
            try:
                self.send("AT", timeout=1.5)
                self.send("ATE0", timeout=2.0)
                return
            except (RuntimeError, TimeoutError, SerialTransportError) as exc:
                last_error = exc
                time.sleep(0.4)
        raise TimeoutError(f"{self.port}: AT synchronization failed: {last_error}")

    def wait_runtime_ready(self, timeout: float = 90.0) -> AtLine:
        """Poll the recoverable snapshot until Wi-Fi, MQTT and TiRTC are ready."""
        deadline = time.monotonic() + timeout
        last_status = ""
        while time.monotonic() < deadline:
            result = self.send("AT+STATUS?", timeout=3.0)
            for line in result.lines:
                fields = parse_csv_urc(line.text, "+STATUS:")
                if fields is None or len(fields) < 7:
                    continue
                last_status = line.text
                if fields[3:7] == ["1", "1", "1", "1"]:
                    return line
            time.sleep(1.0)
        raise TimeoutError(
            f"{self.port}: runtime did not become ready; "
            f"last={redact_rx(last_status)}"
        )


def default_artifact_dir() -> Path:
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    return Path("artifacts") / f"hardware-{stamp}"


def run_probe(devices: Iterable[AtDevice]) -> None:
    for device in devices:
        device.synchronize()
        device.send("AT+STATUS?")
        device.send("AT+PLATFORM?")
        device.send("AT+SESSION?")
        device.send("AT+MEDIA?")


def run_ready(devices: Iterable[AtDevice], timeout: float) -> None:
    for device in devices:
        device.synchronize()
    for device in devices:
        device.wait_runtime_ready(timeout=timeout)
        device.send("AT+PLATFORM?")
        device.send("AT+SESSION?")
        device.send("AT+MEDIA?")


def run_wifi(devices: Iterable[AtDevice]) -> None:
    ssid = os.environ.get("TIRTC_TEST_WIFI_SSID")
    password = os.environ.get("TIRTC_TEST_WIFI_PASSWORD")
    if not ssid or not password:
        raise RuntimeError(
            "TIRTC_TEST_WIFI_SSID and TIRTC_TEST_WIFI_PASSWORD are required"
    )
    command = f"AT+WIFI={quote_at(ssid)},{quote_at(password)}"
    for device in devices:
        device.synchronize()
        expected_epoch, before = device.checkpoint()
        request = device.submit_intent(command, "WIFI_SET")

        def is_wifi_restart(line: AtLine) -> bool:
            fields = parse_csv_urc(line.text, "+SYSTEM:")
            if fields is None or len(fields) != 4:
                return False
            try:
                generation = int(fields[1])
                status = int(fields[2])
            except ValueError:
                return False
            return (
                fields[0] == "RESTARTING"
                and generation == request.app_generation
                and status == 0
                and fields[3] == "wifi_config_changed"
            )

        device.wait_for(
            is_wifi_restart,
            timeout=10.0,
            description="planned Wi-Fi configuration restart",
            after=before,
            expected_epoch=expected_epoch,
        )
        device.close()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "action",
        choices=("probe", "ready", "wifi", "command", "capture"),
    )
    parser.add_argument("--port", action="append", required=True)
    parser.add_argument("--command")
    parser.add_argument("--duration", type=float, default=30.0)
    parser.add_argument("--wait-after", type=float, default=0.0)
    parser.add_argument("--startup-timeout", type=float, default=90.0)
    parser.add_argument("--artifact-dir", type=Path, default=default_artifact_dir())
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.action == "command" and not args.command:
        raise RuntimeError("--command is required for the command action")
    evidence = EvidenceWriter(args.artifact_dir)
    planned_restarts = (
        ("wifi_config_changed",)
        if args.action == "wifi"
        else ()
    )
    devices = [
        AtDevice(
            port,
            evidence,
            planned_restart_details=planned_restarts,
        )
        for port in args.port
    ]
    try:
        for device in devices:
            device.open()
        if args.action == "probe":
            run_probe(devices)
        elif args.action == "ready":
            run_ready(devices, args.startup_timeout)
        elif args.action == "wifi":
            run_wifi(devices)
        elif args.action == "command":
            for device in devices:
                device.synchronize()
                device.send(args.command)
            if args.wait_after > 0:
                time.sleep(args.wait_after)
        else:
            time.sleep(args.duration)
        for device in devices:
            device.assert_healthy(
                expected_restarts=1 if args.action == "wifi" else 0
            )
        evidence.record("summary", "-", f"{args.action}:passed")
        print(f"evidence={evidence.path.resolve()}")
        return 0
    finally:
        for device in devices:
            device.close()
        evidence.close()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise
