import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "third_party/wpe/source/wpewebkit-2.53.92-combined2/Source/WebCore/page"
sys.path.insert(0, str(ROOT / "scripts"))
sys.dont_write_bytecode = True

from wpe.adopted.opportunistic_yield import PATCHES, scheduler_header_patch, scheduler_patch
from wpe.patch_support import without_comments


def extract_definition(source, signature):
    if source.count(signature) != 1:
        raise RuntimeError("Pinned Scheduler source did not contain one definition: " + signature)
    start = source.index(signature)
    opening = source.index("\n{\n", start) + 1
    depth = 0
    quote = ""
    escaped = False
    for position in range(opening, len(source)):
        character = source[position]
        if quote:
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == quote:
                quote = ""
        elif character in "\"'":
            quote = character
        elif character == "{":
            depth += 1
        elif character == "}":
            depth -= 1
            if not depth:
                return source[start : position + 1]
    raise RuntimeError("Pinned Scheduler definition had unbalanced braces: " + signature)


class OpportunisticYieldTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary_directory = tempfile.TemporaryDirectory()
        temporary_path = Path(cls.temporary_directory.name)
        harness_path = ROOT / "tests/wpe/opportunistic_yield_harness.cpp"
        header_path = SOURCE / "OpportunisticTaskScheduler.h"
        cpp_path = SOURCE / "OpportunisticTaskScheduler.cpp"
        if not harness_path.is_file() or not header_path.is_file() or not cpp_path.is_file():
            raise RuntimeError("The Scheduler adapter fixture or pinned source is missing")

        patched_header = scheduler_header_patch(header_path.read_text())
        patched_cpp = scheduler_patch(cpp_path.read_text())
        cls.assert_header_contract(patched_header)
        signatures = (
            "static bool edenOpportunisticYieldEnabled()",
            "OpportunisticTaskScheduler::OpportunisticTaskScheduler(Page& page)",
            "OpportunisticTaskScheduler::~OpportunisticTaskScheduler()",
            "void OpportunisticTaskScheduler::cancelPendingTask()",
            "void OpportunisticTaskScheduler::rescheduleIfNeeded(MonotonicTime deadline)",
            "void OpportunisticTaskScheduler::runLoopObserverFired()",
        )
        definitions = [extract_definition(patched_cpp, signature) for signature in signatures]
        for definition in definitions:
            if without_comments(definition) != definition:
                raise RuntimeError("Generated Scheduler definition contains a code comment")

        template = harness_path.read_text()
        if without_comments(template) != template:
            raise RuntimeError("The Scheduler adapter fixture contains a code comment")
        marker = "GENERATED_SCHEDULER_DEFINITIONS_TOKEN"
        if template.count(marker) != 1:
            raise RuntimeError("The Scheduler adapter fixture needs one generated-definition marker")
        generated_source = template.replace(marker, "\n\n".join(definitions))
        cls.source_path = temporary_path / "opportunistic_yield_generated.cpp"
        cls.binary_path = temporary_path / "opportunistic_yield_harness"
        cls.source_path.write_text(generated_source)

        compiler = shlex.split(os.environ.get("CXX", "c++"))
        glib_flags = shlex.split(
            subprocess.check_output(["pkg-config", "--cflags", "--libs", "glib-2.0"], text=True)
        )
        command = [
            "taskset",
            "--cpu-list",
            "0-7,16-23",
            *compiler,
            "-std=c++20",
            "-O0",
            "-g",
            "-pthread",
            str(cls.source_path),
            "-o",
            str(cls.binary_path),
            *glib_flags,
        ]
        result = subprocess.run(command, capture_output=True, text=True, timeout=90)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)

    @classmethod
    def assert_header_contract(cls, patched_header):
        if "void cancelPendingTask();" not in patched_header:
            raise RuntimeError("Generated Scheduler header omitted the cancellation method")
        if "m_retryTimer" not in patched_header:
            raise RuntimeError("Generated Scheduler header omitted the retry timer")
        if set(PATCHES) != {
            "Source/WebCore/page/OpportunisticTaskScheduler.h",
            "Source/WebCore/page/OpportunisticTaskScheduler.cpp",
        }:
            raise RuntimeError("Scheduler patch paths changed unexpectedly")

    @classmethod
    def tearDownClass(cls):
        cls.temporary_directory.cleanup()

    def run_policy_process(self, mode, setting):
        environment = {**os.environ, "WEBKIT_EDEN_OPPORTUNISTIC_YIELD": setting}
        result = subprocess.run(
            [str(self.binary_path), mode],
            env=environment,
            capture_output=True,
            text=True,
            timeout=5,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(mode + " scheduler behavior passed", result.stdout)

    def test_enabled_scheduler_uses_glib_priority_and_bounded_retries(self):
        self.run_policy_process("enabled", "1")

    def test_disabled_scheduler_preserves_one_shot_policy(self):
        self.run_policy_process("disabled", "0")


if __name__ == "__main__":
    unittest.main()
