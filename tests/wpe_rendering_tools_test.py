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
    def test_leaf_samples_survive_failed_unwinding_and_resolve_jit(self):
        samples = """WPEWebProcess 10/10 1.000000000: PERF_RECORD_MMAP2 10/10: native mapping
JSC PerfLog 10/12 1.000000000: PERF_RECORD_MMAP2 10/12: jit mapping /capture/jit-4-2.dump
WPEWebProcess 10/10 1.001000000:     1234 WebCore::visit(WTF::Function<void (int)> const&) (/engine.so)
WPEWebProcess 10/10 1.002000000:     4321 [unknown] (/tmp/perf-10.map)
WPEWebProcess 10/11 1.003000000:     1234 Worker::work() (/engine.so)
"""

        class Capture:
            stdout = io.StringIO(samples)

            def wait(self):
                return 0

        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            (directory / "run.json").write_text("{}")
            (directory / "jit-4-2.dump").touch()
            with patch.object(
                sys, "argv", ["analyze_profile", str(directory), "--perf", "/tmp/perf", "--leaf-only"]
            ), patch.object(
                analyzer.subprocess, "Popen", return_value=Capture()
            ) as process, patch.object(
                analyzer, "jit_symbols", return_value={4: [(1000000000, 0x4300, 0x4400, "JIT::update")]}
            ), patch("sys.stdout", new_callable=io.StringIO):
                analyzer.main()
            self.assertIn("--hide-call-graph", process.call_args.args[0])
            result = json.loads((directory / "functions-leaf.json").read_text())["all-main"]
            self.assertEqual(result["samples"], 2)
            self.assertEqual(result["self"], {
                "WebCore::visit(WTF::Function<void (int)> const&)": 1,
                "JIT::update": 1,
            })
            self.assertFalse(result["callchains"])
            self.assertIsNone(result["meanDepth"])
            self.assertEqual(result["inclusive"], {})

    def test_first_and_subsequent_iterations_keep_distinct_phase_samples(self):
        samples = """WPEWebProcess 10/10 1.001000000:
    1234 Cold::work() (/engine.so)
WPEWebProcess 10/10 1.008000000:
    2345 Warm::work() (/engine.so)
"""

        class Capture:
            def __init__(self):
                self.stdout = io.StringIO(samples)

            def wait(self):
                return 0

        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            (directory / "run.json").write_text(json.dumps({
                "timeOrigin": 1000,
                "clockCorrelation": {"monotonicMilliseconds": 0, "wallMilliseconds": 0},
                "timings": [
                    {"name": "fixture-sync", "startTime": 6, "duration": 3},
                    {"name": "fixture-sync", "startTime": 0, "duration": 3},
                ],
            }))
            for iteration, symbol in (("first", "Cold::work()"), ("subsequent", "Warm::work()")):
                with self.subTest(iteration=iteration), patch.object(
                    sys, "argv", ["analyze_profile", str(directory), "--perf", "/tmp/perf", "--iteration", iteration]
                ), patch.object(analyzer.subprocess, "Popen", side_effect=lambda *args, **kwargs: Capture()), patch(
                    "sys.stdout", new_callable=io.StringIO
                ):
                    analyzer.main()
                result = json.loads((directory / f"functions-{iteration}.json").read_text())
                self.assertEqual(result["all-main"]["samples"], 2)
                self.assertEqual(result["fixture-sync"]["samples"], 1)
                self.assertEqual(result["fixture-sync"]["self"], {symbol: 1})

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
