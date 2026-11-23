"""Deterministic parser/logger checks for the standalone soak recorder."""
import json
import tempfile
import unittest
from datetime import datetime, timezone
from pathlib import Path

from soak import log_sample, parse_snapshot


INFO = """--- info ---
app sha  : 0123456789abcdef
heap int : 42000 free / 180000 total
heap psram: 123456 free
uptime   : 9876 s
------------"""
NET = """--- net ---
time   : synced
feishu : configured, ws=online
-----------"""


class SoakTests(unittest.TestCase):
    def test_parser_extracts_only_allowlisted_fields(self):
        self.assertEqual(parse_snapshot(INFO, NET), {
            "image_hash": "0123456789abcdef", "uptime_s": 9876,
            "internal_free": 42000, "internal_total": 180000,
            "psram_free": 123456, "time_sync": "synced", "ws": "online",
        })

    def test_markers_and_jsonl_allowlist(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "samples.jsonl"
            at = datetime(2026, 1, 2, tzinfo=timezone.utc)
            first = parse_snapshot(INFO, NET)
            log_sample(path, first, None, at)
            changed = dict(first, uptime_s=5, image_hash="fedcba9876543210")
            log_sample(path, changed, first, at)
            log_sample(path, None, changed, at)
            rows = [json.loads(line) for line in path.read_text().splitlines()]
            self.assertEqual(rows[1]["markers"], ["reset", "image_change"])
            self.assertEqual(rows[2]["markers"], ["unavailable"])
            self.assertEqual(set(rows[0]), {
                "timestamp", "image_hash", "uptime_s", "internal_free",
                "internal_total", "psram_free", "time_sync", "ws", "markers",
            })
            self.assertEqual(rows[0]["timestamp"], at.isoformat())

    def test_missing_fields_become_null(self):
        parsed = parse_snapshot("--- info ---", "--- net ---")
        self.assertTrue(all(value is None for value in parsed.values()))


if __name__ == "__main__":
    unittest.main()
