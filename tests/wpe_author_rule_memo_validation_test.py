import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts/bench/wpe"))

import validate_author_rule_memo as validation


class AuthorRuleMemoValidationTest(unittest.TestCase):
    def test_counter_evidence_rejects_no_hits_disabled_hits_and_mismatches(self):
        line = "Eden author rule memo: lookups 12 hits 1 misses 3 bypasses 8 reused-rules 3"
        self.assertEqual(validation.validate_counters(line, True, "enabled")[0]["hits"], 1)
        self.assertEqual(validation.validate_counters("", False, "disabled"), [])
        for text, enabled, reason in (
            ("", True, "no memo hits"),
            (line, False, "disabled run"),
            (line.replace("hits 1", "hits 9"), True, "inconsistent"),
            ("Eden author rule memo: mismatch for <li>", True, "failed memo counter"),
        ):
            with self.subTest(reason=reason), self.assertRaisesRegex(ValueError, reason):
                validation.validate_counters(text, enabled, "run")

    def test_failed_process_preserves_results_and_does_not_pass(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build = root / "build"
            build.mkdir()
            (build / "eden-wpe-bench").touch()
            output = root / "results"

            def invoke(command, **kwargs):
                self.assertEqual(kwargs["env"]["EDEN_WPE_PREWARM"], "0")
                directory = Path(command[command.index("--training-directory") + 1])
                self.assertTrue(directory.is_dir())
                self.assertEqual(kwargs["env"]["LLVM_PROFILE_FILE"], str(directory / "%p-%m.profraw"))
                Path(command[command.index("--output") + 1]).write_text("partial output")
                kwargs["stdout"].write("renderer exited\n")
                return SimpleNamespace(returncode=6)

            with self.assertRaisesRegex(RuntimeError, "process exited 6"):
                validation.run_validation(build, output, runner=invoke)
            summary = json.loads((output / "summary.json").read_text())
            self.assertFalse(summary["passed"])
            self.assertEqual((output / "author-memo-off-scale-1.json").read_text(), "partial output")
            self.assertIn("renderer exited", (output / "author-memo-off-scale-1.log").read_text())

    def test_truncated_or_nonfixture_json_is_not_a_passing_run(self):
        for value in ({}, {"probe": "author-rule-memo", "visibility": "hidden"},
                      {"probe": "author-rule-memo", "visibility": "visible", "viewport": [1280, 900, 1],
                       "measurements": {"assertions": 0, "failures": [], "records": []}}):
            with self.subTest(value=value), self.assertRaises(ValueError):
                validation.validate_result(value, "run")


if __name__ == "__main__":
    unittest.main()
