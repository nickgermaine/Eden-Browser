import json
from pathlib import Path
import sys
import tempfile
import unittest
from types import SimpleNamespace


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts/bench/wpe"))

import validate_intrinsic_width


def valid_result(offset=0):
    names = ["simple-min-content-hit"]
    names.extend(
        f"{white_space}-edge-spaces-{layout}"
        for white_space, layout in zip(
            ("normal", "pre-line", "pre-wrap", "break-spaces", "pre", "nowrap"),
            ("direct", "flex", "grid", "direct", "flex", "grid"),
            strict=True,
        )
    )
    features = (
        "spacing-positive-letter",
        "spacing-negative-letter",
        "spacing-positive-word",
        "spacing-negative-word",
        "indent-right-align",
        "indent-center-align",
        "overflow-wrap-anywhere",
        "word-break-all",
        "word-break-keep-all",
        "hyphens-auto",
        "nested-mark",
        "multiple-text-nodes",
        "first-line-style",
        "first-line-preserved-spaces",
        "soft-hyphen-at-end",
        "zwsp-breakpoint",
        "nbsp-run",
        "mixed-script-bidi",
    )
    names.extend(feature + "-direct" for feature in features)
    names.extend(
        (
            "pre-line-leading-consecutive-breaks-direct",
            "pre-wrap-leading-consecutive-breaks-direct",
            "br-start-flex",
            "br-end-grid",
            "br-middle-direct",
            "single-character-min-content-direct",
            "only-whitespace-collapse-direct",
            "only-whitespace-preserved-flex",
            "single-full-mark-grid",
            "overflow-wrap-break-word-direct",
        )
    )
    names.extend(f"mutation-{state}" for state in ("initial", "text", "width-font", "second-width-font"))
    names.extend(f"random-{index:02d}" for index in range(12))
    filler_count = validate_intrinsic_width.EXPECTED_GEOMETRY_CASES - len(names)
    names.extend(f"filler-{index:02d}" for index in range(filler_count))
    geometry = []
    white_spaces = ("normal", "pre-line", "pre-wrap", "break-spaces", "pre", "nowrap")
    for index, name in enumerate(names):
        geometry.append(
            {
                "name": name,
                "layout": "direct",
                "whiteSpace": white_spaces[index % len(white_spaces)],
                "font": "17px FixtureVariable",
                "host": [float(offset), 0, 216, 20],
                "target": [0, 0, 63, 20],
                "peer": None,
                "scroll": [63, 20],
                "hardBreaks": [],
                "textRanges": [{"text": "alpha beta", "rects": [[0, 0, 63, 20]]}],
            }
        )
    return {
        "probe": "intrinsic-width",
        "measurements": {"geometry": geometry},
        "visibility": "visible",
        "viewport": [1280, 900, 1],
    }


def counter_line(path, candidate, eligible):
    return (
        "Eden intrinsic width probe: event=1 path="
        + path
        + " candidate="
        + str(candidate)
        + " eligible="
        + str(eligible)
        + " lines=2 start_ns=100 duration_ns=20 eligibility_ns=5\n"
    )


class IntrinsicWidthValidationTest(unittest.TestCase):
    def make_build(self, root):
        build = root / "build"
        build.mkdir()
        (build / "eden-wpe-bench").touch()
        return build

    def fake_runner(self, behavior="success"):
        calls = []

        def invoke(command, env, stdout, stderr, check, timeout):
            self.assertEqual(env["EDEN_WPE_PREWARM"], "0")
            directory = Path(command[command.index("--training-directory") + 1])
            self.assertTrue(directory.is_dir())
            self.assertEqual(env["LLVM_PROFILE_FILE"], str(directory / "%p-%m.profraw"))
            calls.append((command, env, timeout))
            result_path = Path(command[command.index("--output") + 1])
            label = command[command.index("--label") + 1]
            mode = env["WEBKIT_EDEN_INTRINSIC_WIDTH"]
            stdout.write(label + " completed\n")
            if behavior == "nonzero":
                result_path.write_text(json.dumps(valid_result()))
                return SimpleNamespace(returncode=7)
            if behavior == "empty":
                result_path.write_bytes(b"")
                return SimpleNamespace(returncode=0)
            if behavior == "malformed":
                result_path.write_text("{invalid")
                return SimpleNamespace(returncode=0)
            result = valid_result(offset=1 if behavior == "mismatch" and mode == "1" else 0)
            library = Path(command[command.index("--build") + 1]) / "fixture-library.so"
            library.write_text("changed" if behavior == "runtime-mismatch" and mode == "1" else "original")
            result["runtime"] = {"library": str(library)}
            result_path.write_text(json.dumps(result))
            if mode == "1":
                stdout.write(counter_line("cached-text", 1, 1))
            else:
                stdout.write(counter_line("text-only", 1, 1))
            return SimpleNamespace(returncode=0)

        return invoke, calls

    def test_same_binary_is_run_at_both_scales_with_verified_geometry_and_hits(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build = self.make_build(root)
            output = root / "results"
            invoke, calls = self.fake_runner()
            summary = validate_intrinsic_width.run_validation(build, output, runner=invoke)
            self.assertTrue(summary["passed"])
            self.assertEqual([call[0][call[0].index("--scale") + 1] for call in calls], ["1", "1", "2", "2"])
            self.assertEqual(
                [call[1]["WEBKIT_EDEN_INTRINSIC_WIDTH"] for call in calls], ["0", "1", "0", "1"]
            )
            verification_flags = [
                call[1].get("WEBKIT_EDEN_VERIFY_INTRINSIC_WIDTH") for call in calls
            ]
            self.assertEqual(verification_flags, [None, "1", None, "1"])
            self.assertTrue(all(call[1]["WEBKIT_EDEN_PATCH_COUNTERS"] == "1" for call in calls))
            self.assertTrue(all((output / f"intrinsic-{mode}-scale-{scale}.{suffix}").is_file()
                                for scale in (1, 2)
                                for mode in ("off", "on")
                                for suffix in ("json", "log")))
            self.assertEqual([item["enabledVerifiedHits"] for item in summary["scales"]], [1, 1])

    def test_empty_or_malformed_result_cannot_pass_and_is_preserved(self):
        for behavior, preserved in (("empty", b""), ("malformed", b"{invalid")):
            with self.subTest(behavior=behavior), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                build = self.make_build(root)
                output = root / "results"
                invoke, _ = self.fake_runner(behavior)
                with self.assertRaises(ValueError):
                    validate_intrinsic_width.run_validation(build, output, runner=invoke)
                self.assertEqual((output / "intrinsic-off-scale-1.json").read_bytes(), preserved)
                self.assertTrue((output / "intrinsic-off-scale-1.log").is_file())
                self.assertFalse(json.loads((output / "summary.json").read_text())["passed"])

    def test_nonzero_process_exit_cannot_pass_and_keeps_raw_artifacts(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build = self.make_build(root)
            output = root / "results"
            invoke, _ = self.fake_runner("nonzero")
            with self.assertRaisesRegex(RuntimeError, "status 7"):
                validate_intrinsic_width.run_validation(build, output, runner=invoke)
            self.assertTrue((output / "intrinsic-off-scale-1.json").is_file())
            self.assertIn("completed", (output / "intrinsic-off-scale-1.log").read_text())
            summary = json.loads((output / "summary.json").read_text())
            self.assertFalse(summary["passed"])
            self.assertEqual(summary["runs"][0]["returnCode"], 7)

    def test_geometry_mismatch_cannot_pass(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build = self.make_build(root)
            output = root / "results"
            invoke, _ = self.fake_runner("mismatch")
            with self.assertRaisesRegex(AssertionError, r"host\[0\]"):
                validate_intrinsic_width.run_validation(build, output, runner=invoke)
            self.assertEqual(len(list(output.glob("intrinsic-*-scale-1.json"))), 2)
            self.assertFalse(json.loads((output / "summary.json").read_text())["passed"])

    def test_changed_library_cannot_pass_despite_matching_geometry(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build = self.make_build(root)
            output = root / "results"
            invoke, _ = self.fake_runner("runtime-mismatch")
            with self.assertRaisesRegex(AssertionError, "runtime identity.sha256"):
                validate_intrinsic_width.run_validation(build, output, runner=invoke)
            self.assertFalse(json.loads((output / "summary.json").read_text())["passed"])

    def test_counter_log_rejects_missing_hits_malformed_records_and_truncation(self):
        enabled = validate_intrinsic_width.parse_counter_log(counter_line("cached-text", 1, 1), "enabled")
        self.assertEqual(validate_intrinsic_width.verify_counter_hits(enabled, "enabled"), 1)
        with self.assertRaisesRegex(ValueError, "no verified"):
            validate_intrinsic_width.verify_counter_hits([], "enabled")
        with self.assertRaisesRegex(ValueError, "disabled control unexpectedly"):
            validate_intrinsic_width.verify_no_cached_hits(enabled, "control")
        with self.assertRaisesRegex(ValueError, "malformed"):
            validate_intrinsic_width.parse_counter_log("Eden intrinsic width probe: event=bad\n", "run")
        with self.assertRaisesRegex(ValueError, "truncated"):
            validate_intrinsic_width.parse_counter_log(
                "Eden intrinsic width probe: truncated=1 event_limit=262144\n", "run"
            )


if __name__ == "__main__":
    unittest.main()
