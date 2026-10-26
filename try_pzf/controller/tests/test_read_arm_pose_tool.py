"""Run the real PS collector against a loopback fixture, never a device.

Tests its HTTP/CSV path, not FreeRTOS, the C cache, or physical position reads.
"""

import csv
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading
import unittest


CONTROLLER = Path(__file__).resolve().parents[1]
POWERSHELL = Path(os.environ.get("SystemRoot", "C:/Windows")) / "System32/WindowsPowerShell/v1.0/powershell.exe"


@unittest.skipUnless(os.name == "nt" and POWERSHELL.is_file(), "requires Windows PowerShell 5.1")
class ReadArmPoseCollectorTests(unittest.TestCase):
    def collect(self, status):
        requests = []

        class Fixture(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                requests.append(("GET", self.path))
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                if status == 302:
                    self.send_header("Location", "/must-not-follow")
                self.end_headers()
                axes = []
                for axis, bus_id in enumerate((4, 1, 2, 3, 5)):
                    has_value = axis == 0
                    axes.append({
                        "axis": axis, "bus_id": bus_id,
                        "protocol": "em3" if axis == 0 else "lx",
                        "raw": 0 if has_value else None,
                        "valid": has_value, "stale": not has_value,
                        "age_ms": 0 if has_value else None,
                        "last_attempt_at_ms": 1000,
                        "last_ok_at_ms": 1000 if has_value else None,
                        "attempts": 1, "successes": int(has_value),
                        "failures": int(not has_value),
                    })
                self.wfile.write(json.dumps({
                    "ok": True, "read_only": True,
                    "source": "servo-feedback-cache", "sampling_mode": "idle_only",
                    "sampling_started": True, "motion_busy": False,
                    "captured_at_ms": 1000, "stale_after_ms": 10000, "axes": axes,
                }).encode("utf-8"))

        server = ThreadingHTTPServer(("127.0.0.1", 0), Fixture)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            with tempfile.TemporaryDirectory(prefix="arm-readonly-fixture-") as output:
                run = subprocess.run([
                    str(POWERSHELL), "-NoProfile", "-ExecutionPolicy", "Bypass",
                    "-File", str(CONTROLLER / "tools/read_arm_pose.ps1"),
                    "-ControllerUrl", f"http://127.0.0.1:{server.server_port}",
                    "-Samples", "2", "-IntervalSeconds", "0.1",
                    "-OutputDirectory", output,
                ], capture_output=True, timeout=30)
                self.assertEqual(run.returncode, 0, run.stdout.decode(errors="replace") + run.stderr.decode(errors="replace"))
                sample_paths = list(Path(output).glob("*/arm_pose_samples.csv"))
                summary_paths = list(Path(output).glob("*/arm_pose_summary.csv"))
                self.assertEqual(len(sample_paths), 1)
                self.assertEqual(len(summary_paths), 1)
                with sample_paths[0].open(encoding="utf-8-sig", newline="") as stream:
                    samples = list(csv.DictReader(stream))
                with summary_paths[0].open(encoding="utf-8-sig", newline="") as stream:
                    summary = list(csv.DictReader(stream))
                return samples, summary, requests
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)

    def test_real_http_preserves_null_zero_and_does_not_count_repeated_cache(self):
        samples, summary, requests = self.collect(200)
        self.assertEqual(requests, [("GET", "/api/arm/pose")] * 2)
        self.assertEqual(len(samples), 10)
        self.assertEqual(len(summary), 5)
        self.assertEqual(summary[0]["latest_raw"], "0")
        for axis in summary[1:]:
            self.assertEqual(axis["latest_raw"], "")
            self.assertEqual(axis["latest_last_ok_at_ms"], "")
        for axis in summary:
            self.assertEqual(axis["new_completed_attempts"], "0")
            self.assertEqual(axis["repeated_cache_snapshots"], "1")
            self.assertEqual(axis["reference_raw"], "")
            self.assertEqual(axis["mechanical_safe_min"], "")
            self.assertEqual(axis["mechanical_safe_max"], "")
            self.assertEqual(axis["stability_assessment"], "not_assessed")

    def test_http_failures_are_recorded_without_retry_or_redirect(self):
        for status in (302, 503):
            with self.subTest(status=status):
                samples, summary, requests = self.collect(status)
                self.assertEqual(requests, [("GET", "/api/arm/pose")] * 2)
                self.assertEqual(len(samples), 10)
                for row in samples:
                    self.assertEqual(row["request_ok"], "False")
                    self.assertEqual(row["raw"], "")
                    self.assertEqual(row["http_status"], str(status))
                for axis in summary:
                    self.assertEqual(axis["response_snapshots"], "0")
                    self.assertEqual(axis["latest_raw"], "")


if __name__ == "__main__":
    unittest.main()
