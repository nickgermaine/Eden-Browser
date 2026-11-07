import os
from pathlib import Path
import subprocess
import sys
import tarfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from wpe.adopted.text_cache_keys import PATCHES
from wpe.adopted_patches import load, render
from wpe.patch_support import without_comments
from wpe.performance_patches import names, resolve_patches


class TextCacheKeysPatchTest(unittest.TestCase):
    def test_registered_as_default_with_test_payload_digest(self):
        self.assertIn("text-cache-key-length", names(resolve_patches(False)))
        self.assertIn("text-cache-key-length", names(resolve_patches(True)))
        patches, _, sources = load(["text_cache_keys"])
        self.assertEqual(set(patches), set(PATCHES))
        self.assertTrue(any(path.name == "text_cache_keys_tests.inc" for path in sources))

    def test_pinned_source_and_existing_simple_text_compose(self):
        archive = ROOT / "third_party/wpe/packages/wpewebkit-2.53.92.tar.xz"
        if not archive.is_file():
            self.skipTest("The pinned WebKit source archive is required")
        selected, _, _ = load(["simple_text", "text_cache_keys"])
        with tarfile.open(archive) as package:
            for path, patch in PATCHES.items():
                original = package.extractfile("wpewebkit-2.53.92/" + path).read().decode()
                rendered = render(ROOT, original, selected[path], path)
                self.assertEqual(without_comments(rendered), rendered)
                with self.assertRaises(RuntimeError):
                    patch(patch(original))
                if path.endswith("TextMeasurementCache.h"):
                    self.assertIn("maxTextLength = 128", rendered)
                    self.assertIn(".first(length)", rendered)
                    self.assertIn("left.m_hashAndLength != right.m_hashAndLength", rendered)
                    self.assertIn(".first(left.length())", rendered)
                    self.assertIn('std::getenv("WEBKIT_EDEN_TEXT_CACHE_KEYS")', rendered)
                else:
                    self.assertEqual(rendered.count("TEST_F(ComplexTextControllerTest, EdenTextMeasurementCache"), 4)

    def test_unmatched_source_is_rejected(self):
        for path, patch in PATCHES.items():
            with self.subTest(path=path), self.assertRaises(RuntimeError):
                patch("unrelated source")

    def test_selected_engine_cache_semantics_with_both_policies(self):
        binary = os.environ.get("EDEN_TEST_WEBCORE_BINARY")
        if not binary:
            self.skipTest("Set EDEN_TEST_WEBCORE_BINARY to the candidate TestWebCore executable")
        for enabled in ("0", "1"):
            with self.subTest(enabled=enabled):
                result = subprocess.run(
                    [binary, "--gtest_filter=ComplexTextControllerTest.EdenTextMeasurementCache*"],
                    env={**os.environ, "WEBKIT_EDEN_TEXT_CACHE_KEYS": enabled},
                    capture_output=True,
                    text=True,
                    timeout=30,
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("4 tests", result.stdout)
                self.assertNotIn("[  SKIPPED ]", result.stdout)


if __name__ == "__main__":
    unittest.main()
