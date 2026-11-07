from pathlib import Path
import sys
import tarfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from wpe.adopted.author_rule_memo import FILES, PATCHES, SOURCES
from wpe.adopted.ua_rule_memo import PATCHES as UA_PATCHES
from wpe.adopted_patches import render
from wpe.patch_support import without_comments


ARCHIVE = ROOT / "third_party/wpe/packages/wpewebkit-2.53.92.tar.xz"
PREFIX = "wpewebkit-2.53.92/"


class AuthorRuleMemoPatchTest(unittest.TestCase):
    def test_raw_archive_composition_and_one_shot_anchors(self):
        if not ARCHIVE.is_file():
            self.skipTest("The pinned WebKit source archive is required")
        self.assertEqual(
            set(PATCHES),
            {
                "Source/WebCore/style/RuleSet.h",
                "Source/WebCore/style/RuleSet.cpp",
                "Source/WebCore/style/ElementRuleCollector.h",
                "Source/WebCore/style/ElementRuleCollector.cpp",
            },
        )
        with tarfile.open(ARCHIVE) as package:
            for path, patch in PATCHES.items():
                original = package.extractfile(PREFIX + path).read().decode()
                transformations = [patch]
                if path in UA_PATCHES:
                    transformations.insert(0, UA_PATCHES[path])
                transformed = render(ROOT, original, transformations, path)
                self.assertEqual(without_comments(transformed), transformed)
                with self.assertRaises(RuntimeError):
                    patch(transformed)
                if path.endswith("RuleSet.h"):
                    self.assertIn("EdenAuthorRuleMemo m_edenAuthorRuleMemo", transformed)
                    self.assertIn("edenInvalidateAuthorRuleMemo() const", transformed)
                    self.assertGreater(
                        transformed.rindex("m_edenAuthorRuleMemo"),
                        transformed.index("bool m_isBuilding"),
                    )
                elif path.endswith("RuleSet.cpp"):
                    self.assertIn("edenInvalidateAuthorRuleMemo();", transformed)
                    add_rule = transformed.index("RuleData &&ruleData,")
                    self.assertLess(
                        transformed.index("edenInvalidateAuthorRuleMemo();", add_rule),
                        transformed.index("++m_ruleCount", add_rule),
                    )
                    self.assertLess(
                        transformed.index("edenInvalidateAuthorRuleMemo();", transformed.index("void RuleSet::shrinkToFit")),
                        transformed.index("shrinkMapVectorsToFit(m_idRules)", transformed.index("void RuleSet::shrinkToFit")),
                    )
                    media_change = transformed.index(
                        "edenInvalidateAuthorRuleMemo();",
                        transformed.index("if (collectedChanges.requiredFullReset"),
                    )
                    self.assertLess(media_change, transformed.index("ruleData.setEnabled"))
                elif path.endswith("ElementRuleCollector.cpp"):
                    self.assertIn("edenCollectAuthorRulesWithMemo(matchRequest)", transformed)
                    self.assertIn("edenCollectUARulesWithMemo(rules)", transformed)
                    self.assertIn("!cachedRule.ruleData->styleRule().properties().isEmpty()", transformed)
                    self.assertIn("element.hasClass()", transformed)
                    self.assertIn("cachedRule.ruleData->isEnabled()", transformed)
                    self.assertIn("WEBKIT_EDEN_VERIFY_AUTHOR_RULE_MEMO", transformed)
                    self.assertIn("WEBKIT_EDEN_AUTHOR_RULE_MEMO", transformed)

    def test_cache_bounds_fallbacks_and_lifecycle_payload(self):
        header = (ROOT / "scripts/wpe/adopted/author_rule_memo.h").read_text()
        implementation = (ROOT / "scripts/wpe/adopted/author_rule_memo.inc").read_text()
        self.assertIn("edenAuthorMemoMaximumEntries = 32", implementation)
        self.assertIn("edenAuthorMemoMinimumExpensiveRules = 3", implementation)
        self.assertIn("edenAuthorMemoMaximumStaticRules = 64", implementation)
        self.assertIn("edenAuthorMemoMaximumCandidates = 256", implementation)
        self.assertIn("edenAuthorMemoMaximumAttributes = 4", implementation)
        self.assertIn("edenAuthorRuleMemoHasLocalSelectorGrammar", implementation)
        self.assertIn("return false;", implementation[implementation.index("switch (selector->match())"):])
        self.assertIn("element.isInShadowTree()", implementation)
        self.assertIn("element.assignedSlot()", implementation)
        self.assertIn("element.shadowRoot()", implementation)
        self.assertIn("hasContainerQueries()", implementation)
        self.assertIn("hasScopeRules()", implementation)
        self.assertIn("memo.entryCount >= edenAuthorMemoMaximumEntries", implementation)
        self.assertIn("candidate.attributeValues[i] != key.attributeValues[i]", implementation)
        self.assertIn("Vector<EdenAuthorRuleMemoResult, 32> rules", header)
        self.assertIn("entries.clear();", header)
        self.assertIn("bool firstHit", implementation)

    def test_layout_fixture_and_generated_inventory(self):
        self.assertEqual(
            FILES,
            {
                "Source/WebCore/style/EdenAuthorRuleMemo.h": "author_rule_memo.h",
                "LayoutTests/fast/css/eden-author-rule-memo.html": "author_rule_memo_test.html",
                "LayoutTests/fast/css/eden-author-rule-memo-expected.txt": "author_rule_memo_test_expected.txt",
                "LayoutTests/resources/eden-author-rule-memo.js": "author_rule_memo_test.js",
            },
        )
        self.assertEqual(
            set(SOURCES),
            {
                "author_rule_memo.h",
                "author_rule_memo.inc",
                "author_rule_memo_test.html",
                "author_rule_memo_test.js",
                "author_rule_memo_test_expected.txt",
            },
        )
        for relative_path, payload in FILES.items():
            self.assertTrue((ROOT / "scripts/wpe/adopted" / payload).is_file(), relative_path)
        fixture = (ROOT / "scripts/wpe/adopted/author_rule_memo_test.js").read_text()
        for behavior in (
            "class mutation changes key",
            "id mutation changes key",
            "attribute value mutation changes key",
            "namespaced attribute mutation changes key",
            "CSSOM empty declaration is skipped on cached rule",
            "disabled media stylesheet",
            "ancestor combinator",
            "positional pseudo class",
            "has selector updates after descendant removal",
            "shadow tree selector",
            "cap fixture repeated shape",
            "quirks id selector",
            "inherited custom property",
        ):
            with self.subTest(behavior=behavior):
                self.assertIn(behavior, fixture)


if __name__ == "__main__":
    unittest.main()
