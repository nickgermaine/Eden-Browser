import os
from pathlib import Path
import subprocess
import sys
import tarfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from wpe.adopted.jsc_json_keys import PATCHES, json_keys_patch
from wpe.adopted_patches import load, render
from wpe.performance_patches import names, resolve_patches, selections


class JSONPropertyKeysPatchTest(unittest.TestCase):
    def test_registered_in_default_builds(self):
        self.assertIn("jsc-json-property-keys", names(resolve_patches(False)))
        self.assertIn("jsc-json-property-keys", names(resolve_patches(True)))
        self.assertIn("jsc_json_keys", selections(resolve_patches(True), "adopted"))
        patches, _, sources = load(["jsc_json_keys"])
        self.assertEqual(set(patches), set(PATCHES))
        self.assertTrue(any(path.name == "jsc_switch.py" for path in sources))

    def test_rejects_missing_ambiguous_or_already_patched_source(self):
        anchor = '''            if (type == TokComma) {
                type = m_lexer.next();
                bool isPropertyKey = type == TokString;
                if constexpr (parserMode != StrictJSON)
'''
        for body in ("", anchor + anchor):
            with self.subTest(body=body), self.assertRaises(RuntimeError):
                json_keys_patch('#include "config.h"\n' + body)
        patched = json_keys_patch('#include "config.h"\n' + anchor)
        with self.assertRaises(RuntimeError):
            json_keys_patch(patched)

    def test_pinned_patch_preserves_non_object_paths(self):
        archive = ROOT / "third_party/wpe/packages/wpewebkit-2.53.92.tar.xz"
        if not archive.is_file():
            self.skipTest("The pinned WebKit source archive is required")
        path = next(iter(PATCHES))
        with tarfile.open(archive) as package:
            original = package.extractfile("wpewebkit-2.53.92/" + path).read().decode()
        patched = json_keys_patch(original)
        start = original.index("    ASSERT(type == TokLBrace);", original.index("::parseRecursively(VM&"))
        end = original.index("constexpr unsigned maximumRangesStackRecursion", start)
        self.assertEqual(original[start:end].count("m_lexer.nextMaybeIdentifier()"), 1)
        self.assertEqual(patched.count("m_lexer.nextMaybeIdentifier()"), original.count("m_lexer.nextMaybeIdentifier()") + 1)
        self.assertEqual(patched[patched.index("constexpr unsigned maximumRangesStackRecursion"):], original[end:])
        candidate = render(ROOT, original, [json_keys_patch], path)
        self.assertIn("sizeof(CharType) == 2 && parserMode == StrictJSON", candidate)
        self.assertIn('std::getenv("WEBKIT_EDEN_JSC_JSON_KEYS")', candidate)

    def test_semantic_boundaries_with_selected_jsc(self):
        executable = os.environ.get("EDEN_JSC_TEST_BINARY")
        if not executable:
            self.skipTest("Set EDEN_JSC_TEST_BINARY to the candidate jsc executable")
        for enabled in ("0", "1"):
            with self.subTest(enabled=enabled):
                environment = {**os.environ, "WEBKIT_EDEN_JSC_JSON_KEYS": enabled}
                result = subprocess.run(
                    [executable, str(ROOT / "tests/jsc/json-property-keys.js")],
                    env=environment,
                    capture_output=True,
                    text=True,
                    timeout=30,
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("JSON property key boundaries passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
