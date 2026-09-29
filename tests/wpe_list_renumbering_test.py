import os
from pathlib import Path
import subprocess
import sys
import tarfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from wpe.adopted.list_renumbering import FILES, PATCHES, SOURCES
from wpe.adopted_patches import render
from wpe.patch_support import without_comments


class ListRenumberingPatchTest(unittest.TestCase):
    def test_pinned_sources_and_fixture_payload(self):
        archive = ROOT / "third_party/wpe/packages/wpewebkit-2.53.92.tar.xz"
        if not archive.is_file():
            self.skipTest("The pinned WebKit source archive is required")
        self.assertEqual(set(PATCHES), {
            "Source/WebCore/dom/Document.h",
            "Source/WebCore/rendering/RenderListItem.h",
            "Source/WebCore/rendering/RenderListItem.cpp",
            "Tools/TestWebKitAPI/Tests/WebCore/CachedMatchFinder.cpp",
        })
        self.assertEqual(
            FILES,
            {
                "Tools/TestWebKitAPI/Tests/WebCore/EdenListRenumberingFixture.h": "list_renumbering_fixture.h",
            },
        )
        self.assertEqual(set(SOURCES), {"list_renumbering_tests.inc", "list_renumbering_fixture.h"})
        with tarfile.open(archive) as package:
            for relative_path, patch in PATCHES.items():
                original = package.extractfile("wpewebkit-2.53.92/" + relative_path).read().decode()
                transformed = render(ROOT, original, [patch], relative_path)
                self.assertEqual(without_comments(transformed), transformed)
                with self.assertRaises(RuntimeError):
                    patch(patch(original))
                if relative_path.endswith("Document.h"):
                    self.assertIn("listItemsWithCachedValues() const", transformed)
                    self.assertIn("m_listItemsWithCachedValues{0}", transformed)
                elif relative_path.endswith("RenderListItem.h"):
                    self.assertIn("void willBeDestroyed() final", transformed)
                elif relative_path.endswith("RenderListItem.cpp"):
                    self.assertIn('std::getenv("WEBKIT_EDEN_LIST_RENUMBERING")', transformed)
                    self.assertIn('std::getenv("WEBKIT_EDEN_LIST_RENUMBERING_DIAGNOSTICS")', transformed)
                    self.assertIn('std::getenv("WEBKIT_EDEN_VERIFY_LIST_RENUMBERING")', transformed)
                    self.assertIn("EDEN_LIST_RENUMBERING|truncated=1", transformed)
                    self.assertIn("setCachedValue(valueForItem(value, directives))", transformed)
                    self.assertIn("clearCachedValue();", transformed)
                    self.assertLess(
                        transformed.index("orderedList->itemCountChanged();"),
                        transformed.index("!document().listItemsWithCachedValues()"),
                    )
                    self.assertIn("if (edenListRenumberingEnabled() && !document().listItemsWithCachedValues())", transformed)
                    self.assertIn("RELEASE_ASSERT(!verifiedItem->m_value)", transformed)
                    self.assertNotIn("WEBKIT_EDEN_VERIFY_PATCHES", transformed)
                    self.assertIn('reportEdenListRenumberingEvent("verified-skip", verifiedItems)', transformed)
                    self.assertIn("while ((item = subsequentListItem(*list, *item)) && item->m_value)", transformed)
                else:
                    self.assertIn("TEST(ListRenumbering, PreservesMarkersAcrossMutation)", transformed)
                    self.assertIn('"reverse-one"_s), "2. "_s', transformed)
                    self.assertIn('"explicit-value"_s), "12. "_s', transformed)
                    self.assertIn('"inner-two"_s), "2. "_s', transformed)
                    self.assertIn('"css-set"_s), "7. "_s', transformed)
                    self.assertIn("CacheCountTracksRendererTeardownAndAdoption", transformed)
        fixture = (ROOT / "scripts/wpe/adopted/list_renumbering_fixture.h").read_text()
        self.assertIn("counter-reset: list-item 20", fixture)
        self.assertIn("counter-set: list-item 7", fixture)

    def test_selected_engine_markers_with_enabled_and_control_modes(self):
        binary = os.environ.get("EDEN_TEST_WEBCORE_BINARY")
        if not binary:
            self.skipTest("Set EDEN_TEST_WEBCORE_BINARY to the candidate TestWebCore executable")
        expected_tests = [
            "PreservesMarkersAcrossMutation",
            "CacheCountTracksRendererTeardownAndAdoption",
        ]
        for enabled in ("1", "0"):
            with self.subTest(enabled=enabled):
                environment = {
                    **os.environ,
                    "WEBKIT_EDEN_LIST_RENUMBERING": enabled,
                    "WEBKIT_EDEN_VERIFY_LIST_RENUMBERING": "1" if enabled == "1" else "0",
                }
                inventory = subprocess.run(
                    [binary, "--gtest_list_tests"],
                    env=environment,
                    capture_output=True,
                    text=True,
                    timeout=30,
                )
                self.assertEqual(inventory.returncode, 0, inventory.stdout + inventory.stderr)
                inventory_lines = inventory.stdout.splitlines()
                suite_index = inventory_lines.index("ListRenumbering.")
                actual_tests = []
                for line in inventory_lines[suite_index + 1 :]:
                    if not line or not line[0].isspace():
                        break
                    actual_tests.append(line.strip())
                self.assertEqual(actual_tests, expected_tests)
                result = subprocess.run(
                    [binary, "--gtest_filter=ListRenumbering.*", "--gtest_brief=0"],
                    env=environment,
                    capture_output=True,
                    text=True,
                    timeout=60,
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("[  PASSED  ] 2 tests.", result.stdout)


if __name__ == "__main__":
    unittest.main()
