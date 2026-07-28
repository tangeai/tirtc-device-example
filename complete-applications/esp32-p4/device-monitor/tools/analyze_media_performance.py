#!/usr/bin/env python3
"""Summarize ESP32-P4 media performance from a serial log.

The analyzer deliberately uses the firmware's existing low-rate statistics.
It does not require extra runtime tracing or change the media data path.
"""

from __future__ import annotations

import argparse
import re
import statistics
import sys
from dataclasses import dataclass, field
from pathlib import Path


TIMESTAMP_RE = re.compile(r"^(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})")
TARGET_RE = re.compile(r"target=(\d+)x(\d+)@(\d+)")
ACTIVE_RUNTIME_RE = re.compile(r"camera=1 rtc=1 (\d+)x(\d+) fps=([0-9.]+) bitrate=(\d+)kbps")
CLOSE_SNAPSHOT_RE = re.compile(
    r"rtc close snapshot:.*?age_ms=(\d+).*?"
    r"tx\[attempt=(\d+) fail=(\d+) v=(\d+)/(\d+)KB a=(\d+)/(\d+)KB\].*?"
    r"sdk_buf=(\d+)/(\d+)"
)
DOWNLINK_RE = re.compile(
    r"rx=([0-9.]+)fps/(\d+)kbps queued=([0-9.]+)fps "
    r"decoded=([0-9.]+)fps converted=([0-9.]+)fps presented=([0-9.]+)fps.*?"
    r"drop=input:(\d+) display:(\d+) fail=decode:(\d+) convert:(\d+).*?"
    r"sync=create:(\d+) restart:(\d+) reset:(\d+) overflow:(\d+)"
)
PAYLOAD_RE = re.compile(r"payload\[min/avg/max\]=(\d+)/(\d+)/(\d+)")
LUMA_SOURCE_CHANGE_RE = re.compile(r"luma_src_change=(\d+)/(\d+)")
LUMA_ENCODER_CHANGE_RE = re.compile(r"luma_enc_change=(\d+)/(\d+)")
NUMBER_RE_TEMPLATE = r"(?:^|\s){key}=(\d+)"
FLOAT_RE_TEMPLATE = r"(?:^|\s){key}=([0-9.]+)"

CRITICAL_PATTERNS = {
    "sdio_dma_oom": re.compile(r"SDIO RX no DMA memory|copy_buff.*assert", re.IGNORECASE),
    "dma_escrow_reclaim": re.compile(r"dma escrow: action=reclaim-failed", re.IGNORECASE),
    "downlink_decode_no_mem": re.compile(
        r"H264 downlink decode lost sync: ret=ESP_ERR_NO_MEM", re.IGNORECASE
    ),
    "heartbeat_timeout": re.compile(r"TIRTC_E_HEARTBEAT_TIMEOUT|-40007"),
    "local_send_buffer_disconnect": re.compile(r"rtc send buffer stale:"),
    "panic": re.compile(r"Guru Meditation|assert failed|abort\(\)|Backtrace:", re.IGNORECASE),
    "invalid_handle": re.compile(r"TIRTC_E_INVALID_HANDLE|-40002"),
}


def number(text: str, key: str, default: int = 0) -> int:
    match = re.search(NUMBER_RE_TEMPLATE.format(key=re.escape(key)), text)
    return int(match.group(1)) if match else default


def decimal(text: str, key: str, default: float = 0.0) -> float:
    match = re.search(FLOAT_RE_TEMPLATE.format(key=re.escape(key)), text)
    return float(match.group(1)) if match else default


@dataclass
class SampleSet:
    camera: list[dict[str, float | int]] = field(default_factory=list)
    tx: list[dict[str, float | int]] = field(default_factory=list)
    downlink: list[dict[str, float | int]] = field(default_factory=list)
    active_runtime: list[dict[str, float | int]] = field(default_factory=list)
    close_snapshots: list[dict[str, int]] = field(default_factory=list)
    first_timestamp: str | None = None
    last_timestamp: str | None = None
    first_upstream: bool = False
    subscribed: bool = False
    critical: dict[str, int] = field(default_factory=lambda: {key: 0 for key in CRITICAL_PATTERNS})


def parse_log(path: Path) -> SampleSet:
    samples = SampleSet()
    with path.open("r", encoding="utf-8-sig", errors="replace") as handle:
        for raw_line in handle:
            line = raw_line.strip()
            timestamp = TIMESTAMP_RE.match(line)
            if timestamp:
                samples.first_timestamp = samples.first_timestamp or timestamp.group(1)
                samples.last_timestamp = timestamp.group(1)

            samples.first_upstream |= "camera pipeline first upstream frame" in line
            samples.subscribed |= "remote local video subscription" in line and "subscribed=1" in line
            for name, pattern in CRITICAL_PATTERNS.items():
                if pattern.search(line):
                    samples.critical[name] += 1

            if "camera pipeline stats:" in line:
                target = TARGET_RE.search(line)
                payload = PAYLOAD_RE.search(line)
                source_change = LUMA_SOURCE_CHANGE_RE.search(line)
                encoder_change = LUMA_ENCODER_CHANGE_RE.search(line)
                samples.camera.append(
                    {
                        "width": int(target.group(1)) if target else 0,
                        "height": int(target.group(2)) if target else 0,
                        "target_fps": int(target.group(3)) if target else 0,
                        "fps": decimal(line, "fps"),
                        "bitrate": number(line, "bitrate"),
                        "encoded": number(line, "encoded"),
                        "upstream": number(line, "upstream"),
                        "drop": number(line, "drop"),
                        "cap_fail": number(line, "cap_fail"),
                        "enc_fail": number(line, "enc_fail"),
                        "max_gap_us": number(line, "max_gap_us"),
                        "avg_loop_us": number(line, "avg_loop_us"),
                        "dma_largest": number(line, "dma_largest"),
                        "payload_avg": int(payload.group(2)) if payload else 0,
                        "payload_max": int(payload.group(3)) if payload else 0,
                        "luma_src_delta": decimal(line, "luma_src_delta"),
                        "luma_src_transitions": int(source_change.group(2)) if source_change else 0,
                        "luma_enc_delta": decimal(line, "luma_enc_delta"),
                        "luma_enc_transitions": int(encoder_change.group(2)) if encoder_change else 0,
                    }
                )
            elif "local video tx stats:" in line:
                samples.tx.append(
                    {
                        "sent": number(line, "sent"),
                        "fps": decimal(line, "fps"),
                        "bitrate": number(line, "bitrate"),
                        "fail": number(line, "fail"),
                        "busy": number(line, "busy"),
                        "stale": number(line, "stale"),
                    }
                )
            elif "H264 downlink stats:" in line:
                downlink = DOWNLINK_RE.search(line)
                if downlink:
                    samples.downlink.append(
                        {
                            "rx_fps": float(downlink.group(1)),
                            "rx_bitrate": int(downlink.group(2)),
                            "queued_fps": float(downlink.group(3)),
                            "decoded_fps": float(downlink.group(4)),
                            "converted_fps": float(downlink.group(5)),
                            "presented_fps": float(downlink.group(6)),
                            "input_drops": int(downlink.group(7)),
                            "display_drops": int(downlink.group(8)),
                            "decode_failures": int(downlink.group(9)),
                            "convert_failures": int(downlink.group(10)),
                            "decoder_creations": int(downlink.group(11)),
                            "decoder_restarts": int(downlink.group(12)),
                            "resets": int(downlink.group(13)),
                            "overflows": int(downlink.group(14)),
                        }
                    )
            elif "runtime snapshot:" in line:
                active = ACTIVE_RUNTIME_RE.search(line)
                if active:
                    samples.active_runtime.append(
                        {
                            "width": int(active.group(1)),
                            "height": int(active.group(2)),
                            "fps": float(active.group(3)),
                            "bitrate": int(active.group(4)),
                            "dma_free": number(line, "dma_free"),
                            "dma_largest": number(line, "dma_largest"),
                            "internal_largest": number(line, "internal_largest"),
                            "psram_free": number(line, "psram_free"),
                            "video_q": number(line, "video_q"),
                            "free": number(line, "free"),
                            "rtc_sendbuf": number(line, "rtc_sendbuf"),
                        }
                    )
            elif "rtc close snapshot:" in line:
                close = CLOSE_SNAPSHOT_RE.search(line)
                if close:
                    samples.close_snapshots.append(
                        {
                            "age_ms": int(close.group(1)),
                            "attempts": int(close.group(2)),
                            "failures": int(close.group(3)),
                            "video_frames": int(close.group(4)),
                            "video_kb": int(close.group(5)),
                            "audio_frames": int(close.group(6)),
                            "audio_kb": int(close.group(7)),
                            "send_buffer_used": int(close.group(8)),
                            "send_buffer_limit": int(close.group(9)),
                        }
                    )
    return samples


def mean(items: list[dict[str, float | int]], key: str) -> float:
    values = [float(item[key]) for item in items]
    return statistics.fmean(values) if values else 0.0


def minimum(items: list[dict[str, float | int]], key: str) -> int:
    values = [int(item[key]) for item in items if int(item[key]) > 0]
    return min(values) if values else 0


@dataclass
class Check:
    level: str
    message: str


def evaluate(samples: SampleSet) -> tuple[str, list[Check]]:
    checks: list[Check] = []

    critical_total = sum(samples.critical.values())
    if critical_total:
        detail = ", ".join(f"{key}={value}" for key, value in samples.critical.items() if value)
        checks.append(Check("FAIL", f"critical runtime errors: {detail}"))
    else:
        checks.append(Check("PASS", "no DMA OOM, heartbeat timeout, invalid handle or panic detected"))

    if not samples.first_upstream:
        checks.append(Check("FAIL", "no first upstream H264 frame"))
    elif not samples.camera:
        close = samples.close_snapshots[-1] if samples.close_snapshots else None
        if close and close["video_frames"] > 0 and close["age_ms"] < 15_000:
            checks.append(
                Check(
                    "WARN",
                    "call ended before the 10-second camera statistics window; "
                    f"close snapshot still recorded {close['video_frames']} video frames",
                )
            )
        else:
            checks.append(Check("FAIL", "only startup frame observed; no sustained camera statistics"))
    else:
        checks.append(Check("PASS", f"camera produced {len(samples.camera)} sustained statistics samples"))

    if not samples.subscribed:
        checks.append(Check("WARN", "local video subscription confirmation was not found"))
    else:
        checks.append(Check("PASS", "local video subscription confirmed"))

    if samples.camera:
        target_fps = int(samples.camera[-1]["target_fps"])
        avg_fps = mean(samples.camera, "fps")
        fps_ratio = avg_fps / target_fps if target_fps else 0.0
        if fps_ratio >= 0.80:
            checks.append(Check("PASS", f"camera average fps {avg_fps:.1f}/{target_fps}"))
        elif fps_ratio >= 0.60:
            checks.append(Check("WARN", f"camera average fps {avg_fps:.1f}/{target_fps}"))
        else:
            checks.append(Check("FAIL", f"camera average fps {avg_fps:.1f}/{target_fps}"))

        upstream = sum(int(item["upstream"]) for item in samples.camera)
        dropped = sum(int(item["drop"]) for item in samples.camera)
        drop_ratio = dropped / (upstream + dropped) if upstream + dropped else 1.0
        failures = sum(int(item["cap_fail"]) + int(item["enc_fail"]) for item in samples.camera)
        if failures:
            checks.append(Check("FAIL", f"camera capture/encode failures={failures}"))
        elif drop_ratio <= 0.10:
            checks.append(Check("PASS", f"camera drop ratio {drop_ratio * 100:.1f}%"))
        elif drop_ratio <= 0.25:
            checks.append(Check("WARN", f"camera drop ratio {drop_ratio * 100:.1f}%"))
        else:
            checks.append(Check("FAIL", f"camera drop ratio {drop_ratio * 100:.1f}%"))

        max_gap_us = max(int(item["max_gap_us"]) for item in samples.camera)
        if max_gap_us <= 150_000:
            checks.append(Check("PASS", f"maximum frame gap {max_gap_us / 1000:.1f}ms"))
        elif max_gap_us <= 300_000:
            checks.append(Check("WARN", f"maximum frame gap {max_gap_us / 1000:.1f}ms"))
        else:
            checks.append(Check("FAIL", f"maximum frame gap {max_gap_us / 1000:.1f}ms"))

        avg_bitrate = mean(samples.camera, "bitrate")
        avg_payload = mean(samples.camera, "payload_avg")
        max_payload = max(int(item["payload_max"]) for item in samples.camera)
        if avg_bitrate <= 32.0 and avg_payload <= 128.0 and max_payload <= 2048:
            checks.append(
                Check(
                    "FAIL",
                    "H264 output is near-empty: "
                    f"average={avg_payload:.0f}B, max={max_payload}B, bitrate={avg_bitrate:.0f}kbps",
                )
            )

        luma_samples = [
            item
            for item in samples.camera
            if int(item["luma_src_transitions"]) > 0 and int(item["luma_enc_transitions"]) > 0
        ]
        if luma_samples:
            source_delta = mean(luma_samples, "luma_src_delta")
            encoder_delta = mean(luma_samples, "luma_enc_delta")
            if source_delta >= 1.0 and encoder_delta < max(0.2, source_delta * 0.10):
                checks.append(
                    Check(
                        "FAIL",
                        f"PPA output appears frozen: source luma delta={source_delta:.1f}, "
                        f"encoder-input delta={encoder_delta:.1f}",
                    )
                )
            elif encoder_delta >= 1.0 and avg_bitrate <= 32.0:
                checks.append(
                    Check(
                        "FAIL",
                        f"H264 encoder emitted near-empty frames despite changing input "
                        f"(luma delta={encoder_delta:.1f})",
                    )
                )
            else:
                checks.append(
                    Check(
                        "PASS",
                        f"luma probe source/encoder-input delta={source_delta:.1f}/{encoder_delta:.1f}",
                    )
                )

    if samples.tx:
        sent = sum(int(item["sent"]) for item in samples.tx)
        failed = sum(int(item["fail"]) for item in samples.tx)
        tx_ratio = failed / (sent + failed) if sent + failed else 1.0
        avg_tx_fps = mean(samples.tx, "fps")
        if tx_ratio <= 0.05:
            checks.append(Check("PASS", f"TiRTC video TX failure ratio {tx_ratio * 100:.1f}%"))
        elif tx_ratio <= 0.15:
            checks.append(Check("WARN", f"TiRTC video TX failure ratio {tx_ratio * 100:.1f}%"))
        else:
            checks.append(Check("FAIL", f"TiRTC video TX failure ratio {tx_ratio * 100:.1f}%"))
        if avg_tx_fps < 10.0:
            checks.append(Check("FAIL", f"TiRTC average video TX fps {avg_tx_fps:.1f}"))
        elif avg_tx_fps < 15.0:
            checks.append(Check("WARN", f"TiRTC average video TX fps {avg_tx_fps:.1f}"))
        else:
            checks.append(Check("PASS", f"TiRTC average video TX fps {avg_tx_fps:.1f}"))
    elif samples.close_snapshots:
        close = samples.close_snapshots[-1]
        duration_s = max(close["age_ms"] / 1000.0, 0.001)
        close_fps = close["video_frames"] / duration_s
        close_bitrate_kbps = close["video_kb"] * 8.0 / duration_s
        level = "PASS" if close_fps >= 15.0 else "WARN" if close_fps >= 10.0 else "FAIL"
        checks.append(
            Check(
                level,
                "short-session close snapshot: "
                f"video={close['video_frames']} frames, {close_fps:.1f}fps, "
                f"{close_bitrate_kbps:.0f}kbps",
            )
        )
    else:
        checks.append(Check("FAIL", "no periodic TiRTC video TX statistics or close snapshot"))

    active_downlink = [item for item in samples.downlink if float(item["rx_fps"]) > 0.0]
    if active_downlink:
        rx_fps = mean(active_downlink, "rx_fps")
        decoded_fps = mean(active_downlink, "decoded_fps")
        presented_fps = mean(active_downlink, "presented_fps")
        input_drops = sum(int(item["input_drops"]) for item in active_downlink)
        decode_failures = sum(int(item["decode_failures"]) for item in active_downlink)
        restarts = sum(int(item["decoder_restarts"]) for item in active_downlink)
        resets = sum(int(item["resets"]) for item in active_downlink)
        overflows = sum(int(item["overflows"]) for item in active_downlink)
        decode_ratio = decoded_fps / rx_fps if rx_fps else 0.0
        present_ratio = presented_fps / rx_fps if rx_fps else 0.0

        if decode_failures or decode_ratio < 0.60:
            checks.append(
                Check(
                    "FAIL",
                    f"H264 downlink decode {decoded_fps:.1f}/{rx_fps:.1f}fps "
                    f"failures={decode_failures} restarts={restarts} resets={resets} "
                    f"overflows={overflows} input_drops={input_drops}",
                )
            )
        elif decode_ratio < 0.85 or restarts or overflows:
            checks.append(
                Check(
                    "WARN",
                    f"H264 downlink decode {decoded_fps:.1f}/{rx_fps:.1f}fps "
                    f"restarts={restarts} overflows={overflows} input_drops={input_drops}",
                )
            )
        else:
            checks.append(Check("PASS", f"H264 downlink decode {decoded_fps:.1f}/{rx_fps:.1f}fps"))

        if present_ratio < 0.50:
            checks.append(Check("FAIL", f"LCD presentation {presented_fps:.1f}/{rx_fps:.1f}fps"))
        elif present_ratio < 0.75:
            checks.append(Check("WARN", f"LCD presentation {presented_fps:.1f}/{rx_fps:.1f}fps"))
        else:
            checks.append(Check("PASS", f"LCD presentation {presented_fps:.1f}/{rx_fps:.1f}fps"))

    dma_largest = minimum(samples.active_runtime, "dma_largest") or minimum(samples.camera, "dma_largest")
    if dma_largest >= 16 * 1024:
        checks.append(Check("PASS", f"minimum largest DMA block {dma_largest} bytes"))
    elif dma_largest >= 8 * 1024:
        checks.append(Check("WARN", f"minimum largest DMA block {dma_largest} bytes"))
    else:
        checks.append(Check("FAIL", f"minimum largest DMA block {dma_largest} bytes"))

    levels = {check.level for check in checks}
    result = "FAIL" if "FAIL" in levels else "WARN" if "WARN" in levels else "PASS"
    return result, checks


def print_report(path: Path, samples: SampleSet, result: str, checks: list[Check]) -> None:
    print("Media performance report")
    print(f"file: {path}")
    print(f"window: {samples.first_timestamp or '-'} -> {samples.last_timestamp or '-'}")
    print(f"result: {result}")
    if samples.camera:
        last = samples.camera[-1]
        print(
            "camera: "
            f"{int(last['width'])}x{int(last['height'])}@{int(last['target_fps'])} "
            f"avg_fps={mean(samples.camera, 'fps'):.1f} "
            f"avg_bitrate={mean(samples.camera, 'bitrate'):.0f}kbps samples={len(samples.camera)}"
        )
        luma_samples = [
            item
            for item in samples.camera
            if int(item["luma_src_transitions"]) > 0 and int(item["luma_enc_transitions"]) > 0
        ]
        if luma_samples:
            print(
                "luma_probe: "
                f"source_delta={mean(luma_samples, 'luma_src_delta'):.1f} "
                f"encoder_input_delta={mean(luma_samples, 'luma_enc_delta'):.1f}"
            )
    if samples.tx:
        print(
            "tirtc_tx: "
            f"avg_fps={mean(samples.tx, 'fps'):.1f} "
            f"avg_bitrate={mean(samples.tx, 'bitrate'):.0f}kbps samples={len(samples.tx)}"
        )
    active_downlink = [item for item in samples.downlink if float(item["rx_fps"]) > 0.0]
    if active_downlink:
        print(
            "downlink: "
            f"rx={mean(active_downlink, 'rx_fps'):.1f}fps "
            f"decoded={mean(active_downlink, 'decoded_fps'):.1f}fps "
            f"presented={mean(active_downlink, 'presented_fps'):.1f}fps "
            f"samples={len(active_downlink)}"
        )
    if samples.close_snapshots:
        close = samples.close_snapshots[-1]
        duration_s = max(close["age_ms"] / 1000.0, 0.001)
        print(
            "close_snapshot: "
            f"age={duration_s:.1f}s video={close['video_frames']} "
            f"audio={close['audio_frames']} attempts={close['attempts']} "
            f"failures={close['failures']} "
            f"sdk_buf={close['send_buffer_used']}/{close['send_buffer_limit']}"
        )
    print("checks:")
    for check in checks:
        print(f"  [{check.level}] {check.message}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path, help="UTF-8 serial log file")
    args = parser.parse_args()

    if not args.log.is_file():
        parser.error(f"log file not found: {args.log}")
    samples = parse_log(args.log)
    result, checks = evaluate(samples)
    print_report(args.log, samples, result, checks)
    return {"PASS": 0, "WARN": 1, "FAIL": 2}[result]


if __name__ == "__main__":
    sys.exit(main())
