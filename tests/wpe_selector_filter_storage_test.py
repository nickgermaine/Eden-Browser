from pathlib import Path
import os
import subprocess
import sys
import tarfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
ARCHIVE = ROOT / "third_party/wpe/packages/wpewebkit-2.53.92.tar.xz"
PREFIX = "wpewebkit-2.53.92/"
sys.path.insert(0, str(ROOT / "scripts"))

from wpe.adopted.selector_filter_storage import (
    HEADER,
    HEADERS_LIST,
    PARENT_FRAME_INLINE_HASHES,
    PATCHES,
    SOURCE,
    SOURCES,
    TEST,
)
from wpe.adopted_patches import render
from wpe.patch_support import without_cmake_comments, without_comments


TEST_NAMES = (
    "ElementHashOrderMatchesSelectorHashes",
    "OverflowingParentFrameKeepsEveryHash",
    "DeepParentStackMatchesFreshFiltersAfterPops",
    "DuplicateAncestorHashesSurviveChildPop",
    "SvgAndHtmlAttributeCasingMatchesSelectors",
)


def _braced_block(text, marker):
    start = text.index(marker)
    opening = text.index("{", start)
    depth = 0
    for position in range(opening, len(text)):
        if text[position] == "{":
            depth += 1
        elif text[position] == "}":
            depth -= 1
            if depth == 0:
                return text[start:position + 1]
    raise AssertionError("Unclosed block for " + marker)


def _body(block):
    return block[block.index("{") + 1:block.rindex("}")]


def _archive_text(path):
    with tarfile.open(ARCHIVE) as package:
        return package.extractfile(PREFIX + path).read().decode()


class SelectorFilterStoragePatchTest(unittest.TestCase):
    def setUp(self):
        if not ARCHIVE.is_file():
            self.skipTest("The pinned WebKit source archive is required")

    def test_inventory_is_limited_to_parent_frame_storage_and_its_tests(self):
        self.assertEqual(set(PATCHES), {HEADER, SOURCE, HEADERS_LIST, TEST})
        self.assertNotIn("Source/WebCore/style/HasSelectorFilter.cpp", PATCHES)
        self.assertNotIn("Source/WebCore/style/HasSelectorFilter.h", PATCHES)
        self.assertEqual(SOURCES, ["selector_filter_storage_tests.inc"])
        self.assertEqual(PARENT_FRAME_INLINE_HASHES, 10)

    def test_header_changes_only_the_parent_frame_capacity(self):
        original = _archive_text(HEADER)
        transformed = render(ROOT, original, [PATCHES[HEADER]], HEADER)
        baseline = render(ROOT, original, [], HEADER)
        self.assertEqual(without_comments(transformed), transformed)
        with self.assertRaises(RuntimeError):
            PATCHES[HEADER](transformed)
        self.assertIn("static constexpr size_t parentFrameInlineHashCapacity = 10;", transformed)
        self.assertIn("Vector<unsigned, parentFrameInlineHashCapacity> identifierHashes;", transformed)
        self.assertEqual(transformed.count("Vector<unsigned, 4>"), 1)
        self.assertIn("static void collectElementIdentifierHashes(const Element &, Vector<unsigned, 4> &);", transformed)
        self.assertIn("Vector<ParentStackFrame, 20> m_parentStack;", transformed)
        self.assertEqual(
            baseline.replace("Vector<unsigned, 4> identifierHashes;", "Vector<unsigned, parentFrameInlineHashCapacity> identifierHashes;"),
            transformed.replace("        static constexpr size_t parentFrameInlineHashCapacity = 10;\n\n", ""),
        )

    def test_source_moves_the_collection_body_without_changing_it(self):
        original = _archive_text(SOURCE)
        transformed = render(ROOT, original, [PATCHES[SOURCE]], SOURCE)
        baseline = render(ROOT, original, [], SOURCE)
        self.assertEqual(without_comments(transformed), transformed)
        with self.assertRaises(RuntimeError):
            PATCHES[SOURCE](transformed)
        original_body = _body(_braced_block(baseline, "void SelectorFilter::collectElementIdentifierHashes("))
        declaration = "collectIdentifierHashesInto(const Element &element, Vector<unsigned, inlineCapacity> &identifierHashes)"
        helper = _braced_block(transformed, declaration)
        self.assertRegex(transformed[:transformed.index(declaration)][-80:], r"template <size_t inlineCapacity>\s+static void\s+$")
        self.assertEqual(_body(helper), original_body)
        wrapper = _braced_block(transformed, "void SelectorFilter::collectElementIdentifierHashes(")
        self.assertIn("Vector<unsigned, 4> &identifierHashes", wrapper)
        self.assertEqual(_body(wrapper).strip(), "collectIdentifierHashesInto(element, identifierHashes);")
        push = _braced_block(transformed, "void SelectorFilter::pushParent(Element *parent)")
        original_push = _braced_block(baseline, "void SelectorFilter::pushParent(Element *parent)")
        self.assertEqual(
            push,
            original_push.replace(
                "collectElementIdentifierHashes(*parent, parentFrame.identifierHashes);",
                "collectIdentifierHashesInto(*parent, parentFrame.identifierHashes);",
            ),
        )
        for unchanged in (
            "void SelectorFilter::popParent()",
            "static bool NODELETE isExcludedAttribute(",
            "SelectorFilter::collectSimpleSelectorHash(CollectedSelectorHashes &collectedHashes",
            "auto SelectorFilter::chooseSelectorHashesForFilter(",
        ):
            self.assertEqual(_braced_block(transformed, unchanged), _braced_block(baseline, unchanged))
        self.assertIn("enum { TagNameSalt = 13, IdSalt = 17, ClassSalt = 19, AttributeSalt = 23 };", transformed)
        self.assertEqual(
            baseline.replace(_braced_block(baseline, "void SelectorFilter::collectElementIdentifierHashes("), "").count("collectElementIdentifierHashes("),
            1,
        )

    def test_selector_filter_header_is_exported_for_tests(self):
        original = _archive_text(HEADERS_LIST)
        transformed = render(ROOT, original, [PATCHES[HEADERS_LIST]], HEADERS_LIST)
        self.assertEqual(transformed.count("    css/SelectorFilter.h\n"), 1)
        self.assertLess(transformed.index("    css/SelectorFilter.h\n"), transformed.index("    css/ShorthandSerializer.h\n"))
        self.assertEqual(without_cmake_comments(transformed), transformed)
        self.assertNotIn("#", transformed)
        self.assertEqual(
            transformed.replace("    css/SelectorFilter.h\n", ""),
            without_cmake_comments(original),
        )
        with self.assertRaises(RuntimeError):
            PATCHES[HEADERS_LIST](transformed)

    def test_native_regression_payload_is_complete(self):
        original = _archive_text(TEST)
        transformed = PATCHES[TEST](original)
        rendered = render(ROOT, original, [PATCHES[TEST]], TEST)
        self.assertIn("#include <WebCore/SelectorFilter.h>", transformed)
        self.assertEqual(without_comments(rendered), rendered)
        for name in TEST_NAMES:
            self.assertIn(f"TEST(SelectorFilterStorage, {name})", transformed)
        with self.assertRaises(RuntimeError):
            PATCHES[TEST](transformed)
        tests = (ROOT / "scripts/wpe/adopted/selector_filter_storage_tests.inc").read_text()
        self.assertEqual(without_comments(tests), tests)
        self.assertIn("for (unsigned depth = 0; depth < 30; ++depth)", tests)
        self.assertIn("class='c0 c1 c2 c3 c4 c5 c6 c7 c8 c9 c10 c11'", tests)
        self.assertIn("ASSERT_EQ(17u, hashes.size());", tests)
        self.assertIn("ASSERT_GT(chain.size(), 20u);", tests)
        self.assertIn("viewBox='0 0 10 10'", tests)
        self.assertIn("data-Mixed='x'", tests)

    def test_selected_testwebcore_selector_filter_regressions(self):
        binary = os.environ.get("EDEN_TEST_WEBCORE_BINARY")
        if not binary:
            self.skipTest("Set EDEN_TEST_WEBCORE_BINARY to the candidate TestWebCore executable")
        result = subprocess.run(
            [binary, "--gtest_filter=SelectorFilterStorage.*", "--gtest_brief=0"],
            capture_output=True,
            text=True,
            timeout=120,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for name in TEST_NAMES:
            self.assertIn(f"[       OK ] SelectorFilterStorage.{name}", result.stdout)
        self.assertIn(f"[  PASSED  ] {len(TEST_NAMES)} tests.", result.stdout)
        self.assertNotIn("[  SKIPPED ]", result.stdout)


if __name__ == "__main__":
    unittest.main()
