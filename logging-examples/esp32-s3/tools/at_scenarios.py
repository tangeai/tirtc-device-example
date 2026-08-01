#!/usr/bin/env python3
"""State-aware hardware scenarios for the ESP32-S3 ThingConnect AT demo."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
import time
from dataclasses import asdict, dataclass
from datetime import datetime
from pathlib import Path
from typing import Callable

from at_regression import (
    AtCommandResult,
    AtDevice,
    AtLine,
    EvidenceWriter,
    RequestHandle,
    RequestRejectedError,
    SerialTransportError,
    parse_csv_urc,
    quote_at,
    redact_rx,
)

REPO_ROOT = Path(__file__).resolve().parents[1]
ESP_ERR_INVALID_STATE_STATUS = 0x103
ESP_ERR_INVALID_ARG_STATUS = 0x102
ESP_ERR_NOT_SUPPORTED_STATUS = 0x106
USER_FORBIDDEN_PREFIXES = (
    "+REQUEST,",
    "+SYSTEM:",
    "+WIFI:",
    "+BIND:",
    "+PLATFORM:",
    "+BUILD:",
    "+STATUS:",
    "+SESSION:",
    "+MEDIA:",
    "+AI:",
    "+CALL:",
    "+CONTACT,",
    "+CONTACTS:",
    "+PENDING,",
    "+PENDING:",
    "+ERROR:",
    "+AT:URC_OVERFLOW,",
    "+TIRTC:READY,",
    "+TIRTC:ERROR,",
)


@dataclass(frozen=True)
class StatusSnapshot:
    generation: int
    state: str
    wifi_configured: bool
    wifi_online: bool
    platform_ready: bool
    mqtt_online: bool
    tirtc_ready: bool
    device_id: str
    owner: str
    session_state: str
    last_error: int


@dataclass(frozen=True)
class BuildSnapshot:
    project_name: str
    project_version: str
    idf_version: str
    elf_sha256: str
    tirtc_version: str
    tirtc_build_info: str


@dataclass(frozen=True)
class SessionSnapshot:
    app_generation: int
    revision: int
    generation: int
    request_id: int
    owner: str
    state: str
    deadline_ms: int
    pending: bool
    caller: bool
    call_type: str
    room_id: str
    peer_id: str
    ai_session_id: str
    ai_update_pending: bool
    ai_update_deadline_ms: int
    ai_action_pending: bool
    ai_action_id: str
    ai_action_deadline_ms: int
    ai_call_handoff_pending: bool
    ai_call_handoff_phase: str
    ai_call_handoff_deadline_ms: int


@dataclass(frozen=True)
class MediaSnapshot:
    adapter_state: str
    connected: bool
    active_profile: str
    measured_profile: str
    active_generation: int
    measured_generation: int
    connection_generation: int
    tx_audio_frames: int
    tx_audio_bytes: int
    tx_video_frames: int
    tx_video_bytes: int
    rx_audio_frames: int
    rx_audio_bytes: int
    rx_video_frames: int
    rx_video_bytes: int
    send_errors: int
    first_tx_ms: int
    last_tx_ms: int
    first_rx_ms: int
    last_rx_ms: int
    connect_request_pending: bool
    connect_callback_pending: bool
    accept_callbacks_pending: int
    disconnects_pending: int
    connection_users: int
    incoming_armed: bool


@dataclass(frozen=True)
class ContactSnapshot:
    device_id: str
    remark: str
    online: bool


@dataclass
class Board:
    name: str
    at: AtDevice
    status: StatusSnapshot | None = None
    build: BuildSnapshot | None = None
    app_generation: int | None = None
    build_identity: dict[str, object] | None = None
    handled_restarts: int = 0
    handled_transport_failures: int = 0
    handled_reconnects: int = 0
    handled_boot_events: int = 0

    @property
    def device_id(self) -> str:
        if self.status is None:
            raise RuntimeError(f"{self.name}: status has not been queried")
        return self.status.device_id


class OperationResultError(RuntimeError):
    def __init__(
        self,
        board: str,
        operation: str,
        status: int,
        phase: str,
        payload: str,
        session_generation: int,
        request_id: int,
    ) -> None:
        self.board = board
        self.operation = operation
        self.status = status
        self.phase = phase
        self.session_generation = session_generation
        self.request_id = request_id
        super().__init__(
            f"{board}: {operation} ended with status={status} "
            "phase=<redacted-operation-phase>; "
            "payload=<redacted-operation-payload>"
        )


def safe_exception_summary(exc: BaseException) -> str:
    if isinstance(exc, OperationResultError):
        return (
            "OperationResultError("
            f"board={exc.board},operation={exc.operation},status={exc.status},"
            f"session_generation={exc.session_generation},"
            f"request_id={exc.request_id})"
        )
    fingerprint = hashlib.sha256(
        str(exc).encode("utf-8", errors="replace")
    ).hexdigest()[:16]
    return f"{type(exc).__name__}(fingerprint={fingerprint})"


def artifact_dir() -> Path:
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    return Path("artifacts") / f"scenario-{stamp}"


def to_int(value: str, label: str) -> int:
    try:
        return int(value)
    except ValueError as exc:
        raise RuntimeError(f"invalid integer {label}={value!r}") from exc


def query_fields(board: Board, command: str, prefix: str) -> list[str]:
    result = board.at.send(command, timeout=4.0)
    matches = [
        fields
        for line in result.lines
        if (fields := parse_csv_urc(line.text, prefix)) is not None
    ]
    if len(matches) != 1:
        raise RuntimeError(
            f"{board.name}: expected one {prefix} response to {command}, "
            f"received {len(matches)}"
        )
    return matches[0]


def query_status(board: Board) -> StatusSnapshot:
    fields = query_fields(board, "AT+STATUS?", "+STATUS:")
    if len(fields) != 13:
        raise RuntimeError(f"{board.name}: STATUS schema mismatch: {len(fields)}")
    snapshot = StatusSnapshot(
        generation=to_int(fields[0], "status.generation"),
        state=fields[1],
        wifi_configured=fields[2] == "1",
        wifi_online=fields[3] == "1",
        platform_ready=fields[4] == "1",
        mqtt_online=fields[5] == "1",
        tirtc_ready=fields[6] == "1",
        device_id=fields[9],
        owner=fields[10],
        session_state=fields[11],
        last_error=to_int(fields[12], "status.last_error"),
    )
    if (
        board.app_generation is not None
        and snapshot.generation != board.app_generation
    ):
        raise RuntimeError(
            f"{board.name}: application generation changed from "
            f"{board.app_generation} to {snapshot.generation}"
        )
    board.status = snapshot
    return snapshot


def query_build(
    board: Board, expected: dict[str, object]
) -> BuildSnapshot:
    fields = query_fields(board, "AT+BUILD?", "+BUILD:")
    if len(fields) != 7 or fields[0] != "1":
        raise RuntimeError(f"{board.name}: BUILD schema mismatch")
    snapshot = BuildSnapshot(
        project_name=fields[1],
        project_version=fields[2],
        idf_version=fields[3],
        elf_sha256=fields[4].lower(),
        tirtc_version=fields[5],
        tirtc_build_info=fields[6],
    )
    metadata = expected["metadata"]
    elf = expected["elf"]
    sdk_manifest = expected["sdk_manifest_data"]
    if not isinstance(metadata, dict) or not isinstance(elf, dict):
        raise RuntimeError("local build identity has an invalid shape")
    if not isinstance(sdk_manifest, dict):
        raise RuntimeError("local SDK manifest has an invalid shape")
    try:
        observed_sdk = json.loads(snapshot.tirtc_build_info)
    except json.JSONDecodeError as exc:
        raise RuntimeError(
            f"{board.name}: TiRTC BuildInfo is not JSON"
        ) from exc
    expected_sdk_version = str(sdk_manifest["sdk_version"])
    expected_commit = str(sdk_manifest["source_commit"])
    if (
        snapshot.project_name != metadata["running_project_name"]
        or snapshot.project_version != metadata["project_version"]
        or snapshot.idf_version != metadata["idf_version"]
        or snapshot.elf_sha256 != elf["sha256"]
        or snapshot.tirtc_version != f"v{expected_sdk_version}"
        or observed_sdk.get("tirtc") != f"v{expected_sdk_version}"
        or observed_sdk.get("commit") != expected_commit[:12]
        or not observed_sdk.get("tgtrp")
    ):
        raise RuntimeError(
            f"{board.name}: running firmware identity mismatch: "
            f"observed={snapshot}"
        )
    board.build = snapshot
    return snapshot


def query_session(board: Board) -> SessionSnapshot:
    fields = query_fields(board, "AT+SESSION?", "+SESSION:")
    if len(fields) != 22 or fields[0] != "3":
        raise RuntimeError(f"{board.name}: SESSION schema mismatch")
    snapshot = SessionSnapshot(
        app_generation=to_int(fields[1], "session.app_generation"),
        revision=to_int(fields[2], "session.revision"),
        generation=to_int(fields[3], "session.generation"),
        request_id=to_int(fields[4], "session.request_id"),
        owner=fields[5],
        state=fields[6],
        deadline_ms=to_int(fields[7], "session.deadline"),
        pending=fields[8] == "1",
        caller=fields[9] == "1",
        call_type=fields[10],
        room_id=fields[11],
        peer_id=fields[12],
        ai_session_id=fields[13],
        ai_update_pending=fields[14] == "1",
        ai_update_deadline_ms=to_int(
            fields[15], "session.ai_update_deadline"
        ),
        ai_action_pending=fields[16] == "1",
        ai_action_id=fields[17],
        ai_action_deadline_ms=to_int(
            fields[18], "session.ai_action_deadline"
        ),
        ai_call_handoff_pending=fields[19] == "1",
        ai_call_handoff_phase=fields[20],
        ai_call_handoff_deadline_ms=to_int(
            fields[21], "session.ai_call_handoff_deadline"
        ),
    )
    if (
        board.app_generation is not None
        and snapshot.app_generation != board.app_generation
    ):
        raise RuntimeError(
            f"{board.name}: SESSION application generation changed from "
            f"{board.app_generation} to {snapshot.app_generation}"
        )
    return snapshot


def session_is_canonical_idle(session: SessionSnapshot) -> bool:
    return (
        session.owner == "none"
        and session.state == "idle"
        and session.request_id == 0
        and session.deadline_ms == 0
        and not session.pending
        and not session.caller
        and session.call_type == "none"
        and session.room_id == ""
        and session.peer_id == ""
        and session.ai_session_id == ""
        and not session.ai_update_pending
        and session.ai_update_deadline_ms == 0
        and not session.ai_action_pending
        and session.ai_action_id == ""
        and session.ai_action_deadline_ms == 0
        and not session.ai_call_handoff_pending
        and session.ai_call_handoff_phase == "none"
        and session.ai_call_handoff_deadline_ms == 0
    )


def query_media(board: Board) -> MediaSnapshot:
    fields = query_fields(board, "AT+MEDIA?", "+MEDIA:")
    if len(fields) != 27 or fields[0] != "1":
        raise RuntimeError(
            f"{board.name}: MEDIA schema mismatch: {len(fields)}"
        )
    values = [to_int(value, f"media[{index}]") for index, value in enumerate(fields[8:17], 8)]
    return MediaSnapshot(
        adapter_state=fields[1],
        connected=fields[2] == "1",
        active_profile=fields[3],
        measured_profile=fields[4],
        active_generation=to_int(fields[5], "media.active_generation"),
        measured_generation=to_int(fields[6], "media.measured_generation"),
        connection_generation=to_int(fields[7], "media.connection_generation"),
        tx_audio_frames=values[0],
        tx_audio_bytes=values[1],
        tx_video_frames=values[2],
        tx_video_bytes=values[3],
        rx_audio_frames=values[4],
        rx_audio_bytes=values[5],
        rx_video_frames=values[6],
        rx_video_bytes=values[7],
        send_errors=values[8],
        first_tx_ms=to_int(fields[17], "media.first_tx_ms"),
        last_tx_ms=to_int(fields[18], "media.last_tx_ms"),
        first_rx_ms=to_int(fields[19], "media.first_rx_ms"),
        last_rx_ms=to_int(fields[20], "media.last_rx_ms"),
        connect_request_pending=fields[21] == "1",
        connect_callback_pending=fields[22] == "1",
        accept_callbacks_pending=to_int(
            fields[23], "media.accept_callbacks_pending"
        ),
        disconnects_pending=to_int(fields[24], "media.disconnects_pending"),
        connection_users=to_int(fields[25], "media.connection_users"),
        incoming_armed=fields[26] == "1",
    )


def expected_restart_count(board: Board) -> int:
    return int(board.at.health_snapshot()["expected_restarts"])


def consume_expected_restarts(
    board: Board,
    reason: str,
    expected_generation: int | None,
) -> None:
    health = board.at.health_snapshot()
    restart_count = int(health["expected_restarts"])
    restart_delta = restart_count - board.handled_restarts
    failure_count = int(health["transport_failures"])
    reconnect_count = int(health["reconnects"])
    failure_delta = failure_count - board.handled_transport_failures
    reconnect_delta = reconnect_count - board.handled_reconnects
    boot_count = int(health["boot_events"])
    boot_delta = boot_count - board.handled_boot_events
    transport_delta_valid = (
        failure_delta in (0, 1)
        and reconnect_delta in (0, 1)
        and failure_delta <= reconnect_delta
    )
    if (
        restart_delta != 1
        or not transport_delta_valid
        or boot_delta < 0
        or boot_delta > 1
        or (reconnect_delta == 0 and boot_delta != 1)
    ):
        raise RuntimeError(
            f"{board.name}: restart recovery is not causally bounded: "
            f"restart_delta={restart_delta} failure_delta={failure_delta} "
            f"reconnect_delta={reconnect_delta} boot_delta={boot_delta} "
            f"health={health}"
        )
    restart_events = board.at.expected_restart_events()
    restart_event = restart_events[board.handled_restarts]
    if (
        restart_event.status != 0
        or restart_event.detail != "tirtc_failed_connect_transport"
        or (
            expected_generation is not None
            and restart_event.generation != expected_generation
        )
    ):
        raise RuntimeError(
            f"{board.name}: controlled restart event is not correlated: "
            f"event={restart_event} expected_generation={expected_generation}"
        )
    boot_event = None
    if boot_delta == 1:
        boot_event = board.at.boot_events()[board.handled_boot_events]
        if (
            boot_event.status != 0
            or boot_event.generation <= 0
            or boot_event.sequence <= restart_event.sequence
        ):
            raise RuntimeError(
                f"{board.name}: BOOTING event does not complete restart: "
                f"restart={restart_event} boot={boot_event}"
            )
    board.handled_restarts = restart_count
    board.handled_transport_failures = failure_count
    board.handled_reconnects = reconnect_count
    board.handled_boot_events = boot_count
    board.at.evidence.record(
        "recovery",
        board.at.port,
        reason,
        handled_restarts=board.handled_restarts,
        handled_transport_failures=board.handled_transport_failures,
        handled_reconnects=board.handled_reconnects,
        transport_failed=failure_delta == 1,
        transport_reopened=reconnect_delta == 1,
        boot_event_seen=boot_event is not None,
        restart_generation=restart_event.generation,
        transport_epoch=board.at.transport_epoch,
    )


def assert_board_healthy(board: Board) -> None:
    board.at.assert_healthy(
        expected_restarts=board.handled_restarts,
        expected_transport_failures=board.handled_transport_failures,
        expected_reconnects=board.handled_reconnects,
    )


def recover_expected_restart(board: Board, timeout: float = 150.0) -> bool:
    restart_count = expected_restart_count(board)
    if restart_count <= board.handled_restarts:
        return False
    expected_generation = board.app_generation

    board.at.evidence.record(
        "recovery",
        board.at.port,
        "expected-device-restart",
        expected_restarts=restart_count,
        previously_handled=board.handled_restarts,
    )
    board.app_generation = None
    board.at.recover(timeout=45.0)
    board.at.wait_runtime_ready(timeout)
    if board.build_identity is not None:
        query_build(board, board.build_identity)
    status = query_status(board)
    board.app_generation = status.generation
    session = query_session(board)
    media = query_media(board)
    if not (
        status.state == "READY"
        and status.wifi_online
        and status.platform_ready
        and status.mqtt_online
        and status.tirtc_ready
        and status.last_error == 0
        and session_is_canonical_idle(session)
        and media.adapter_state == "running"
        and not media.connected
        and media.active_profile == "none"
        and not media.connect_request_pending
        and not media.connect_callback_pending
        and media.accept_callbacks_pending == 0
        and media.disconnects_pending == 0
        and media.connection_users == 0
        and not media.incoming_armed
    ):
        raise RuntimeError(
            f"{board.name}: controlled restart did not recover cleanly: "
            f"status={status} session={session} media={media}"
        )
    assert_platform_canary(board)
    consume_expected_restarts(
        board,
        "expected-device-restart-recovered",
        expected_generation,
    )
    assert_board_healthy(board)
    return True


def wait_session(
    board: Board,
    predicate: Callable[[SessionSnapshot], bool],
    description: str,
    timeout: float = 35.0,
) -> SessionSnapshot:
    deadline = time.monotonic() + timeout
    last: SessionSnapshot | None = None
    while time.monotonic() < deadline:
        last = query_session(board)
        if predicate(last):
            return last
        time.sleep(0.35)
    raise TimeoutError(f"{board.name}: {description}; last={last}")


def wait_request_operation(
    board: Board,
    handle: RequestHandle,
    operation: str,
    result: str,
    timeout: float = 20.0,
    after: int | None = None,
) -> AtLine:
    if (
        board.app_generation is not None
        and handle.app_generation != board.app_generation
    ):
        raise RuntimeError(
            f"{board.name}: request belongs to application generation "
            f"{handle.app_generation}, expected {board.app_generation}"
        )
    prefix = (
        "+AI:OP,"
        if operation.startswith("ai-")
        else "+CALL:OP,"
        if operation.startswith("call-")
        else "+CONTACT:OP,"
    )

    def matches_request(line: AtLine) -> bool:
        fields = parse_csv_urc(line.text, prefix)
        if (
            fields is None
            or len(fields) < 6
            or to_int(fields[1], "operation.request_id") != handle.request_id
            or fields[3] != operation
        ):
            return False
        return fields[4] == result or to_int(fields[2], "operation.status") != 0

    line = board.at.wait_for(
        matches_request,
        timeout,
        f"{operation}={result}",
        after=handle.cursor if after is None else after,
        expected_epoch=handle.transport_epoch,
    )
    fields = parse_csv_urc(line.text, prefix)
    assert fields is not None
    status = to_int(fields[2], "operation.status")
    if status != 0 or fields[4] != result:
        raise OperationResultError(
            board=board.name,
            operation=fields[3],
            status=status,
            phase=fields[4],
            payload=fields[5],
            session_generation=to_int(
                fields[0], "operation.session_generation"
            ),
            request_id=to_int(fields[1], "operation.request_id"),
        )
    board.at.evidence.record(
        "step",
        board.at.port,
        "operation",
        transport_epoch=line.transport_epoch,
        sequence=line.sequence,
        request_id=handle.request_id,
        operation=operation,
        phase=result,
        status=0,
    )
    return line


def wait_operation_after(
    board: Board,
    domain: str,
    operation: str,
    phase: str,
    cursor: int,
    transport_epoch: int,
    timeout: float = 20.0,
) -> AtLine:
    prefix = f"+{domain}:OP,"

    def matches(line: AtLine) -> bool:
        fields = parse_csv_urc(line.text, prefix)
        return (
            fields is not None
            and len(fields) == 6
            and fields[3] == operation
            and (
                fields[4] == phase
                or to_int(fields[2], "operation.status") != 0
            )
        )

    line = board.at.wait_for(
        matches,
        timeout,
        f"{operation}={phase}",
        after=cursor,
        expected_epoch=transport_epoch,
    )
    fields = parse_csv_urc(line.text, prefix)
    assert fields is not None
    status = to_int(fields[2], "operation.status")
    if status != 0 or fields[4] != phase:
        raise OperationResultError(
            board=board.name,
            operation=fields[3],
            status=status,
            phase=fields[4],
            payload=fields[5],
            session_generation=to_int(
                fields[0], "operation.session_generation"
            ),
            request_id=to_int(fields[1], "operation.request_id"),
        )
    board.at.evidence.record(
        "step",
        board.at.port,
        "operation-anchor",
        transport_epoch=line.transport_epoch,
        sequence=line.sequence,
        session_generation=to_int(
            fields[0], "operation.session_generation"
        ),
        request_id=to_int(fields[1], "operation.request_id"),
        operation=operation,
        phase=phase,
        status=0,
    )
    return line


def wait_state_reason(
    board: Board,
    handle: RequestHandle,
    domain: str,
    state: str,
    reason: str,
    expected_generation: int | None = None,
    expected_room_id: str | None = None,
    timeout: float = 20.0,
) -> AtLine:
    if (
        board.app_generation is not None
        and handle.app_generation != board.app_generation
    ):
        raise RuntimeError(
            f"{board.name}: state request belongs to application generation "
            f"{handle.app_generation}, expected {board.app_generation}"
        )
    prefix = f"+{domain}:STATE,"

    def matches(line: AtLine) -> bool:
        fields = parse_csv_urc(line.text, prefix)
        if (
            fields is None
            or len(fields) != 6
            or to_int(fields[1], "state.request_id") != handle.request_id
            or to_int(fields[2], "state.status") != 0
            or fields[3] != state
            or fields[5] != reason
        ):
            return False
        if (
            expected_generation is not None
            and to_int(fields[0], "state.generation") != expected_generation
        ):
            return False
        return expected_room_id is None or fields[4] == expected_room_id

    line = board.at.wait_for(
        matches,
        timeout=timeout,
        description=f"{domain} state {state} reason {reason}",
        after=handle.cursor,
        expected_epoch=handle.transport_epoch,
    )
    fields = parse_csv_urc(line.text, prefix)
    assert fields is not None
    board.at.evidence.record(
        "step",
        board.at.port,
        "state-anchor",
        transport_epoch=line.transport_epoch,
        sequence=line.sequence,
        request_id=handle.request_id,
        session_generation=to_int(fields[0], "state.generation"),
        domain=domain,
        state=state,
        reason=reason,
        room_id=fields[4],
    )
    return line


def wait_ai_state(
    board: Board,
    handle: RequestHandle,
    state: str,
    reason: str | None = None,
    timeout: float = 15.0,
) -> AtLine:
    if (
        board.app_generation is not None
        and handle.app_generation != board.app_generation
    ):
        raise RuntimeError(
            f"{board.name}: AI request belongs to application generation "
            f"{handle.app_generation}, expected {board.app_generation}"
        )
    def matches(line: AtLine) -> bool:
        fields = parse_csv_urc(line.text, "+AI:STATE,")
        return (
            fields is not None
            and len(fields) >= 6
            and to_int(fields[1], "ai-state.request_id") == handle.request_id
            and to_int(fields[2], "ai-state.status") == 0
            and fields[3] == state
            and (reason is None or fields[5] == reason)
        )

    line = board.at.wait_for(
        matches,
        timeout=timeout,
        description=f"AI state {state}",
        after=handle.cursor,
        expected_epoch=handle.transport_epoch,
    )
    board.at.evidence.record(
        "step",
        board.at.port,
        "ai-state",
        transport_epoch=line.transport_epoch,
        sequence=line.sequence,
        request_id=handle.request_id,
        state=state,
    )
    return line


def wait_call_ending(
    board: Board,
    session_generation: int,
    request_ids: set[int],
    room_id: str,
    allowed_outcomes: set[tuple[str, int]],
    cursor: int,
    transport_epoch: int,
    timeout: float = 20.0,
) -> AtLine:
    def matches(line: AtLine) -> bool:
        fields = parse_csv_urc(line.text, "+CALL:STATE,")
        return (
            fields is not None
            and len(fields) == 6
            and to_int(fields[0], "call-ending.generation")
            == session_generation
            and to_int(fields[1], "call-ending.request_id") in request_ids
            and fields[3] == "ending"
            and fields[4] == room_id
            and (
                fields[5],
                to_int(fields[2], "call-ending.status"),
            )
            in allowed_outcomes
        )

    line = board.at.wait_for(
        matches,
        timeout=timeout,
        description=(
            f"call ending outcomes={sorted(allowed_outcomes)} room={room_id}"
        ),
        after=cursor,
        expected_epoch=transport_epoch,
    )
    fields = parse_csv_urc(line.text, "+CALL:STATE,")
    assert fields is not None
    board.at.evidence.record(
        "step",
        board.at.port,
        "call-ending",
        transport_epoch=line.transport_epoch,
        sequence=line.sequence,
        session_generation=session_generation,
        request_id=to_int(fields[1], "call-ending.request_id"),
        room_id=room_id,
        reason=fields[5],
        status=to_int(fields[2], "call-ending.status"),
    )
    return line


def wait_media(
    board: Board,
    generation: int,
    video: bool,
    baseline: MediaSnapshot,
    require_tx_growth: bool = True,
    require_rx_growth: bool = True,
    timeout: float = 20.0,
) -> MediaSnapshot:
    deadline = time.monotonic() + timeout
    last: MediaSnapshot | None = None
    while time.monotonic() < deadline:
        last = query_media(board)
        audio_ok = (
            (
                last.tx_audio_frames > baseline.tx_audio_frames
                and last.tx_audio_bytes > baseline.tx_audio_bytes
                if require_tx_growth
                else last.tx_audio_frames >= baseline.tx_audio_frames
                and last.tx_audio_bytes >= baseline.tx_audio_bytes
            )
            and (
                last.rx_audio_frames > baseline.rx_audio_frames
                and last.rx_audio_bytes > baseline.rx_audio_bytes
                if require_rx_growth
                else last.rx_audio_frames >= baseline.rx_audio_frames
                and last.rx_audio_bytes >= baseline.rx_audio_bytes
            )
        )
        video_ok = not video or (
            (
                last.tx_video_frames > baseline.tx_video_frames
                and last.tx_video_bytes > baseline.tx_video_bytes
                if require_tx_growth
                else last.tx_video_frames >= baseline.tx_video_frames
                and last.tx_video_bytes >= baseline.tx_video_bytes
            )
            and (
                last.rx_video_frames > baseline.rx_video_frames
                and last.rx_video_bytes > baseline.rx_video_bytes
                if require_rx_growth
                else last.rx_video_frames >= baseline.rx_video_frames
                and last.rx_video_bytes >= baseline.rx_video_bytes
            )
        )
        expected_profile = "call" if video or baseline.active_profile == "call" else "ai"
        if (
            last.connected
            and last.active_profile == expected_profile
            and last.measured_profile == expected_profile
            and last.active_generation == generation
            and last.measured_generation == generation
            and audio_ok
            and video_ok
            and baseline.send_errors == 0
            and last.send_errors == baseline.send_errors
        ):
            board.at.evidence.record(
                "step",
                board.at.port,
                "media-window",
                session_generation=generation,
                video=video,
                require_tx_growth=require_tx_growth,
                require_rx_growth=require_rx_growth,
                baseline=asdict(baseline),
                final=asdict(last),
            )
            return last
        time.sleep(0.75)
    raise TimeoutError(f"{board.name}: media did not grow; last={last}")


def wait_incoming(
    callee: Board,
    caller_id: str,
    call_type: str,
    cursor: int,
    transport_epoch: int,
    expected_room_id: str | None = None,
    timeout: float = 20.0,
) -> tuple[str, AtLine]:
    def matches(line: AtLine) -> bool:
        fields = parse_csv_urc(line.text, "+CALL:INCOMING,")
        return (
            fields is not None
            and len(fields) == 5
            and fields[2] == caller_id
            and (expected_room_id is None or fields[3] == expected_room_id)
            and fields[4] == call_type
        )

    line = callee.at.wait_for(
        matches,
        timeout,
        f"incoming {call_type} call from {caller_id}",
        after=cursor,
        expected_epoch=transport_epoch,
    )
    fields = parse_csv_urc(line.text, "+CALL:INCOMING,")
    assert fields is not None
    return fields[3], line


def preflight(
    boards: list[Board],
    timeout: float,
    build_identity: dict[str, object],
) -> None:
    for board in boards:
        board.build_identity = build_identity
        board.at.synchronize()
    for board in boards:
        board.at.wait_runtime_ready(timeout)
        query_build(board, build_identity)
        status = query_status(board)
        board.app_generation = status.generation
        session = query_session(board)
        media = query_media(board)
        if not (
            status.wifi_online
            and status.state == "READY"
            and status.platform_ready
            and status.mqtt_online
            and status.tirtc_ready
            and status.device_id
            and status.owner == "none"
            and status.session_state == "idle"
            and status.last_error == 0
        ):
            raise RuntimeError(f"{board.name}: runtime is not fully ready: {status}")
        if not session_is_canonical_idle(session):
            raise RuntimeError(f"{board.name}: session is not idle: {session}")
        if (
            media.adapter_state != "running"
            or media.connected
            or media.active_profile != "none"
            or media.send_errors != 0
            or media.connect_request_pending
            or media.connect_callback_pending
            or media.accept_callbacks_pending != 0
            or media.disconnects_pending != 0
            or media.connection_users != 0
            or media.incoming_armed
        ):
            raise RuntimeError(f"{board.name}: TiRTC adapter is not running: {media}")
        board.at.assert_healthy()
        board.handled_boot_events = len(board.at.boot_events())
    ids = [board.device_id for board in boards]
    if len(set(ids)) != len(ids):
        raise RuntimeError("boards reported the same device_id")


def ensure_idle(board: Board, timeout: float = 15.0) -> None:
    try:
        session = query_session(board)
        if session.owner == "ai":
            try:
                cleanup = board.at.submit_intent("AT+AI=STOP", "AI_STOP")
                wait_ai_state(board, cleanup, "idle", timeout=timeout)
            except (RequestRejectedError, TimeoutError):
                pass
        elif session.owner == "call":
            command = (
                "AT+REJECT"
                if session.pending
                else "AT+CANCEL"
                if session.caller and session.state != "in-call"
                else "AT+HANGUP"
            )
            operation = command.removeprefix("AT+").replace("=", "_")
            try:
                cleanup = board.at.submit_intent(command, f"CALL_{operation}")
                wait_request_operation(
                    board,
                    cleanup,
                    f"call-{operation.lower()}",
                    "submitted",
                    timeout=5.0,
                )
            except (RequestRejectedError, TimeoutError):
                pass
        wait_session(
            board,
            lambda item: item.owner == "none" and item.state == "idle",
            "cleanup to idle",
            timeout,
        )
    except (RuntimeError, TimeoutError, SerialTransportError):
        if not recover_expected_restart(board):
            raise


def assert_platform_canary(board: Board) -> None:
    handle = board.at.submit_intent("AT+CONTACTS?", "CONTACTS_LIST")
    if (
        board.app_generation is not None
        and handle.app_generation != board.app_generation
    ):
        raise RuntimeError(
            f"{board.name}: contacts canary crossed an application restart"
        )

    def matches(line: AtLine) -> bool:
        fields = parse_csv_urc(line.text, "+CONTACTS:DONE,")
        return (
            fields is not None
            and len(fields) == 3
            and to_int(fields[0], "contacts.request_id") == handle.request_id
        )

    line = board.at.wait_for(
        matches,
        timeout=15.0,
        description="contacts HTTPS canary",
        after=handle.cursor,
        expected_epoch=handle.transport_epoch,
    )
    fields = parse_csv_urc(line.text, "+CONTACTS:DONE,")
    assert fields is not None
    status = to_int(fields[2], "contacts.status")
    if status != 0:
        raise RuntimeError(
            f"{board.name}: contacts HTTPS canary failed with status={status}"
        )
    board.at.evidence.record(
        "step",
        board.at.port,
        "platform-canary",
        request_id=handle.request_id,
        contacts=to_int(fields[1], "contacts.count"),
        status=status,
    )


def query_device_contacts(board: Board) -> dict[str, ContactSnapshot]:
    handle = board.at.submit_intent("AT+CONTACTS?", "CONTACTS_LIST")

    def done_matches(line: AtLine) -> bool:
        fields = parse_csv_urc(line.text, "+CONTACTS:DONE,")
        return (
            fields is not None
            and len(fields) == 3
            and to_int(fields[0], "contacts.request_id") == handle.request_id
        )

    done = board.at.wait_for(
        done_matches,
        timeout=15.0,
        description="device contacts",
        after=handle.cursor,
        expected_epoch=handle.transport_epoch,
    )
    done_fields = parse_csv_urc(done.text, "+CONTACTS:DONE,")
    assert done_fields is not None
    status = to_int(done_fields[2], "contacts.status")
    if status != 0:
        raise RuntimeError(
            f"{board.name}: contacts query failed with status={status}"
        )

    contacts: dict[str, ContactSnapshot] = {}
    for line in board.at.history(handle.cursor):
        if line.sequence > done.sequence:
            break
        fields = parse_csv_urc(line.text, "+CONTACT,")
        if (
            fields is None
            or len(fields) != 6
            or to_int(fields[0], "contact.request_id") != handle.request_id
        ):
            continue
        item = ContactSnapshot(
            device_id=fields[3],
            remark=fields[4],
            online=to_int(fields[2], "contact.online") == 1,
        )
        if item.device_id in contacts:
            raise RuntimeError(f"{board.name}: duplicate device contact")
        contacts[item.device_id] = item
    expected_count = to_int(done_fields[1], "contacts.count")
    if len(contacts) != expected_count:
        raise RuntimeError(
            f"{board.name}: contact snapshot count mismatch "
            f"expected={expected_count} actual={len(contacts)}"
        )
    return contacts


def set_contact_remark(
    board: Board,
    target_device_id: str,
    remark: str,
) -> None:
    update = board.at.submit_intent(
        "AT+CONTACT=REMARK,"
        f"{quote_at(target_device_id)},{quote_at(remark)}",
        "CONTACT_REMARK",
    )
    wait_request_operation(
        board,
        update,
        "contacts-remark",
        "completed",
        timeout=15.0,
    )
    refreshed = query_device_contacts(board)
    contact = refreshed.get(target_device_id)
    if contact is None or contact.remark != remark:
        raise RuntimeError(
            f"{board.name}: contact remark did not converge after update"
        )


def command_response_fields(
    result: AtCommandResult,
    prefix: str,
    description: str,
) -> list[str]:
    matches = [
        fields
        for line in result.lines
        if (fields := parse_csv_urc(line.text, prefix)) is not None
    ]
    if len(matches) != 1:
        raise RuntimeError(
            f"{result.command}: expected one {description} response, "
            f"received {len(matches)}"
        )
    return matches[0]


def set_protocol_mode(board: Board, mode: str) -> AtCommandResult:
    result = board.at.send(f"AT+PROTO={mode}", timeout=4.0)
    fields = command_response_fields(result, "+PROTO:", "protocol mode")
    if fields != [mode]:
        raise RuntimeError(
            f"{board.name}: protocol mode did not change to {mode}"
        )
    board.at.evidence.record(
        "step",
        board.at.port,
        "protocol-mode",
        board=board.name,
        mode=mode,
    )
    return result


def expect_user_accepted(
    board: Board,
    command: str,
    expected_text: str,
) -> AtCommandResult:
    result = board.at.send(command, timeout=8.0)
    fields = command_response_fields(
        result,
        "+TIRTC:已受理,",
        "Chinese accepted",
    )
    if len(fields) != 1 or expected_text not in fields[0]:
        raise RuntimeError(
            f"{board.name}: simplified command acknowledgement mismatch"
        )
    return result


def wait_user_event(
    board: Board,
    categories: str | tuple[str, ...],
    predicate: Callable[[list[str]], bool],
    description: str,
    *,
    after: int,
    transport_epoch: int,
    timeout: float = 60.0,
) -> AtLine:
    accepted_categories = (
        (categories,) if isinstance(categories, str) else categories
    )

    def matches(line: AtLine) -> bool:
        for category in accepted_categories:
            fields = parse_csv_urc(
                line.text,
                f"+TIRTC:{category},",
            )
            if fields is not None and predicate(fields):
                return True
        return False

    return board.at.wait_for(
        matches,
        timeout=timeout,
        description=description,
        after=after,
        expected_epoch=transport_epoch,
    )


def wait_user_text(
    board: Board,
    categories: str | tuple[str, ...],
    expected_text: str,
    description: str,
    *,
    after: int,
    transport_epoch: int,
    timeout: float = 60.0,
) -> AtLine:
    return wait_user_event(
        board,
        categories,
        lambda fields: any(expected_text in field for field in fields),
        description,
        after=after,
        transport_epoch=transport_epoch,
        timeout=timeout,
    )


def query_user_status(board: Board) -> tuple[AtCommandResult, list[str]]:
    result = board.at.send("AT+TIRTC?", timeout=4.0)
    fields = command_response_fields(
        result,
        "+TIRTC:状态,",
        "Chinese status",
    )
    if len(fields) != 2:
        raise RuntimeError(f"{board.name}: simplified status schema mismatch")
    return result, fields


def query_user_media(board: Board) -> tuple[AtCommandResult, list[str]]:
    result = board.at.send("AT+TIRTC=媒体", timeout=4.0)
    fields = command_response_fields(
        result,
        "+TIRTC:媒体,",
        "Chinese media",
    )
    if len(fields) != 6:
        raise RuntimeError(f"{board.name}: simplified media schema mismatch")
    for field in fields[1:]:
        key, separator, value = field.partition("=")
        if not key or separator != "=":
            raise RuntimeError(
                f"{board.name}: simplified media counter is malformed"
            )
        try:
            parsed = int(value)
        except ValueError as exc:
            raise RuntimeError(
                f"{board.name}: simplified media counter is not numeric"
            ) from exc
        if parsed < 0:
            raise RuntimeError(
                f"{board.name}: simplified media counter is negative"
            )
    return result, fields


def assert_user_help_and_snapshots(board: Board) -> None:
    help_result = board.at.send("AT+TIRTC=?", timeout=4.0)
    help_lines = [
        fields
        for line in help_result.lines
        if (fields := parse_csv_urc(line.text, "+TIRTC:帮助,"))
        is not None
    ]
    expected_sections = {
        "状态",
        "配网",
        "绑定",
        "AI",
        "AI呼叫",
        "通话",
        "联系人",
    }
    if (
        len(help_lines) != len(expected_sections)
        or {fields[0] for fields in help_lines if len(fields) >= 2}
        != expected_sections
    ):
        raise RuntimeError(f"{board.name}: simplified help is incomplete")

    _, status = query_user_status(board)
    if status != ["已就绪", "空闲"]:
        raise RuntimeError(
            f"{board.name}: simplified status is not ready and idle"
        )

    device_result = board.at.send("AT+TIRTC=设备", timeout=4.0)
    device = command_response_fields(
        device_result,
        "+TIRTC:设备,",
        "Chinese device",
    )
    if len(device) != 1 or not device[0] or device[0] == "未绑定":
        raise RuntimeError(
            f"{board.name}: simplified device identity is unavailable"
        )

    _, media = query_user_media(board)
    if media[0] != "空闲":
        raise RuntimeError(
            f"{board.name}: simplified media is not idle at preflight"
        )


def assert_user_contacts(
    board: Board,
    peer_device_id: str,
    expected_alias: str,
) -> None:
    def query_and_verify() -> None:
        result = expect_user_accepted(
            board,
            "AT+TIRTC=联系人",
            "查询联系人",
        )
        wait_user_event(
            board,
            "联系人",
            lambda fields: (
                len(fields) == 4
                and fields[1] == "1"
                and fields[2] == peer_device_id
                and fields[3] == expected_alias
            ),
            f"online contact alias {expected_alias}",
            after=result.after,
            transport_epoch=result.transport_epoch,
            timeout=20.0,
        )
        wait_user_event(
            board,
            "联系人",
            lambda fields: len(fields) == 1 and fields[0].startswith("共"),
            "contact list terminal",
            after=result.after,
            transport_epoch=result.transport_epoch,
            timeout=20.0,
        )

    query_and_verify()
    remark = expect_user_accepted(
        board,
        (
            f"AT+TIRTC=备注,{quote_at(peer_device_id)},"
            f"{quote_at(expected_alias)}"
        ),
        "更新联系人备注",
    )
    wait_user_text(
        board,
        "联系人",
        "联系人备注已更新",
        "contact remark terminal",
        after=remark.after,
        transport_epoch=remark.transport_epoch,
        timeout=20.0,
    )
    query_and_verify()

    pending = expect_user_accepted(
        board,
        "AT+TIRTC=待处理",
        "查询好友申请",
    )
    wait_user_event(
        board,
        "好友申请",
        lambda fields: len(fields) == 1 and fields[0].startswith("共"),
        "pending contacts terminal",
        after=pending.after,
        transport_epoch=pending.transport_epoch,
        timeout=20.0,
    )


def assert_user_invalid_inputs(board: Board) -> None:
    cases = (
        ('AT+TIRTC=天气,"91","121"', "经纬度"),
        ('AT+TIRTC=呼叫,"不存在的联系人"', "小张或小李"),
        ("AT+TIRTC=未知操作", "未知操作"),
    )
    for command, expected_text in cases:
        result = board.at.send(
            command,
            timeout=5.0,
            expected_error=ESP_ERR_INVALID_ARG_STATUS,
        )
        fields = command_response_fields(
            result,
            "+TIRTC:失败,",
            "invalid simplified input",
        )
        if len(fields) != 1 or expected_text not in fields[0]:
            raise RuntimeError(
                f"{board.name}: invalid input did not return a clear reason"
            )


def wait_user_surface_idle(
    board: Board,
    timeout: float = 20.0,
) -> None:
    deadline = time.monotonic() + timeout
    last_status: list[str] = []
    last_media: list[str] = []
    while time.monotonic() < deadline:
        _, last_status = query_user_status(board)
        _, last_media = query_user_media(board)
        if (
            last_status == ["已就绪", "空闲"]
            and last_media[0] == "空闲"
        ):
            return
        time.sleep(0.5)
    raise TimeoutError(
        f"{board.name}: simplified surface did not settle to idle"
    )


def run_user_ai_round(
    board: Board,
    action: str,
    keyword: str,
) -> None:
    result = expect_user_accepted(
        board,
        f"AT+TIRTC={action}",
        action,
    )
    user_caption = wait_user_event(
        board,
        "字幕",
        lambda fields: (
            len(fields) == 2
            and fields[0] == "用户"
            and keyword in fields[1]
        ),
        f"{action} user caption",
        after=result.after,
        transport_epoch=result.transport_epoch,
        timeout=90.0,
    )
    premature = [
        line.text
        for line in board.at.history(result.after)
        if line.sequence < user_caption.sequence
        and (
            line.text.startswith('+TIRTC:字幕,"AI",')
            or line.text == '+TIRTC:AI,"本轮完成"'
        )
    ]
    if premature:
        raise RuntimeError(
            f"{board.name}: {action} exposed the AI greeting as the "
            "requested round"
        )
    ai_caption = wait_user_event(
        board,
        "字幕",
        lambda fields: (
            len(fields) == 2
            and fields[0] == "AI"
            and bool(fields[1])
        ),
        f"{action} AI caption",
        after=user_caption.sequence,
        transport_epoch=result.transport_epoch,
        timeout=120.0,
    )
    round_end = wait_user_text(
        board,
        "AI",
        "本轮完成",
        f"{action} round completion",
        after=ai_caption.sequence,
        transport_epoch=result.transport_epoch,
        timeout=45.0,
    )
    board.at.evidence.record(
        "step",
        board.at.port,
        "simple-cn-ai-round",
        board=board.name,
        action=action,
        user_caption_sequence=user_caption.sequence,
        ai_caption_sequence=ai_caption.sequence,
        round_end_sequence=round_end.sequence,
    )


def stop_user_ai(board: Board) -> None:
    result = expect_user_accepted(
        board,
        "AT+TIRTC=结束AI",
        "结束AI",
    )
    wait_user_text(
        board,
        "AI",
        "对讲结束",
        "AI stop",
        after=result.after,
        transport_epoch=result.transport_epoch,
        timeout=30.0,
    )
    wait_user_surface_idle(board)


def begin_user_alias_call(
    caller: Board,
    callee: Board,
    target_alias: str,
) -> tuple[int, int]:
    callee_epoch, callee_cursor = callee.at.checkpoint()
    result = expect_user_accepted(
        caller,
        f"AT+TIRTC=呼叫,{quote_at(target_alias)}",
        target_alias,
    )
    wait_user_event(
        caller,
        "字幕",
        lambda fields: (
            len(fields) == 2
            and fields[0] == "用户"
            and target_alias in fields[1]
        ),
        f"AI call caption for {target_alias}",
        after=result.after,
        transport_epoch=result.transport_epoch,
        timeout=90.0,
    )
    incoming = wait_user_event(
        callee,
        "呼叫",
        lambda fields: (
            len(fields) == 3
            and fields[0] == "收到来电"
            and fields[1] == caller.device_id
            and fields[2] in ("音频", "视频")
        ),
        "simplified incoming call",
        after=callee_cursor,
        transport_epoch=callee_epoch,
        timeout=150.0,
    )
    caller.at.evidence.record(
        "step",
        caller.at.port,
        "simple-cn-alias-call",
        caller=caller.name,
        callee=callee.name,
        alias=target_alias,
        incoming_sequence=incoming.sequence,
    )
    return callee_epoch, incoming.sequence


def wait_user_call_terminal(
    board: Board,
    *,
    after: int,
    transport_epoch: int,
    expected_text: str | None = None,
) -> AtLine:
    terminal_texts = (
        "通话结束",
        "已拒绝来电",
        "已取消呼叫",
        "呼叫未接通",
        "通话异常结束",
    )
    return wait_user_event(
        board,
        "呼叫",
        lambda fields: (
            any(
                (
                    expected_text in field
                    if expected_text is not None
                    else any(text in field for text in terminal_texts)
                )
                for field in fields
            )
        ),
        "simplified call terminal state",
        after=after,
        transport_epoch=transport_epoch,
        timeout=45.0,
    )


def run_user_connected_call(
    caller: Board,
    callee: Board,
    target_alias: str,
    media_seconds: float,
    hangup_board: Board,
) -> None:
    callee_epoch, incoming_cursor = begin_user_alias_call(
        caller,
        callee,
        target_alias,
    )
    caller_epoch, caller_cursor = caller.at.checkpoint()
    accepted = expect_user_accepted(
        callee,
        "AT+TIRTC=接听",
        "接听",
    )
    wait_user_text(
        callee,
        "呼叫",
        "通话接通",
        "callee connected",
        after=accepted.after,
        transport_epoch=callee_epoch,
        timeout=30.0,
    )
    wait_user_text(
        caller,
        "呼叫",
        "通话接通",
        "caller connected",
        after=caller_cursor,
        transport_epoch=caller_epoch,
        timeout=30.0,
    )
    time.sleep(media_seconds)
    for board in (caller, callee):
        _, media = query_user_media(board)
        if media[0] != "通话":
            raise RuntimeError(
                f"{board.name}: simplified media did not enter call profile"
            )

    peer = callee if hangup_board is caller else caller
    peer_epoch, peer_cursor = peer.at.checkpoint()
    hangup = expect_user_accepted(
        hangup_board,
        "AT+TIRTC=挂断",
        "挂断",
    )
    wait_user_call_terminal(
        hangup_board,
        after=hangup.after,
        transport_epoch=hangup.transport_epoch,
        expected_text="通话结束",
    )
    wait_user_call_terminal(
        peer,
        after=peer_cursor,
        transport_epoch=peer_epoch,
        expected_text="通话结束",
    )
    for board in (caller, callee):
        wait_user_surface_idle(board)


def run_user_rejected_call(
    caller: Board,
    callee: Board,
    target_alias: str,
) -> None:
    callee_epoch, _ = begin_user_alias_call(caller, callee, target_alias)
    caller_epoch, caller_cursor = caller.at.checkpoint()
    rejected = expect_user_accepted(
        callee,
        "AT+TIRTC=拒接",
        "拒接",
    )
    wait_user_call_terminal(
        callee,
        after=rejected.after,
        transport_epoch=callee_epoch,
        expected_text="已拒绝来电",
    )
    wait_user_call_terminal(
        caller,
        after=caller_cursor,
        transport_epoch=caller_epoch,
    )
    for board in (caller, callee):
        wait_user_surface_idle(board)


def run_user_cancelled_call(
    caller: Board,
    callee: Board,
    target_alias: str,
) -> None:
    callee_epoch, incoming_cursor = begin_user_alias_call(
        caller,
        callee,
        target_alias,
    )
    cancelled = expect_user_accepted(
        caller,
        "AT+TIRTC=取消",
        "取消",
    )
    wait_user_call_terminal(
        caller,
        after=cancelled.after,
        transport_epoch=cancelled.transport_epoch,
        expected_text="已取消呼叫",
    )
    wait_user_call_terminal(
        callee,
        after=incoming_cursor,
        transport_epoch=callee_epoch,
    )
    for board in (caller, callee):
        wait_user_surface_idle(board)


def assert_user_idle_error(board: Board, action: str) -> None:
    result = board.at.send(
        f"AT+TIRTC={action}",
        timeout=8.0,
        expected_error=ESP_ERR_INVALID_STATE_STATUS,
    )
    fields = command_response_fields(
        result,
        "+TIRTC:失败,",
        f"idle {action} rejection",
    )
    if len(fields) != 1 or "当前" not in fields[0]:
        raise RuntimeError(
            f"{board.name}: idle {action} did not return a clear Chinese reason"
        )


def assert_user_legacy_commands_blocked(board: Board) -> None:
    for command in ("AT+STATUS?", "AT+BUILD?", "AT+CONTACTS?"):
        result = board.at.send(
            command,
            timeout=5.0,
            expected_error=ESP_ERR_NOT_SUPPORTED_STATUS,
        )
        fields = command_response_fields(
            result,
            "+TIRTC:失败,",
            "USER legacy-command guard",
        )
        if len(fields) != 1 or "RAW" not in fields[0]:
            raise RuntimeError(
                f"{board.name}: USER legacy-command guard is unclear"
            )


def assert_user_window_clean(
    board: Board,
    transport_epoch: int,
    start: int,
    end: int,
) -> None:
    lines = [
        line
        for line in board.at.history(start)
        if line.sequence <= end
    ]
    if any(line.transport_epoch != transport_epoch for line in lines):
        raise RuntimeError(
            f"{board.name}: simplified output crossed a transport epoch"
        )
    forbidden = [
        prefix
        for line in lines
        for prefix in USER_FORBIDDEN_PREFIXES
        if line.text.startswith(prefix)
    ]
    if forbidden:
        raise RuntimeError(
            f"{board.name}: legacy structured output leaked into USER mode: "
            f"{sorted(set(forbidden))}"
        )
    unexpected = [
        line.text
        for line in lines
        if not (
            line.text == "OK"
            or line.text.startswith("ERROR:")
            or line.text.startswith("+TIRTC:")
            or line.text.startswith("+PROTO:")
        )
    ]
    if unexpected:
        raise RuntimeError(
            f"{board.name}: unexpected output leaked into USER mode: "
            f"{unexpected[:3]}"
        )
    user_lines = [
        line
        for line in lines
        if line.text.startswith("+TIRTC:")
    ]
    if not user_lines:
        raise RuntimeError(
            f"{board.name}: simplified output window had no Chinese events"
        )
    board.at.evidence.record(
        "step",
        board.at.port,
        "simple-cn-output-audit",
        board=board.name,
        user_lines=len(user_lines),
        legacy_lines=0,
    )


def scenario_simple_cn(
    board_a: Board,
    board_b: Board,
    media_seconds: float,
) -> None:
    contacts_a = query_device_contacts(board_a)
    contacts_b = query_device_contacts(board_b)
    contact_b = contacts_a.get(board_b.device_id)
    contact_a = contacts_b.get(board_a.device_id)
    if (
        contact_b is None
        or contact_b.remark != "小李"
        or not contact_b.online
        or contact_a is None
        or contact_a.remark != "小张"
        or not contact_a.online
    ):
        raise RuntimeError(
            "simple-cn requires the existing online aliases 小张 and 小李"
        )

    boards = (board_a, board_b)
    user_windows: dict[str, tuple[int, int]] = {}
    cleanup_errors: list[str] = []
    scenario_error: BaseException | None = None
    try:
        for board in boards:
            mode = set_protocol_mode(board, "USER")
            user_windows[board.name] = (
                mode.transport_epoch,
                mode.after,
            )

        for board in boards:
            assert_user_help_and_snapshots(board)
            assert_user_legacy_commands_blocked(board)

        assert_user_contacts(board_a, board_b.device_id, "小李")
        assert_user_contacts(board_b, board_a.device_id, "小张")
        assert_user_invalid_inputs(board_a)

        run_user_ai_round(board_a, "故事", "故事")
        run_user_ai_round(board_a, "笑话", "笑话")
        run_user_ai_round(board_a, "天气", "天气")
        stop_user_ai(board_a)

        run_user_connected_call(
            board_a,
            board_b,
            "小李",
            media_seconds,
            board_a,
        )
        run_user_rejected_call(board_a, board_b, "小李")
        run_user_cancelled_call(board_a, board_b, "小李")
        run_user_connected_call(
            board_b,
            board_a,
            "小张",
            media_seconds,
            board_a,
        )

        for action in ("结束AI", "接听", "拒接", "取消", "挂断"):
            assert_user_idle_error(board_a, action)
        wait_user_surface_idle(board_a)
    except BaseException as exc:
        scenario_error = exc
    finally:
        for board in boards:
            window = user_windows.get(board.name)
            try:
                raw = set_protocol_mode(board, "RAW")
                if window is not None:
                    assert_user_window_clean(
                        board,
                        window[0],
                        window[1],
                        raw.after,
                    )
            except Exception as exc:
                cleanup_errors.append(
                    f"{board.name}: USER output audit or RAW restore failed: "
                    f"{safe_exception_summary(exc)}"
                )

        for board in boards:
            try:
                ensure_idle(board, timeout=20.0)
            except Exception as exc:
                cleanup_errors.append(
                    f"{board.name}: cleanup failed: "
                    f"{safe_exception_summary(exc)}"
                )

    if scenario_error is not None or cleanup_errors:
        for board in boards:
            try:
                set_protocol_mode(board, "USER")
            except Exception as exc:
                cleanup_errors.append(
                    f"{board.name}: USER restore failed: "
                    f"{safe_exception_summary(exc)}"
                )
        if scenario_error is not None:
            raise scenario_error
        raise RuntimeError("; ".join(cleanup_errors))

    final_error: BaseException | None = None
    try:
        for board in boards:
            assert_settled_idle(board, "simple-cn final RAW barrier")
            status = query_status(board)
            session = query_session(board)
            if not (
                status.state == "READY"
                and status.owner == "none"
                and status.session_state == "idle"
                and status.last_error == 0
                and session_is_canonical_idle(session)
            ):
                raise RuntimeError(
                    f"{board.name}: simple-cn did not finish in canonical idle"
                )
    except BaseException as exc:
        final_error = exc
    finally:
        for board in boards:
            try:
                set_protocol_mode(board, "USER")
                _, status = query_user_status(board)
                if status != ["已就绪", "空闲"]:
                    raise RuntimeError("USER surface is not ready and idle")
            except Exception as exc:
                cleanup_errors.append(
                    f"{board.name}: final USER restore failed: "
                    f"{safe_exception_summary(exc)}"
                )
    if final_error is not None:
        raise final_error
    if cleanup_errors:
        raise RuntimeError("; ".join(cleanup_errors))


def wait_ai_round_event(
    board: Board,
    generation: int,
    name: str,
    cursor: int,
    transport_epoch: int,
    timeout: float = 30.0,
) -> AtLine:
    def matches(line: AtLine) -> bool:
        fields = parse_csv_urc(line.text, "+AI:EVENT,")
        return (
            fields is not None
            and len(fields) == 6
            and to_int(fields[0], "ai-event.generation") == generation
            and fields[3] == name
        )

    return board.at.wait_for(
        matches,
        timeout=timeout,
        description=f"AI event {name}",
        after=cursor,
        expected_epoch=transport_epoch,
    )


def wait_ai_caption(
    board: Board,
    generation: int,
    caption_type: int,
    cursor: int,
    transport_epoch: int,
    timeout: float = 30.0,
    require_final: bool = True,
    expected_keyword: str | None = None,
) -> AtLine:
    def matches(line: AtLine) -> bool:
        fields = parse_csv_urc(line.text, "+AI:CAPTION,")
        return (
            fields is not None
            and len(fields) == 8
            and to_int(fields[0], "ai-caption.generation") == generation
            and to_int(fields[2], "ai-caption.type") == caption_type
            and fields[5] != ""
            and fields[7] != ""
            and "\ufffd" not in fields[5]
            and "\ufffd" not in fields[7]
            and (
                expected_keyword is None
                or expected_keyword in fields[7]
            )
            and (
                not require_final
                or to_int(fields[6], "ai-caption.final") == 1
            )
        )

    return board.at.wait_for(
        matches,
        timeout=timeout,
        description=(
            f"final AI caption type={caption_type}"
            if require_final
            else f"non-empty AI caption type={caption_type}"
        ),
        after=cursor,
        expected_epoch=transport_epoch,
    )


def consume_ai_greeting(
    board: Board,
    generation: int,
    cursor: int,
    transport_epoch: int,
    timeout: float = 15.0,
) -> int:
    try:
        round_start = wait_ai_round_event(
            board,
            generation,
            "round_start",
            cursor,
            transport_epoch,
            timeout=timeout,
        )
    except TimeoutError:
        return cursor
    tts_caption = wait_ai_caption(
        board,
        generation,
        1,
        round_start.sequence,
        transport_epoch,
        timeout=45.0,
    )
    round_end = wait_ai_round_event(
        board,
        generation,
        "round_end",
        tts_caption.sequence,
        transport_epoch,
        timeout=30.0,
    )
    board.at.evidence.record(
        "step",
        board.at.port,
        "ai-greeting-consumed",
        session_generation=generation,
        round_start_sequence=round_start.sequence,
        tts_caption_sequence=tts_caption.sequence,
        round_end_sequence=round_end.sequence,
    )
    return round_end.sequence


def scenario_ai(board: Board, media_seconds: float) -> None:
    try:
        start = board.at.submit_intent("AT+AI=START", "AI_START", timeout=10.0)
        wait_request_operation(board, start, "ai-start", "completed", timeout=30.0)
        active = wait_session(
            board,
            lambda item: item.owner == "ai" and item.state == "ai-active",
            "AI active",
            timeout=10.0,
        )
        consume_ai_greeting(
            board,
            active.generation,
            start.cursor,
            start.transport_epoch,
        )
        baseline = query_media(board)
        prompt = board.at.submit_intent(
            "AT+AI=PROMPT,JOKE",
            "AI_PROMPT",
            timeout=10.0,
        )
        prompt_submitted = wait_request_operation(
            board,
            prompt,
            "ai-prompt",
            "submitted",
            timeout=10.0,
        )
        prompt_completed = wait_request_operation(
            board,
            prompt,
            "ai-prompt",
            "completed",
            timeout=40.0,
            after=prompt_submitted.sequence,
        )
        asr_caption = wait_ai_caption(
            board,
            active.generation,
            0,
            prompt_submitted.sequence,
            prompt.transport_epoch,
            expected_keyword="笑话",
        )
        round_start = wait_ai_round_event(
            board,
            active.generation,
            "round_start",
            asr_caption.sequence,
            prompt.transport_epoch,
        )
        caption = wait_ai_caption(
            board,
            active.generation,
            1,
            round_start.sequence,
            prompt.transport_epoch,
        )
        round_end = wait_ai_round_event(
            board,
            active.generation,
            "round_end",
            caption.sequence,
            prompt.transport_epoch,
        )
        if media_seconds > 0:
            time.sleep(media_seconds)
        wait_media(
            board,
            active.generation,
            video=False,
            baseline=baseline,
            require_rx_growth=True,
        )
        board.at.evidence.record(
            "step",
            board.at.port,
            "ai-round-caption",
            session_generation=active.generation,
            round_start_sequence=round_start.sequence,
            caption_sequence=caption.sequence,
            round_end_sequence=round_end.sequence,
        )

        extra = json.dumps(
            {"latitude": 31.2304, "longitude": 121.4737},
            separators=(",", ":"),
        )
        update = board.at.submit_intent(
            f"AT+AI=UPDATE,{quote_at(extra)}",
            "AI_UPDATE",
        )
        wait_request_operation(
            board,
            update,
            "ai-update-config",
            "completed",
            timeout=15.0,
        )
        interrupt = board.at.submit_intent(
            "AT+AI=INTERRUPT",
            "AI_INTERRUPT",
        )
        wait_request_operation(
            board,
            interrupt,
            "ai-interrupt",
            "submitted",
            timeout=8.0,
        )
        submit = board.at.submit_intent("AT+AI=SUBMIT", "AI_SUBMIT")
        wait_request_operation(
            board,
            submit,
            "ai-submit-speech",
            "submitted",
            timeout=8.0,
        )
        stop = board.at.submit_intent("AT+AI=STOP", "AI_STOP")
        wait_ai_state(board, stop, "idle", reason="ai-local-stop")
        wait_session(
            board,
            lambda item: item.owner == "none" and item.state == "idle",
            "AI idle",
        )
        board.at.send("AT")
        query_media(board)
    finally:
        ensure_idle(board)


def scenario_ai_prompts(board: Board, media_seconds: float) -> None:
    presets = ("STORY", "JOKE", "WEATHER")
    keyword_by_preset = {
        "STORY": "故事",
        "JOKE": "笑话",
        "WEATHER": "天气",
    }
    try:
        start = board.at.submit_intent("AT+AI=START", "AI_START", timeout=10.0)
        wait_request_operation(board, start, "ai-start", "completed", timeout=30.0)
        active = wait_session(
            board,
            lambda item: item.owner == "ai" and item.state == "ai-active",
            "AI active before prompt suite",
            timeout=10.0,
        )
        consume_ai_greeting(
            board,
            active.generation,
            start.cursor,
            start.transport_epoch,
        )

        for preset in presets:
            if preset == "WEATHER":
                extra = json.dumps(
                    {"latitude": 31.2304, "longitude": 121.4737},
                    separators=(",", ":"),
                )
                update = board.at.submit_intent(
                    f"AT+AI=UPDATE,{quote_at(extra)}",
                    "AI_UPDATE",
                )
                wait_request_operation(
                    board,
                    update,
                    "ai-update-config",
                    "completed",
                    timeout=15.0,
                )

            baseline = query_media(board)
            prompt = board.at.submit_intent(
                f"AT+AI=PROMPT,{preset}",
                "AI_PROMPT",
                timeout=10.0,
            )
            submitted = wait_request_operation(
                board,
                prompt,
                "ai-prompt",
                "submitted",
                timeout=10.0,
            )
            completed = wait_request_operation(
                board,
                prompt,
                "ai-prompt",
                "completed",
                timeout=40.0,
                after=submitted.sequence,
            )
            asr_caption = wait_ai_caption(
                board,
                active.generation,
                0,
                submitted.sequence,
                prompt.transport_epoch,
                timeout=45.0,
                expected_keyword=keyword_by_preset[preset],
            )
            round_start = wait_ai_round_event(
                board,
                active.generation,
                "round_start",
                asr_caption.sequence,
                prompt.transport_epoch,
                timeout=45.0,
            )
            tts_caption = wait_ai_caption(
                board,
                active.generation,
                1,
                round_start.sequence,
                prompt.transport_epoch,
                timeout=90.0,
            )
            round_end = wait_ai_round_event(
                board,
                active.generation,
                "round_end",
                tts_caption.sequence,
                prompt.transport_epoch,
                timeout=30.0,
            )
            if media_seconds > 0:
                time.sleep(media_seconds)
            wait_media(
                board,
                active.generation,
                video=False,
                baseline=baseline,
                require_rx_growth=True,
            )
            board.at.evidence.record(
                "step",
                board.at.port,
                "ai-prompt-round",
                preset=preset,
                session_generation=active.generation,
                submitted_sequence=submitted.sequence,
                completed_sequence=completed.sequence,
                round_start_sequence=round_start.sequence,
                asr_caption_sequence=asr_caption.sequence,
                tts_caption_sequence=tts_caption.sequence,
                round_end_sequence=round_end.sequence,
            )

        stop = board.at.submit_intent("AT+AI=STOP", "AI_STOP")
        wait_ai_state(board, stop, "idle", reason="ai-local-stop")
        wait_session(
            board,
            lambda item: item.owner == "none" and item.state == "idle",
            "AI idle after prompt suite",
        )
    finally:
        ensure_idle(board)


def wait_ai_call_action(
    board: Board,
    generation: int,
    cursor: int,
    transport_epoch: int,
    target_alias: str,
    timeout: float = 90.0,
) -> AtLine:
    def matches(line: AtLine) -> bool:
        fields = parse_csv_urc(line.text, "+AI:ACTION,")
        return (
            fields is not None
            and len(fields) == 5
            and to_int(fields[0], "ai-action.generation") == generation
            and fields[3] == "call_device"
        )

    line = board.at.wait_for(
        matches,
        timeout=timeout,
        description="AI call_device action",
        after=cursor,
        expected_epoch=transport_epoch,
    )
    fields = parse_csv_urc(line.text, "+AI:ACTION,")
    assert fields is not None
    try:
        params = json.loads(fields[4])
    except json.JSONDecodeError as exc:
        raise RuntimeError("AI call_device params are not valid JSON") from exc
    action = params.get("action") if isinstance(params, dict) else None
    data = params.get("data") if isinstance(params, dict) else None
    target = data.get("target") if isinstance(data, dict) else None
    if (
        action != "call_device"
        or not isinstance(target, str)
        or not target.strip()
        or target.strip() != target_alias.strip()
    ):
        raise RuntimeError("AI call_device target does not match the test alias")
    board.at.evidence.record(
        "step",
        board.at.port,
        "ai-call-device-action",
        transport_epoch=line.transport_epoch,
        sequence=line.sequence,
        session_generation=generation,
        action_id=fields[2],
        action="call_device",
    )
    return line


def scenario_ai_call_device(
    caller: Board,
    callee: Board,
    target_alias: str,
    media_seconds: float,
) -> None:
    target_alias = target_alias.strip()
    if not target_alias:
        raise ValueError("--target-alias must not be empty")
    prompt_by_alias = {
        "小张": "CALL_XIAOZHANG",
        "小李": "CALL_XIAOLI",
    }
    preset = prompt_by_alias.get(target_alias)
    if preset is None:
        raise ValueError(
            "ai-call-device supports the configured aliases 小张 or 小李"
        )
    original_contacts = query_device_contacts(caller)
    original = original_contacts.get(callee.device_id)
    if original is None:
        raise RuntimeError("callee is not a device contact of the AI board")
    if not original.online:
        raise RuntimeError("callee contact is offline before AI handoff")
    if original.remark != target_alias:
        raise RuntimeError(
            "callee contact remark does not exactly match --target-alias"
        )

    try:
        callee_epoch, callee_cursor = callee.at.checkpoint()
        start = caller.at.submit_intent(
            "AT+AI=START",
            "AI_START",
            timeout=10.0,
        )
        wait_request_operation(
            caller,
            start,
            "ai-start",
            "completed",
            timeout=30.0,
        )
        active = wait_session(
            caller,
            lambda item: item.owner == "ai" and item.state == "ai-active",
            "AI active before call_device",
            timeout=10.0,
        )
        consume_ai_greeting(
            caller,
            active.generation,
            start.cursor,
            start.transport_epoch,
        )
        prompt = caller.at.submit_intent(
            f"AT+AI=PROMPT,{preset}",
            "AI_PROMPT",
            timeout=10.0,
        )
        prompt_submitted = wait_request_operation(
            caller,
            prompt,
            "ai-prompt",
            "submitted",
            timeout=10.0,
        )
        prompt_completed = wait_request_operation(
            caller,
            prompt,
            "ai-prompt",
            "completed",
            timeout=40.0,
            after=prompt_submitted.sequence,
        )
        asr_caption = wait_ai_caption(
            caller,
            active.generation,
            0,
            prompt_submitted.sequence,
            prompt.transport_epoch,
            timeout=45.0,
            expected_keyword=target_alias,
        )
        action = wait_ai_call_action(
            caller,
            active.generation,
            max(prompt_completed.sequence, asr_caption.sequence),
            prompt.transport_epoch,
            target_alias,
        )
        phase_cursor = action.sequence
        for phase, timeout in (
            ("contacts-refresh-submitted", 15.0),
            ("response-submitted", 15.0),
            ("response-drained", 10.0),
            ("adapter-drained", 15.0),
        ):
            anchor = wait_operation_after(
                caller,
                "AI",
                "ai-call-device",
                phase,
                phase_cursor,
                start.transport_epoch,
                timeout=timeout,
            )
            phase_cursor = anchor.sequence
        accepted = wait_operation_after(
            caller,
            "CALL",
            "call-start",
            "accepted",
            phase_cursor,
            start.transport_epoch,
            timeout=20.0,
        )
        room_id, incoming = wait_incoming(
            callee,
            caller.device_id,
            "audio",
            callee_cursor,
            callee_epoch,
            timeout=20.0,
        )
        accept = callee.at.submit_intent("AT+ACCEPT", "CALL_ACCEPT")
        wait_request_operation(callee, accept, "call-accept", "accepted")
        caller_session = wait_session(
            caller,
            lambda item: (
                item.owner == "call"
                and item.state == "in-call"
                and item.room_id == room_id
                and item.peer_id == callee.device_id
            ),
            "AI caller in-call after handoff",
        )
        callee_session = wait_session(
            callee,
            lambda item: (
                item.owner == "call"
                and item.state == "in-call"
                and item.room_id == room_id
                and item.peer_id == caller.device_id
            ),
            "callee in-call after AI handoff",
        )
        if caller_session.generation <= active.generation:
            raise RuntimeError("CALL did not receive a generation after AI")
        if accept.cursor < incoming.sequence:
            raise RuntimeError("callee accepted before the incoming event")
        caller_baseline = query_media(caller)
        callee_baseline = query_media(callee)
        time.sleep(media_seconds)
        wait_media(
            caller,
            caller_session.generation,
            video=False,
            baseline=caller_baseline,
        )
        wait_media(
            callee,
            callee_session.generation,
            video=False,
            baseline=callee_baseline,
        )
        hangup = caller.at.submit_intent("AT+HANGUP", "CALL_HANGUP")
        wait_request_operation(
            caller,
            hangup,
            "call-hangup",
            "submitted",
        )
        for board in (caller, callee):
            wait_session(
                board,
                lambda item: item.owner == "none" and item.state == "idle",
                "idle after AI call_device hangup",
            )
        caller.at.evidence.record(
            "step",
            caller.at.port,
            "ai-call-device-established",
            action_sequence=action.sequence,
            handoff_sequence=accepted.sequence,
            incoming_sequence=incoming.sequence,
            ai_generation=active.generation,
            call_generation=caller_session.generation,
            room_id=room_id,
            peer_id=callee.device_id,
        )
    finally:
        for board in (caller, callee):
            ensure_idle(board)


def assert_settled_idle(board: Board, description: str) -> None:
    deadline = time.monotonic() + 150.0
    session: SessionSnapshot | None = None
    media: MediaSnapshot | None = None
    status: StatusSnapshot | None = None

    def is_settled() -> bool:
        assert session is not None and media is not None and status is not None
        return (
            session_is_canonical_idle(session)
            and media.adapter_state == "running"
            and not media.connected
            and media.active_profile == "none"
            and media.send_errors == 0
            and not media.connect_request_pending
            and not media.connect_callback_pending
            and media.accept_callbacks_pending == 0
            and media.disconnects_pending == 0
            and media.connection_users == 0
            and not media.incoming_armed
            and status.state == "READY"
            and status.owner == "none"
            and status.session_state == "idle"
            and status.wifi_online
            and status.platform_ready
            and status.mqtt_online
            and status.tirtc_ready
            and status.last_error == 0
        )

    while time.monotonic() < deadline:
        try:
            session = query_session(board)
            media = query_media(board)
            status = query_status(board)
        except (RuntimeError, TimeoutError, SerialTransportError):
            if recover_expected_restart(board):
                session = None
                media = None
                status = None
                deadline = time.monotonic() + 150.0
                continue
            raise
        if is_settled():
            restart_count = expected_restart_count(board)
            if restart_count > board.handled_restarts:
                consume_expected_restarts(
                    board,
                    "expected-device-restart-kept-transport",
                    board.app_generation,
                )
            break
        time.sleep(0.5)
    if session is None or media is None or status is None or not is_settled():
        raise RuntimeError(
            f"{board.name}: did not settle after {description}: "
            f"session={session} media={media} status={status}"
        )
    time.sleep(0.5)
    quiet_session = query_session(board)
    quiet_media = query_media(board)
    quiet_status = query_status(board)
    previous = (session, media, status)
    session, media, status = quiet_session, quiet_media, quiet_status
    if not is_settled() or (quiet_session, quiet_media, quiet_status) != previous:
        raise RuntimeError(
            f"{board.name}: cleanup state changed during quiet barrier after "
            f"{description}: before={previous} "
            f"after={(quiet_session, quiet_media, quiet_status)}"
        )
    assert_board_healthy(board)


def scenario_ai_stop_race(board: Board, iteration: int) -> None:
    windows = (
        ("request-accepted", 0.0),
        ("request-accepted", 0.05),
        ("whip-connect-submitted", 0.0),
        ("whip-connect-submitted", 0.02),
        ("whip-connect-submitted", 0.05),
        ("whip-connect-submitted", 0.1),
    )
    try:
        start = board.at.submit_intent("AT+AI=START", "AI_START", timeout=10.0)
        window, delay = windows[(iteration - 1) % len(windows)]
        if window == "whip-connect-submitted":
            accepted = wait_request_operation(
                board,
                start,
                "ai-start",
                "accepted",
                timeout=12.0,
            )
            accepted_fields = parse_csv_urc(accepted.text, "+AI:OP,")
            assert accepted_fields is not None
            wait_state_reason(
                board,
                start,
                "AI",
                "ai-connecting",
                "ai-whip-connecting",
                expected_generation=to_int(
                    accepted_fields[0],
                    "ai-race.accepted_generation",
                ),
                timeout=20.0,
            )
        time.sleep(delay)
        stop = board.at.submit_intent("AT+AI=STOP", "AI_STOP")
        stop_operation = board.at.wait_for(
            lambda line: (
                (fields := parse_csv_urc(line.text, "+AI:OP,")) is not None
                and len(fields) == 6
                and to_int(fields[1], "ai-stop.request_id") == stop.request_id
                and to_int(fields[2], "ai-stop.status") == 0
                and fields[3] == "ai-stop"
            ),
            20.0,
            "ai-stop overlap evidence",
            after=stop.cursor,
            expected_epoch=stop.transport_epoch,
        )
        stop_fields = parse_csv_urc(stop_operation.text, "+AI:OP,")
        assert stop_fields is not None
        stop_phase = stop_fields[4]
        if stop_phase not in (
            "local-cleanup-no-connection",
            "end-submitted",
            "end-send-failed",
        ):
            raise RuntimeError(
                f"{board.name}: unexpected ai-stop phase {stop_phase}"
            )
        try:
            stop_overlap = json.loads(stop_fields[5])
        except json.JSONDecodeError as exc:
            raise RuntimeError(
                f"{board.name}: ai-stop overlap evidence is not JSON"
            ) from exc
        if (
            not isinstance(stop_overlap, dict)
            or not isinstance(
                stop_overlap.get("connect_request_pending"), bool
            )
            or not isinstance(
                stop_overlap.get("connect_callback_pending"), bool
            )
        ):
            raise RuntimeError(
                f"{board.name}: ai-stop overlap evidence schema mismatch"
            )
        wait_ai_state(
            board,
            stop,
            "idle",
            reason="ai-local-stop",
            timeout=20.0,
        )
        assert_settled_idle(board, "AI stop/connect race")
        time.sleep(0.75)
        assert_settled_idle(board, "late AI callback guard")
        board.at.evidence.record(
            "step",
            board.at.port,
            "ai-stop-race",
            iteration=iteration,
            anchor=window,
            delay_seconds=delay,
            start_request_id=start.request_id,
            stop_request_id=stop.request_id,
            stop_phase=stop_phase,
            connect_request_pending=stop_overlap[
                "connect_request_pending"
            ],
            connect_callback_pending=stop_overlap[
                "connect_callback_pending"
            ],
        )
    finally:
        ensure_idle(board)


def establish_call(
    caller: Board,
    callee: Board,
    call_type: str,
) -> tuple[SessionSnapshot, SessionSnapshot]:
    callee_epoch, callee_cursor = callee.at.checkpoint()
    suffix = ",VIDEO" if call_type == "video" else ""
    start = caller.at.submit_intent(
        f"AT+CALL={quote_at(callee.device_id)}{suffix}",
        "CALL_START",
    )
    wait_request_operation(caller, start, "call-start", "accepted")
    room_id, incoming = wait_incoming(
        callee,
        caller.device_id,
        call_type,
        callee_cursor,
        callee_epoch,
    )
    accept = callee.at.submit_intent("AT+ACCEPT", "CALL_ACCEPT")
    wait_request_operation(callee, accept, "call-accept", "accepted")
    caller_session = wait_session(
        caller,
        lambda item: item.state == "in-call" and item.room_id == room_id,
        "caller in-call",
    )
    callee_session = wait_session(
        callee,
        lambda item: item.state == "in-call" and item.room_id == room_id,
        "callee in-call",
    )
    if (
        caller_session.peer_id != callee.device_id
        or callee_session.peer_id != caller.device_id
        or caller_session.call_type != call_type
        or callee_session.call_type != call_type
    ):
        raise RuntimeError("call peer/type snapshots are inconsistent")
    if accept.cursor < incoming.sequence:
        raise RuntimeError("callee accept request predates incoming event")
    caller.at.evidence.record(
        "step",
        caller.at.port,
        "call-established",
        room_id=room_id,
        call_type=call_type,
        caller_generation=caller_session.generation,
        callee_generation=callee_session.generation,
        peer_id=callee.device_id,
    )
    return caller_session, callee_session


def scenario_call_accept(
    caller: Board,
    callee: Board,
    call_type: str,
    media_seconds: float,
) -> None:
    caller_session, callee_session = establish_call(caller, callee, call_type)
    try:
        try:
            callee.at.submit_intent("AT+AI=START", "AI_START")
        except RequestRejectedError:
            pass
        else:
            raise RuntimeError("AI start was not rejected during an active call")
        caller_baseline = query_media(caller)
        callee_baseline = query_media(callee)
        time.sleep(media_seconds)
        wait_media(
            caller,
            caller_session.generation,
            video=call_type == "video",
            baseline=caller_baseline,
        )
        wait_media(
            callee,
            callee_session.generation,
            video=call_type == "video",
            baseline=callee_baseline,
        )
        hangup = caller.at.submit_intent("AT+HANGUP", "CALL_HANGUP")
        wait_request_operation(caller, hangup, "call-hangup", "submitted")
        wait_session(
            caller,
            lambda item: item.owner == "none" and item.state == "idle",
            "caller idle after hangup",
        )
        wait_session(
            callee,
            lambda item: item.owner == "none" and item.state == "idle",
            "callee idle after remote hangup",
        )
        for board in (caller, callee):
            board.at.send("AT")
            query_session(board)
            query_media(board)
    finally:
        for board in (caller, callee):
            ensure_idle(board)


def scenario_call_cancel_race(
    caller: Board,
    callee: Board,
    iteration: int,
    call_type: str,
) -> None:
    delays = (0.0, 0.02, 0.05, 0.1, 0.2)
    try:
        callee_epoch, callee_cursor = callee.at.checkpoint()
        suffix = ",VIDEO" if call_type == "video" else ""
        start = caller.at.submit_intent(
            f"AT+CALL={quote_at(callee.device_id)}{suffix}",
            "CALL_START",
        )
        wait_request_operation(caller, start, "call-start", "accepted")
        wait_request_operation(
            caller,
            start,
            "call-start",
            "room-created",
            timeout=20.0,
        )
        caller_session = wait_session(
            caller,
            lambda item: (
                item.owner == "call"
                and item.room_id != ""
                and item.peer_id == callee.device_id
                and item.call_type == call_type
                and item.request_id == start.request_id
            ),
            "caller room correlation before cancel race",
            timeout=10.0,
        )
        room_id = caller_session.room_id
        incoming_room_id, incoming = wait_incoming(
            callee,
            caller.device_id,
            call_type,
            callee_cursor,
            callee_epoch,
            expected_room_id=room_id,
        )
        if incoming_room_id != room_id:
            raise RuntimeError("incoming room correlation failed")
        incoming_fields = parse_csv_urc(incoming.text, "+CALL:INCOMING,")
        assert incoming_fields is not None
        callee_generation = to_int(
            incoming_fields[0], "incoming.session_generation"
        )
        callee_session = wait_session(
            callee,
            lambda item: (
                item.owner == "call"
                and item.state == "ringing"
                and item.pending
                and item.generation == callee_generation
                and item.room_id == room_id
                and item.peer_id == caller.device_id
                and item.call_type == call_type
            ),
            "callee incoming room correlation before cancel race",
            timeout=5.0,
        )
        accept = callee.at.submit_intent("AT+ACCEPT", "CALL_ACCEPT")
        wait_request_operation(callee, accept, "call-accept", "accepted")
        anchor = wait_state_reason(
            callee,
            accept,
            "CALL",
            "call-connecting",
            "call-p2p-connect-submitted",
            expected_generation=callee_generation,
            expected_room_id=room_id,
            timeout=20.0,
        )
        callee_terminal_epoch, callee_terminal_cursor = callee.at.checkpoint()
        delay = delays[(iteration - 1) % len(delays)]
        time.sleep(delay)
        terminal_submit_monotonic = time.monotonic()
        terminal = caller.at.submit_intent("AT+CANCEL", "CALL_CANCEL")
        cancelled = True
        cancel_status = 0
        try:
            wait_request_operation(
                caller,
                terminal,
                "call-cancel",
                "submitted",
                timeout=8.0,
            )
        except OperationResultError as exc:
            cancelled = False
            cancel_status = exc.status
            current = query_session(caller)
            if (
                exc.operation == "call-cancel"
                and exc.status == ESP_ERR_INVALID_STATE_STATUS
                and exc.phase == "no-outgoing-call"
                and current.owner == "call"
                and current.state == "in-call"
                and current.generation == caller_session.generation
                and current.room_id == room_id
            ):
                terminal = caller.at.submit_intent(
                    "AT+HANGUP",
                    "CALL_HANGUP",
                )
                wait_request_operation(
                    caller,
                    terminal,
                    "call-hangup",
                    "submitted",
                    timeout=8.0,
                )
            else:
                raise RuntimeError(
                    f"{caller.name}: call cancel lost without an active call; "
                    f"session={current}"
                ) from exc

        if cancelled:
            wait_call_ending(
                caller,
                caller_session.generation,
                {terminal.request_id},
                room_id,
                {("call-local-cancel", 0)},
                terminal.cursor,
                terminal.transport_epoch,
            )
            wait_call_ending(
                callee,
                callee_session.generation,
                {0, accept.request_id},
                room_id,
                {
                    ("call-room-cancel", 0),
                    ("call-peer-disconnected", -40008),
                },
                callee_terminal_cursor,
                callee_terminal_epoch,
            )
        else:
            wait_call_ending(
                caller,
                caller_session.generation,
                {terminal.request_id},
                room_id,
                {("call-local-hangup", 0)},
                terminal.cursor,
                terminal.transport_epoch,
            )
            wait_call_ending(
                callee,
                callee_session.generation,
                {0, accept.request_id},
                room_id,
                {
                    ("call-remote-hangup", 0),
                    ("call-room-cancel", 0),
                },
                callee_terminal_cursor,
                callee_terminal_epoch,
            )

        for board in (caller, callee):
            assert_settled_idle(board, "call cancel/connect race")
        time.sleep(0.75)
        for board in (caller, callee):
            assert_settled_idle(board, "late call callback guard")
        caller.at.evidence.record(
            "step",
            caller.at.port,
            "call-cancel-race",
            iteration=iteration,
            scheduled_delay_seconds=delay,
            anchor_monotonic=anchor.monotonic,
            terminal_submit_monotonic=terminal_submit_monotonic,
            observed_delay_seconds=terminal_submit_monotonic - anchor.monotonic,
            call_type=call_type,
            caller=caller.name,
            callee=callee.name,
            room_id=room_id,
            caller_generation=caller_session.generation,
            callee_generation=callee_session.generation,
            connect_anchor_sequence=anchor.sequence,
            cancel_won=cancelled,
            cancel_status=cancel_status,
            terminal_request_id=terminal.request_id,
        )
    finally:
        for board in (caller, callee):
            ensure_idle(board)


def scenario_call_terminal(
    caller: Board,
    callee: Board,
    action: str,
) -> None:
    try:
        callee_epoch, callee_cursor = callee.at.checkpoint()
        start = caller.at.submit_intent(
            f"AT+CALL={quote_at(callee.device_id)}",
            "CALL_START",
        )
        wait_request_operation(caller, start, "call-start", "accepted")
        room_id, incoming = wait_incoming(
            callee,
            caller.device_id,
            "audio",
            callee_cursor,
            callee_epoch,
        )
        incoming_fields = parse_csv_urc(incoming.text, "+CALL:INCOMING,")
        assert incoming_fields is not None
        callee_generation = to_int(
            incoming_fields[0],
            "terminal-call.callee-generation",
        )
        caller_session = wait_session(
            caller,
            lambda item: (
                item.owner == "call"
                and item.room_id == room_id
                and item.peer_id == callee.device_id
                and item.request_id == start.request_id
            ),
            "caller room correlation before terminal action",
        )
        callee_session = wait_session(
            callee,
            lambda item: (
                item.owner == "call"
                and item.state == "ringing"
                and item.pending
                and item.generation == callee_generation
                and item.room_id == room_id
                and item.peer_id == caller.device_id
            ),
            "callee room correlation before terminal action",
        )
        baselines = {
            caller.name: query_media(caller),
            callee.name: query_media(callee),
        }
        caller_terminal_epoch, caller_terminal_cursor = caller.at.checkpoint()
        callee_terminal_epoch, callee_terminal_cursor = callee.at.checkpoint()
        if action == "reject":
            terminal = callee.at.submit_intent("AT+REJECT", "CALL_REJECT")
            wait_request_operation(
                callee,
                terminal,
                "call-reject",
                "submitted",
            )
            wait_call_ending(
                callee,
                callee_session.generation,
                {terminal.request_id},
                room_id,
                {("call-local-reject", 0)},
                terminal.cursor,
                terminal.transport_epoch,
            )
            wait_call_ending(
                caller,
                caller_session.generation,
                {0, start.request_id},
                room_id,
                {("call-room-cancel", 0)},
                caller_terminal_cursor,
                caller_terminal_epoch,
            )
        elif action == "cancel":
            terminal = caller.at.submit_intent("AT+CANCEL", "CALL_CANCEL")
            wait_request_operation(
                caller,
                terminal,
                "call-cancel",
                "submitted",
            )
            wait_call_ending(
                caller,
                caller_session.generation,
                {terminal.request_id},
                room_id,
                {("call-local-cancel", 0)},
                terminal.cursor,
                terminal.transport_epoch,
            )
            wait_call_ending(
                callee,
                callee_session.generation,
                {0},
                room_id,
                {
                    ("call-room-cancel", 0),
                    ("call-peer-disconnected", -40008),
                },
                callee_terminal_cursor,
                callee_terminal_epoch,
            )
        else:
            raise ValueError(action)
        for board in (caller, callee):
            wait_session(
                board,
                lambda item: item.owner == "none" and item.state == "idle",
                f"idle after {action}",
            )
            board.at.send("AT")
            media = query_media(board)
            baseline = baselines[board.name]
            before = (
                baseline.tx_audio_frames,
                baseline.tx_audio_bytes,
                baseline.tx_video_frames,
                baseline.tx_video_bytes,
                baseline.rx_audio_frames,
                baseline.rx_audio_bytes,
                baseline.rx_video_frames,
                baseline.rx_video_bytes,
                baseline.send_errors,
            )
            after = (
                media.tx_audio_frames,
                media.tx_audio_bytes,
                media.tx_video_frames,
                media.tx_video_bytes,
                media.rx_audio_frames,
                media.rx_audio_bytes,
                media.rx_video_frames,
                media.rx_video_bytes,
                media.send_errors,
            )
            if media.connected or media.active_profile != "none" or after != before:
                raise RuntimeError(
                    f"{board.name}: media changed during {action}: "
                    f"before={baseline} after={media}"
                )
    finally:
        for board in (caller, callee):
            ensure_idle(board)


def scenario_busy(caller: Board, ai_board: Board, media_seconds: float) -> None:
    try:
        start = ai_board.at.submit_intent("AT+AI=START", "AI_START")
        wait_request_operation(ai_board, start, "ai-start", "completed", timeout=30.0)
        active = wait_session(
            ai_board,
            lambda item: item.owner == "ai" and item.state == "ai-active",
            "AI active before busy call",
            timeout=30.0,
        )
        baseline = query_media(ai_board)
        call = caller.at.submit_intent(
            f"AT+CALL={quote_at(ai_board.device_id)}",
            "CALL_START",
        )
        wait_request_operation(caller, call, "call-start", "accepted")
        calling = wait_session(
            caller,
            lambda item: (
                item.owner == "call"
                and item.state == "calling"
                and item.room_id != ""
            ),
            "busy call room assigned",
            timeout=15.0,
        )

        def busy_reject(line: AtLine) -> bool:
            fields = parse_csv_urc(line.text, "+CALL:EVENT,")
            return (
                fields is not None
                and len(fields) >= 6
                and fields[3] == "call-reject"
                and fields[4] == calling.room_id
                and '"reason":"busy"' in fields[5]
            )

        caller.at.wait_for(
            busy_reject,
            timeout=25.0,
            description="busy call rejection",
            after=call.cursor,
            expected_epoch=call.transport_epoch,
        )
        wait_session(
            caller,
            lambda item: item.owner == "none" and item.state == "idle",
            "caller idle after busy reject",
            timeout=25.0,
        )
        still_active = query_session(ai_board)
        if (
            still_active.owner != "ai"
            or still_active.state != "ai-active"
            or still_active.generation != active.generation
        ):
            raise RuntimeError("busy call displaced the active AI session")
        time.sleep(media_seconds)
        wait_media(
            ai_board,
            active.generation,
            video=False,
            baseline=baseline,
            require_tx_growth=False,
            require_rx_growth=False,
        )
        stop = ai_board.at.submit_intent("AT+AI=STOP", "AI_STOP")
        wait_ai_state(ai_board, stop, "idle", reason="ai-local-stop")
        wait_session(
            ai_board,
            lambda item: item.owner == "none" and item.state == "idle",
            "AI idle after busy scenario",
        )
    finally:
        for board in (caller, ai_board):
            ensure_idle(board)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "scenario",
        choices=(
            "preflight",
            "ai",
            "ai-prompts",
            "ai-call-device",
            "call-audio",
            "call-video",
            "call-reject",
            "call-cancel",
            "call-busy",
            "simple-cn",
            "race-ai-stop",
            "race-call-cancel",
            "race-suite",
            "suite",
        ),
    )
    parser.add_argument("--port-a", required=True)
    parser.add_argument("--port-b")
    parser.add_argument("--startup-timeout", type=float, default=90.0)
    parser.add_argument("--media-seconds", type=float, default=8.0)
    parser.add_argument("--iterations", type=int, default=1)
    parser.add_argument("--target-alias")
    parser.add_argument("--artifact-dir", type=Path, default=artifact_dir())
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=Path("build-hardware-final"),
    )
    return parser.parse_args()


def file_identity(path: Path) -> dict[str, object]:
    resolved = path.resolve()
    if not resolved.is_file():
        raise RuntimeError(f"build evidence file is missing: {resolved}")
    return {
        "path": str(resolved),
        "size": resolved.stat().st_size,
        "sha256": hashlib.sha256(resolved.read_bytes()).hexdigest(),
        "modified_ns": resolved.stat().st_mtime_ns,
    }


def source_tree_identity() -> dict[str, object]:
    roots = (
        REPO_ROOT / "main",
        REPO_ROOT / "components",
        REPO_ROOT / "tools",
        REPO_ROOT / "docs",
        REPO_ROOT / "media",
    )
    files: list[Path] = []
    for root in roots:
        files.extend(
            path
            for path in root.rglob("*")
            if path.is_file()
            and "__pycache__" not in path.parts
        )
    files.extend(
        path
        for path in (
            REPO_ROOT / "CMakeLists.txt",
            REPO_ROOT / "sdkconfig",
            REPO_ROOT / "sdkconfig.defaults",
            REPO_ROOT / "partitions.csv",
            REPO_ROOT / "README.md",
            REPO_ROOT / "VERSION.md",
        )
        if path.is_file()
    )
    digest = hashlib.sha256()
    unique_files = set(files)
    for path in sorted(unique_files, key=lambda item: item.as_posix()):
        relative = path.relative_to(REPO_ROOT).as_posix()
        digest.update(relative.encode("utf-8"))
        digest.update(b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    return {
        "sha256": digest.hexdigest(),
        "file_count": len(unique_files),
    }


def collect_build_identity(build_dir: Path) -> dict[str, object]:
    resolved_build = (
        build_dir if build_dir.is_absolute() else REPO_ROOT / build_dir
    ).resolve()
    description_path = resolved_build / "project_description.json"
    description = json.loads(description_path.read_text(encoding="utf-8"))
    firmware_path = resolved_build / str(description["app_bin"])
    elf_path = resolved_build / str(description["app_elf"])
    generated_config = resolved_build / "config" / "sdkconfig.h"
    configured_sdkconfig = Path(str(description["config_file"])).resolve()
    described_build = Path(str(description["build_dir"])).resolve()
    described_project = Path(str(description["project_path"])).resolve()
    raw_defaults = description["config_defaults"]
    described_defaults = [
        Path(str(item)).resolve()
        for item in (
            raw_defaults
            if isinstance(raw_defaults, list)
            else [raw_defaults]
        )
    ]
    if described_build != resolved_build:
        raise RuntimeError(
            f"build description points to {described_build}, "
            f"not {resolved_build}"
        )
    if described_project != REPO_ROOT:
        raise RuntimeError(
            f"build belongs to {described_project}, not {REPO_ROOT}"
        )
    if configured_sdkconfig != (REPO_ROOT / "sdkconfig").resolve():
        raise RuntimeError(
            f"build uses unexpected sdkconfig: {configured_sdkconfig}"
        )
    if described_defaults != [(REPO_ROOT / "sdkconfig.defaults").resolve()]:
        raise RuntimeError(
            f"build uses unexpected config defaults: {described_defaults}"
        )
    if (
        description["project_name"]
        != "tirtc_esp32s3_at_thingconnect_demo"
        or description["target"] != "esp32s3"
        or description["git_revision"] != "v5.5.4"
    ):
        raise RuntimeError("project name, target, or ESP-IDF version mismatch")
    config_text = generated_config.read_text(encoding="utf-8")
    sockets = re.search(
        r"^#define CONFIG_LWIP_MAX_SOCKETS (\d+)$",
        config_text,
        re.MULTILINE,
    )
    if sockets is None or int(sockets.group(1)) != 16:
        raise RuntimeError(
            f"{generated_config}: firmware socket budget is not 16"
        )
    firmware_inputs = [
        path
        for root in (
            REPO_ROOT / "main",
            REPO_ROOT / "components",
        )
        for path in root.rglob("*")
        if path.is_file() and "__pycache__" not in path.parts
    ]
    firmware_inputs.extend(
        path
        for path in (
            REPO_ROOT / "CMakeLists.txt",
            configured_sdkconfig,
            *described_defaults,
            REPO_ROOT / "partitions.csv",
        )
        if path.is_file()
    )
    newest_input_ns = max(path.stat().st_mtime_ns for path in firmware_inputs)
    if firmware_path.stat().st_mtime_ns < newest_input_ns:
        raise RuntimeError(
            f"{firmware_path}: firmware is older than current source/config"
        )
    storage_path = resolved_build / "storage.bin"
    newest_media_ns = max(
        path.stat().st_mtime_ns
        for path in (REPO_ROOT / "media").rglob("*")
        if path.is_file()
    )
    if storage_path.stat().st_mtime_ns < newest_media_ns:
        raise RuntimeError(
            f"{storage_path}: storage image is older than recorded media"
        )
    sdk_manifest_path = (
        REPO_ROOT / "components/tirtc_sdk/manifest/build-info.json"
    )
    sdk_manifest_data = json.loads(
        sdk_manifest_path.read_text(encoding="utf-8")
    )
    if sdk_manifest_data["library"]["sha256"].lower() != file_identity(
        REPO_ROOT / "components/tirtc_sdk/lib/esp32s3/libTiRTC.a"
    )["sha256"]:
        raise RuntimeError("TiRTC archive does not match its SDK manifest")
    return {
        "firmware": file_identity(firmware_path),
        "elf": file_identity(elf_path),
        "bootloader": file_identity(
            resolved_build / "bootloader/bootloader.bin"
        ),
        "partition_table": file_identity(
            resolved_build / "partition_table/partition-table.bin"
        ),
        "ota_data": file_identity(resolved_build / "ota_data_initial.bin"),
        "storage": file_identity(storage_path),
        "flash_manifest": file_identity(
            resolved_build / "flasher_args.json"
        ),
        "sdk_archive": file_identity(
            REPO_ROOT / "components/tirtc_sdk/lib/esp32s3/libTiRTC.a"
        ),
        "sdk_manifest": file_identity(sdk_manifest_path),
        "sdk_manifest_data": sdk_manifest_data,
        "configured_sdkconfig": file_identity(configured_sdkconfig),
        "generated_sdkconfig": file_identity(generated_config),
        "project_description": file_identity(description_path),
        "media": {
            "audio": file_identity(
                REPO_ROOT
                / "media/audio_g711a_8khz_mono_20ms_2s_100packets.g711a"
            ),
            "video": file_identity(
                REPO_ROOT
                / "media/video_h264_annexb_640x480_15fps_8s_120frames.h264"
            ),
            "ai_story": file_identity(REPO_ROOT / "media/ai_story.g711a"),
            "ai_joke": file_identity(REPO_ROOT / "media/ai_joke.g711a"),
            "ai_weather": file_identity(REPO_ROOT / "media/ai_weather.g711a"),
            "ai_call_xiaozhang": file_identity(
                REPO_ROOT / "media/ai_call_xiaozhang.g711a"
            ),
            "ai_call_xiaoli": file_identity(
                REPO_ROOT / "media/ai_call_xiaoli.g711a"
            ),
        },
        "metadata": {
            "project_name": str(description["project_name"]),
            "running_project_name": str(description["project_name"])[:31],
            "project_version": str(description["project_version"]),
            "idf_version": str(description["git_revision"]),
            "target": str(description["target"]),
        },
        "idf_path": str(description["idf_path"]),
        "target": str(description["target"]),
        "lwip_max_sockets": int(sockets.group(1)),
        "source_tree": source_tree_identity(),
    }


def write_summary(
    args: argparse.Namespace,
    boards: list[Board],
    evidence: EvidenceWriter,
    status: str,
    build_identity: dict[str, object],
    error: BaseException | None = None,
) -> Path:
    steps = evidence.steps()
    iteration_started = sum(
        1
        for item in steps
        if item.get("kind") == "scenario"
        and item.get("text") == "iteration-start"
    )
    iteration_passed = sum(
        1
        for item in steps
        if item.get("kind") == "scenario"
        and item.get("text") == "iteration-passed"
    )
    ai_race_coverage: dict[str, int] = {}
    call_race_coverage: dict[str, int] = {}
    for item in steps:
        if item.get("text") == "ai-stop-race":
            key = (
                f"{item.get('anchor')}@"
                f"{item.get('delay_seconds')}s:{item.get('stop_phase')}"
            )
            ai_race_coverage[key] = ai_race_coverage.get(key, 0) + 1
        elif item.get("text") == "call-cancel-race":
            outcome = (
                "cancel-won" if item.get("cancel_won") else "connect-won"
            )
            key = (
                f"{item.get('caller')}->{item.get('callee')}:"
                f"{item.get('call_type')}@"
                f"{item.get('scheduled_delay_seconds')}s:{outcome}"
            )
            call_race_coverage[key] = call_race_coverage.get(key, 0) + 1
    summary = {
        "version": 4,
        "scenario": args.scenario,
        "iterations": args.iterations,
        "status": status,
        "error": safe_exception_summary(error) if error is not None else "",
        "iteration_progress": {
            "requested": args.iterations,
            "started": iteration_started,
            "passed": iteration_passed,
        },
        "coverage": {
            "ai_stop_race": ai_race_coverage,
            "call_cancel_race": call_race_coverage,
        },
        "build_identity": build_identity,
        "boards": [
            {
                "name": board.name,
                "port": board.at.port,
                "device_id": (
                    "<redacted-device>" if board.status is not None else ""
                ),
                "running_build": asdict(board.build) if board.build else None,
                "health": board.at.health_snapshot(),
            }
            for board in boards
        ],
        "raw_evidence": evidence.identity(),
        "steps": steps,
    }
    path = args.artifact_dir / "summary.json"
    path.write_text(
        json.dumps(summary, ensure_ascii=True, indent=2) + "\n",
        encoding="utf-8",
    )
    return path


def assert_call_race_gate(args: argparse.Namespace, evidence: EvidenceWriter) -> None:
    if (
        args.scenario not in ("race-call-cancel", "race-suite")
        or args.iterations < 20
    ):
        return
    records = [
        item
        for item in evidence.steps()
        if item.get("text") == "call-cancel-race"
    ]
    expected_delays = {0.0, 0.02, 0.05, 0.1, 0.2}
    expected_routes = {
        ("A", "B", "audio"),
        ("B", "A", "audio"),
        ("A", "B", "video"),
        ("B", "A", "video"),
    }
    for caller, callee, call_type in expected_routes:
        route_records = [
            item
            for item in records
            if item.get("caller") == caller
            and item.get("callee") == callee
            and item.get("call_type") == call_type
        ]
        delays = {
            float(item["scheduled_delay_seconds"])
            for item in route_records
        }
        cancel_wins = sum(
            1 for item in route_records if item.get("cancel_won") is True
        )
        connect_wins = sum(
            1 for item in route_records if item.get("cancel_won") is False
        )
        drifted = [
            (
                float(item["scheduled_delay_seconds"]),
                float(item["observed_delay_seconds"]),
            )
            for item in route_records
            if (
                float(item["observed_delay_seconds"])
                < max(0.0, float(item["scheduled_delay_seconds"]) - 0.01)
                or float(item["observed_delay_seconds"])
                > float(item["scheduled_delay_seconds"]) + 0.08
            )
        ]
        statuses_ok = all(
            (
                item.get("cancel_won") is True
                and int(item.get("cancel_status", -1)) == 0
            )
            or (
                item.get("cancel_won") is False
                and int(item.get("cancel_status", -1))
                == ESP_ERR_INVALID_STATE_STATUS
            )
            for item in route_records
        )
        if (
            delays != expected_delays
            or cancel_wins == 0
            or connect_wins == 0
            or drifted
            or not statuses_ok
        ):
            raise RuntimeError(
                "call cancel race gate incomplete for "
                f"{caller}->{callee}:{call_type}: "
                f"delays={sorted(delays)} cancel_wins={cancel_wins} "
                f"connect_wins={connect_wins} drifted={drifted} "
                f"statuses_ok={statuses_ok}"
            )


def assert_ai_race_gate(args: argparse.Namespace, evidence: EvidenceWriter) -> None:
    if args.scenario not in ("race-ai-stop", "race-suite") or args.iterations < 6:
        return
    records = [
        item
        for item in evidence.steps()
        if item.get("text") == "ai-stop-race"
    ]
    expected_windows = {
        ("request-accepted", 0.0),
        ("request-accepted", 0.05),
        ("whip-connect-submitted", 0.0),
        ("whip-connect-submitted", 0.02),
        ("whip-connect-submitted", 0.05),
        ("whip-connect-submitted", 0.1),
    }
    observed_windows = {
        (str(item.get("anchor")), float(item["delay_seconds"]))
        for item in records
    }
    stop_phases = {str(item.get("stop_phase")) for item in records}
    required_stop_phases = {
        "local-cleanup-no-connection",
        "end-submitted",
    }
    if (
        not expected_windows.issubset(observed_windows)
        or not required_stop_phases.issubset(stop_phases)
    ):
        raise RuntimeError(
            "AI stop race gate incomplete: "
            f"windows={sorted(observed_windows)} "
            f"stop_phases={sorted(stop_phases)}"
        )


def main() -> int:
    args = parse_args()
    if (
        args.scenario
        not in ("preflight", "ai", "ai-prompts", "race-ai-stop")
        and not args.port_b
    ):
        raise RuntimeError(f"{args.scenario} requires --port-b")
    if args.scenario == "ai-call-device" and not args.target_alias:
        raise RuntimeError("ai-call-device requires --target-alias")
    if args.iterations < 1:
        raise RuntimeError("--iterations must be positive")
    if args.port_b and args.port_a.casefold() == args.port_b.casefold():
        raise RuntimeError("--port-a and --port-b must identify different boards")
    build_identity = collect_build_identity(args.build_dir)

    evidence = EvidenceWriter(args.artifact_dir)
    boards = [Board("A", AtDevice(args.port_a, evidence))]
    if args.port_b:
        boards.append(Board("B", AtDevice(args.port_b, evidence)))
    try:
        for board in boards:
            board.at.open()
        preflight(boards, args.startup_timeout, build_identity)
        for iteration in range(1, args.iterations + 1):
            evidence.record(
                "scenario",
                "-",
                "iteration-start",
                scenario=args.scenario,
                iteration=iteration,
            )
            for board in boards:
                ensure_idle(board)
                assert_board_healthy(board)
            if args.scenario == "preflight":
                pass
            elif args.scenario == "ai":
                scenario_ai(boards[0], args.media_seconds)
            elif args.scenario == "ai-prompts":
                scenario_ai_prompts(boards[0], args.media_seconds)
            elif args.scenario == "ai-call-device":
                scenario_ai_call_device(
                    boards[0],
                    boards[1],
                    args.target_alias,
                    args.media_seconds,
                )
            elif args.scenario == "call-audio":
                scenario_call_accept(boards[0], boards[1], "audio", args.media_seconds)
            elif args.scenario == "call-video":
                scenario_call_accept(boards[0], boards[1], "video", args.media_seconds)
            elif args.scenario == "call-reject":
                scenario_call_terminal(boards[0], boards[1], "reject")
            elif args.scenario == "call-cancel":
                scenario_call_terminal(boards[0], boards[1], "cancel")
            elif args.scenario == "call-busy":
                scenario_busy(boards[0], boards[1], args.media_seconds)
            elif args.scenario == "simple-cn":
                scenario_simple_cn(
                    boards[0],
                    boards[1],
                    args.media_seconds,
                )
            elif args.scenario == "race-ai-stop":
                scenario_ai_stop_race(boards[0], iteration)
            elif args.scenario == "race-call-cancel":
                cell = (iteration - 1) % 20
                direction = (cell // 5) % 2
                call_type = "audio" if cell < 10 else "video"
                scenario_call_cancel_race(
                    boards[direction],
                    boards[1 - direction],
                    iteration,
                    call_type,
                )
            elif args.scenario == "race-suite":
                scenario_ai_stop_race(boards[0], iteration)
                cell = (iteration - 1) % 20
                direction = (cell // 5) % 2
                call_type = "audio" if cell < 10 else "video"
                scenario_call_cancel_race(
                    boards[direction],
                    boards[1 - direction],
                    iteration,
                    call_type,
                )
            else:
                scenario_ai(boards[0], args.media_seconds)
                scenario_call_terminal(boards[0], boards[1], "reject")
                scenario_call_terminal(boards[0], boards[1], "cancel")
                scenario_call_accept(
                    boards[0],
                    boards[1],
                    "audio",
                    args.media_seconds,
                )
                scenario_busy(boards[0], boards[1], args.media_seconds)
                scenario_call_accept(
                    boards[1],
                    boards[0],
                    "video",
                    args.media_seconds,
                )
            if args.scenario == "simple-cn":
                for board in boards:
                    set_protocol_mode(board, "RAW")
            for board in boards:
                assert_settled_idle(board, "iteration final barrier")
                if args.scenario != "preflight":
                    assert_platform_canary(board)
                query_build(board, build_identity)
                assert_board_healthy(board)
            evidence.record(
                "scenario",
                "-",
                "iteration-passed",
                scenario=args.scenario,
                iteration=iteration,
            )
        assert_ai_race_gate(args, evidence)
        assert_call_race_gate(args, evidence)
        for board in boards:
            query_status(board)
            query_session(board)
            query_build(board, build_identity)
            assert_settled_idle(board, "final health barrier")
            assert_board_healthy(board)
        final_build_identity = collect_build_identity(args.build_dir)
        if final_build_identity != build_identity:
            raise RuntimeError(
                "local build/source identity changed during hardware scenario"
            )
        if args.scenario == "simple-cn":
            for board in boards:
                set_protocol_mode(board, "USER")
                _, status = query_user_status(board)
                if status != ["已就绪", "空闲"]:
                    raise RuntimeError(
                        f"{board.name}: final USER surface is not ready and idle"
                    )
        for board in boards:
            board.at.close()
        for board in boards:
            assert_board_healthy(board)
        evidence.record("summary", "-", f"{args.scenario}:passed")
        summary_path = write_summary(
            args,
            boards,
            evidence,
            "passed",
            build_identity,
        )
        print(f"evidence={evidence.path.resolve()}")
        print(f"summary={summary_path.resolve()}")
        return 0
    except Exception as exc:
        safe_error = safe_exception_summary(exc)
        evidence.record("summary", "-", f"{args.scenario}:failed:{safe_error}")
        for board in boards:
            try:
                if args.scenario == "simple-cn":
                    set_protocol_mode(board, "RAW")
                ensure_idle(board, timeout=8.0)
            except Exception as cleanup_error:
                evidence.record(
                    "cleanup",
                    board.at.port,
                    f"failed:{safe_exception_summary(cleanup_error)}",
                )
            finally:
                if args.scenario == "simple-cn":
                    try:
                        set_protocol_mode(board, "USER")
                    except Exception as cleanup_error:
                        evidence.record(
                            "cleanup",
                            board.at.port,
                            "USER restore failed:"
                            f"{safe_exception_summary(cleanup_error)}",
                        )
        for board in boards:
            board.at.close()
        write_summary(
            args,
            boards,
            evidence,
            "failed",
            build_identity,
            exc,
        )
        print(f"{args.scenario} failed: {safe_error}", file=sys.stderr)
        return 1
    finally:
        for board in boards:
            board.at.close()
        evidence.close()


if __name__ == "__main__":
    raise SystemExit(main())
