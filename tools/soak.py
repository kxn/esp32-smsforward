#!/usr/bin/env python3
"""Opt-in, read-only heap/uptime soak logger via the localhost serial bridge.

The default run samples every 10 minutes for 30 days (4320 samples). It sends
only the console commands ``info`` and ``net``; it never resets the device.
"""
from __future__ import annotations

import argparse
import json
import re
import socket
import time
from datetime import datetime, timezone
from pathlib import Path


HASH_RE = re.compile(r"^app sha\s*:\s*([0-9a-fA-F]{16,64})\s*$", re.M)
UPTIME_RE = re.compile(r"^uptime\s*:\s*(\d+)\s*s\s*$", re.M)
HEAP_INT_RE = re.compile(r"^heap int\s*:\s*(\d+)\s+free\s*/\s*(\d+)\s+total\s*$", re.M)
HEAP_PSRAM_RE = re.compile(r"^heap psram\s*:\s*(\d+)\s+free\s*$", re.M)
TIME_RE = re.compile(r"^time\s*:\s*(synced|not synced)\s*$", re.M)
WS_RE = re.compile(r"^feishu\s*:\s*[^\r\n]*?\bws=(online|offline)\s*$", re.M)


def parse_snapshot(info: str, net: str) -> dict:
    """Extract the allowed status fields. Raw console text is never returned."""
    def one(pattern, text):
        match = pattern.search(text)
        return match.groups() if match else None

    app_hash = one(HASH_RE, info)
    uptime = one(UPTIME_RE, info)
    heap_int = one(HEAP_INT_RE, info)
    heap_psram = one(HEAP_PSRAM_RE, info)
    synced = one(TIME_RE, net)
    ws = one(WS_RE, net)
    return {
        "image_hash": app_hash[0].lower() if app_hash else None,
        "uptime_s": int(uptime[0]) if uptime else None,
        "internal_free": int(heap_int[0]) if heap_int else None,
        "internal_total": int(heap_int[1]) if heap_int else None,
        "psram_free": int(heap_psram[0]) if heap_psram else None,
        "time_sync": synced[0] if synced else None,
        "ws": ws[0] if ws else None,
    }


class Bridge:
    def __init__(self, host: str, port: int, timeout: float):
        self.host, self.port, self.timeout = host, port, timeout
        self.sock = None
        self.buffer = bytearray()

    def connect(self):
        self.sock = socket.create_connection((self.host, self.port), self.timeout)
        self.sock.settimeout(0.25)
        self.buffer.clear()

    def close(self):
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass
        self.sock = None

    def command(self, command: str, begin: bytes, end: bytes) -> str:
        assert command in ("info", "net")
        if self.sock is None:
            self.connect()
        self.sock.sendall(command.encode("ascii") + b"\r\n")
        deadline = time.monotonic() + self.timeout
        active = False
        lines = []
        while time.monotonic() < deadline:
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                continue
            if not chunk:
                raise OSError("bridge disconnected")
            self.buffer.extend(chunk)
            while b"\n" in self.buffer:
                raw, _, rest = self.buffer.partition(b"\n")
                self.buffer[:] = rest
                line = raw.decode("ascii", "ignore").strip("\r\x00 ")
                encoded = line.encode("ascii", "ignore")
                if begin in encoded:
                    active = True
                    lines = [line]
                elif active:
                    lines.append(line)
                    if end in encoded:
                        return "\n".join(lines)
        raise TimeoutError("console response incomplete")

    def snapshot(self) -> dict:
        # A timed-out partial response can leave command framing ambiguous.
        # Reconnect before the next sample rather than risk correlating stale data.
        try:
            info = self.command("info", b"--- info ---", b"------------")
            net = self.command("net", b"--- net ---", b"-----------")
            return parse_snapshot(info, net)
        finally:
            # The bridge broadcasts live logs. Release the connection between
            # samples so the next read cannot pick up an older console reply.
            self.close()


def log_sample(path: Path, result: dict, previous: dict | None, now: datetime | None = None):
    markers = []
    if result is None:
        payload = {key: None for key in (
            "image_hash", "uptime_s", "internal_free", "internal_total",
            "psram_free", "time_sync", "ws")}
        markers.append("unavailable")
    else:
        payload = result.copy()
        if previous and previous.get("uptime_s") is not None and payload["uptime_s"] is not None:
            if payload["uptime_s"] < previous["uptime_s"]:
                markers.append("reset")
        if previous and previous.get("image_hash") and payload["image_hash"] and previous["image_hash"] != payload["image_hash"]:
            markers.append("image_change")
    payload["timestamp"] = (now or datetime.now(timezone.utc)).isoformat()
    payload["markers"] = markers
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8", newline="\n") as out:
        out.write(json.dumps(payload, separators=(",", ":")) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1", help="localhost serial bridge host")
    parser.add_argument("--port", type=int, default=5555)
    parser.add_argument("--timeout", type=float, default=8.0)
    parser.add_argument("--samples", type=int, default=4320, help="number of samples (default: 30 days at 10 minutes)")
    parser.add_argument("--interval", type=float, default=600.0, help="seconds between samples")
    parser.add_argument("--output", type=Path, default=Path("soak.jsonl"), help="local JSONL output path")
    args = parser.parse_args()
    if args.samples < 1 or args.interval < 0 or args.timeout <= 0:
        parser.error("samples must be >=1, interval >=0, timeout >0")

    bridge = Bridge(args.host, args.port, args.timeout)
    previous = None
    try:
        for index in range(args.samples):
            try:
                result = bridge.snapshot()
            except (OSError, TimeoutError):
                result = None
            log_sample(args.output, result, previous)
            if result is not None:
                previous = result
            if index + 1 < args.samples:
                time.sleep(args.interval)
    except KeyboardInterrupt:
        return 130
    finally:
        bridge.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
