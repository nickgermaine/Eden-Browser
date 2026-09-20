import importlib.util
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
module_spec = importlib.util.spec_from_file_location(
    "analyze_profile", ROOT / "scripts/bench/wpe/analyze_profile.py"
)
analyzer = importlib.util.module_from_spec(module_spec)
module_spec.loader.exec_module(analyzer)


class RenderingProfileTest(unittest.TestCase):
    def test_missing_stacks_keep_their_weight_and_worker_samples_are_excluded(self):
        samples = """WPEWebProcess 10/10 1.001000000:
    1234 WebCore::visit(WTF::Function<void (int)> const&) (/engine.so)
    2345 WebCore::layout() (/engine.so)
WPEWebProcess 10/11 1.002000000:
    1234 Worker::work() (/engine.so)
WPEWebProcess 10/10 1.008000000:
OtherProcess 20/20 1.009000000:
    3456 Other::work() (/engine.so)
"""

        class Capture:
            stdout = io.StringIO(samples)

            def wait(self):
                return 0

        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            (directory / "run.json").write_text(
                json.dumps(
                    {
                        "timeOrigin": 1000,
                        "clockCorrelation": {
                            "monotonicMilliseconds": 0,
                            "wallMilliseconds": 0,
                        },
                        "timings": [
                            {"name": "fixture-layout", "startTime": 0, "duration": 10}
                        ],
                    }
                )
            )
            with patch.object(
                sys, "argv", ["analyze_profile", str(directory), "--perf", "/tmp/perf"]
            ), patch.object(
                analyzer.subprocess, "Popen", return_value=Capture()
            ), patch(
                "sys.stdout", new_callable=io.StringIO
            ):
                analyzer.main()
            result = json.loads((directory / "functions.json").read_text())
        self.assertEqual(result["all-main"]["samples"], 2)
        self.assertEqual(result["all-main"]["meanDepth"], 1)
        self.assertEqual(result["fixture-layout"]["self"]["<no-callchain>"], 1)
        self.assertEqual(
            result["fixture-layout"]["inclusive"][
                "WebCore::visit(WTF::Function<void (int)> const&)"
            ],
            1,
        )
        self.assertNotIn("Worker::work()", result["all-main"]["self"])

    def test_jit_addresses_resolve_only_after_load_and_follow_reuse(self):
        header = struct.pack("<IIIIIIQQ", 0x4A695444, 1, 40, 62, 0, 2, 0, 0)

        def record(stamp, name):
            name = name.encode() + b"\0"
            code = b"\x90" * 8
            return (
                struct.pack(
                    "<IIQIIQQQQ",
                    0,
                    56 + len(name) + len(code),
                    stamp,
                    2,
                    2,
                    0x1000,
                    0x1000,
                    len(code),
                    stamp,
                )
                + name
                + code
            )

        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "jit.dump"
            path.write_bytes(header + record(100, "first") + record(200, "replacement"))
            symbols = analyzer.jit_symbols(path)
            self.assertEqual(
                analyzer.resolve_jit(symbols, 0x1002, 99, "[unknown]"), "[unknown]"
            )
            self.assertEqual(
                analyzer.resolve_jit(symbols, 0x1002, 150, "[unknown]"), "JIT::first"
            )
            self.assertEqual(
                analyzer.resolve_jit(symbols, 0x1002, 250, "[unknown]"),
                "JIT::replacement",
            )
            self.assertEqual(
                analyzer.resolve_jit(symbols, 0x1008, 250, "[unknown]"), "[unknown]"
            )
            self.assertEqual(
                analyzer.resolve_jit(symbols, 0x1002, 250, "Native::function()"),
                "Native::function()",
            )
            path.write_bytes(path.read_bytes()[:-1])
            with self.assertRaises(ValueError):
                analyzer.jit_symbols(path)


if __name__ == "__main__":
    unittest.main()
