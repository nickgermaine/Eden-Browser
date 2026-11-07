from pathlib import Path
import re
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from wpe.adopted.intrinsic_width_probe import PATCHES


SOURCE = ROOT / "third_party/wpe/source/wpewebkit-2.53.92-combined2"


class IntrinsicWidthProbePatchTest(unittest.TestCase):
    def test_pinned_source_transforms_once_and_rejects_reapplication(self):
        self.assertEqual(
            set(PATCHES),
            {
                "Source/WebCore/layout/formattingContexts/inline/IntrinsicWidthHandler.cpp",
                "Source/WebCore/layout/formattingContexts/inline/IntrinsicWidthHandler.h",
            },
        )
        for relative_path, patch in PATCHES.items():
            with self.subTest(path=relative_path):
                original = (SOURCE / relative_path).read_text()
                transformed = patch(original)
                self.assertNotEqual(original, transformed)
                with self.assertRaises(RuntimeError):
                    patch(transformed)

    def test_probe_is_opt_in_bounded_and_emits_path_scoped_records(self):
        relative_path = "Source/WebCore/layout/formattingContexts/inline/IntrinsicWidthHandler.cpp"
        transformed = PATCHES[relative_path]((SOURCE / relative_path).read_text())
        self.assertIn('std::getenv("WEBKIT_EDEN_PATCH_COUNTERS")', transformed)
        self.assertIn('std::strcmp(value, "1")', transformed)
        self.assertIn("eventLimit = 262144", transformed)
        self.assertIn("truncated=1 event_limit=%llu", transformed)
        self.assertIn("!edenIntrinsicWidthProbeReserveEvent(event)", transformed)
        self.assertIn("if (!edenIntrinsicWidthProbeEnabled() || !edenIntrinsicWidthProbeReserveEvent(event))", transformed)
        self.assertIn("event=%llu path=%s candidate=%u eligible=%u lines=%u start_ns=%llu duration_ns=%llu eligibility_ns=%llu", transformed)
        self.assertIn('path = "existing-simplified"', transformed)
        self.assertIn('path = "text-only"', transformed)
        self.assertIn('auto path = "full"', transformed)
        self.assertIn("*lineCount = static_cast<unsigned>(lineIndex + 1)", transformed)
        self.assertOptOutBodyMatchesOriginal(transformed, (SOURCE / relative_path).read_text())

    def test_proposed_gate_checks_documented_fallback_axes(self):
        relative_path = "Source/WebCore/layout/formattingContexts/inline/IntrinsicWidthHandler.cpp"
        transformed = PATCHES[relative_path]((SOURCE / relative_path).read_text())
        required_checks = (
            "m_inlineItems.hasTextAutospace()",
            "&rootStyle != &rootBox.firstLineStyle()",
            "LineBreak::Anywhere",
            "LineBreak::AfterWhiteSpace",
            "WordBreak::BreakAll",
            "WordBreak::BreakWord",
            "OverflowWrap::Anywhere",
            "Hyphens::Auto",
            "rootStyle.usedLetterSpacing() < 0",
            "WhiteSpaceCollapse::Collapse",
            "WhiteSpaceCollapse::PreserveBreaks",
            "WhiteSpaceCollapse::Preserve",
            "WhiteSpaceCollapse::BreakSpaces",
            "&textStyle == &textBox->firstLineStyle()",
            "textStyle.whiteSpaceCollapse() == whiteSpaceCollapse",
            "textStyle.textWrapMode() == rootStyle.textWrapMode()",
            "textStyle.wordBreak() == rootStyle.wordBreak()",
            "textStyle.lineBreak() == rootStyle.lineBreak()",
            "textStyle.overflowWrap() == rootStyle.overflowWrap()",
            "textStyle.hyphens() == rootStyle.hyphens()",
            "!inlineTextItem->length() || !width || *width <= 0",
            "inlineTextItem->hasTrailingSoftHyphen()",
            "inlineTextItem->isZeroWidthSpaceSeparator()",
            "inlineTextItem->isQuirkNonBreakingSpace()",
        )
        for check in required_checks:
            with self.subTest(check=check):
                self.assertIn(check, transformed)
        self.assertIn("if (!edenIntrinsicWidthProbeEnabled() || !edenIntrinsicWidthProbeReserveEvent(event))", transformed)
        self.assertIn("return computedIntrinsicWidthForConstraint(IntrinsicWidthMode::Minimum, lineBuilder, MayCacheLayoutResult::No);", transformed)

    def assertOptOutBodyMatchesOriginal(self, transformed, original):
        method_start = "InlineLayoutUnit IntrinsicWidthHandler::minimumContentSize()\n{"
        original_body_start = original.index(method_start) + len(method_start)
        original_body_end = original.index("\n}\n", original_body_start)
        original_body = original[original_body_start:original_body_end]
        opt_out_start = transformed.index("if (!edenIntrinsicWidthProbeEnabled() || !edenIntrinsicWidthProbeReserveEvent(event)) {")
        opt_out_body_start = transformed.index("{", opt_out_start) + 1
        opt_out_body_end = transformed.index("\n    }\n\n    auto candidate", opt_out_body_start)
        opt_out_body = transformed[opt_out_body_start:opt_out_body_end]
        normalize = lambda value: re.sub(r"\s+", " ", value).strip()
        self.assertEqual(normalize(opt_out_body), normalize(original_body))


if __name__ == "__main__":
    unittest.main()
