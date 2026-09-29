from pathlib import Path
import re
import sys
import tarfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from wpe.adopted.svg_length_parse import ELEMENT_LENGTHS, FILES, PATCHES, SOURCE, SOURCES
from wpe.adopted_patches import render
from wpe.patch_support import without_comments


ARCHIVE = ROOT / "third_party/wpe/packages/wpewebkit-2.53.92.tar.xz"
PREFIX = "wpewebkit-2.53.92/"
SOURCE_TREE = ROOT / "third_party/wpe/source/wpewebkit-2.53.92-combined3"


class SVGLengthParsePatchTest(unittest.TestCase):
    def test_archive_sources_and_one_shot_transformations(self):
        if not ARCHIVE.is_file():
            self.skipTest("The pinned WebKit source archive is required")
        expected_paths = {
            SOURCE + "svg/properties/SVGProperty.h",
            SOURCE + "svg/properties/SVGValueProperty.h",
            *(SOURCE + f"svg/{element}.cpp" for element in ELEMENT_LENGTHS),
        }
        self.assertEqual(set(PATCHES), expected_paths)
        with tarfile.open(ARCHIVE) as package:
            for path, patch in PATCHES.items():
                original = package.extractfile(PREFIX + path).read().decode()
                combined = (SOURCE_TREE / path).read_text()
                self.assertEqual(original, combined, path)
                transformed = render(ROOT, original, [patch], path)
                self.assertEqual(without_comments(transformed), transformed, path)
                with self.assertRaises(RuntimeError):
                    patch(transformed)
                if path.endswith("SVGProperty.h"):
                    self.assertIn("CleanWithSuccessfulLengthAttributeParse", transformed)
                    self.assertIn("if (isDirty())", transformed)
                    self.assertIn("invalidateLengthAttributeParseSuccess();", transformed)
                    self.assertIn("if (m_state != SVGPropertyState::Dirty)", transformed)
                    self.assertIn("m_state = SVGPropertyState::Clean;", transformed)
                elif path.endswith("SVGValueProperty.h"):
                    self.assertIn("invalidateLengthAttributeParseSuccess();", transformed)
                    self.assertIn("m_value = value;", transformed)
                else:
                    self._assert_element_parse_cases(path, original, transformed)

    def _assert_element_parse_cases(self, path, original, transformed):
        element = Path(path).stem
        expected = dict(ELEMENT_LENGTHS[element])
        for attribute, member in expected.items():
            case_start = transformed.index(f"case AttributeNames::{attribute}:")
            case_end = transformed.index("break;", case_start)
            case = transformed[case_start:case_end]
            self.assertIn(f"edenShouldSkipSVGLengthAttributeParse(oldValue, newValue, {member}.get())", case)
            self.assertIn(f"{member}->baseVal()->setLengthAttributeParseSucceeded(parseError == SVGParsingError::None)", case)
            self.assertIn("setBaseValInternal(SVGLengthValue::construct(", re.sub(r"\s+", "", case))
            self.assertLess(case.index("edenShouldSkipSVGLengthAttributeParse"), case.index("setBaseValInternal"))
        report_start = transformed.index("reportAttributeParsingError(parseError, name, newValue)")
        superclass_start = transformed.index("::attributeChanged(name, oldValue, newValue, attributeModificationReason)")
        self.assertLess(transformed.index("switch (name.nodeName())"), report_start)
        self.assertLess(report_start, superclass_start)
        self.assertEqual(
            transformed.count("SVGLengthNegativeValuesMode::Forbid"),
            original.count("SVGLengthNegativeValuesMode::Forbid"),
        )
        if element == "SVGImageElement":
            preserve_start = transformed.index("case AttributeNames::preserveAspectRatioAttr:")
            preserve_end = transformed.index("return;", preserve_start)
            self.assertNotIn("edenShouldSkipSVGLengthAttributeParse", transformed[preserve_start:preserve_end])

    def test_lifecycle_guards_and_runtime_control_payload(self):
        helper = (ROOT / "scripts/wpe/adopted/svg_length_parse.h").read_text()
        for guard in (
            "WEBKIT_EDEN_DISABLE_SVG_LENGTH_PARSE_SKIP",
            "oldValue.isNull()",
            "oldValue != newValue",
            "length.isDirty()",
            "length.isAnimating()",
            "baseValue->hasSuccessfulLengthAttributeParse()",
            "baseValue->contextElement()",
        ):
            with self.subTest(guard=guard):
                self.assertIn(guard, helper)
        self.assertEqual(FILES, {SOURCE + "svg/EdenSVGLengthAttributeParse.h": "svg_length_parse.h"})
        self.assertEqual(SOURCES, ["svg_length_parse.h"])
        property_header = (SOURCE_TREE / (SOURCE + "svg/properties/SVGProperty.h")).read_text()
        self.assertIn("void detach()", property_header)
        self.assertIn("m_state = SVGPropertyState::Clean;", property_header)
        length_header = (SOURCE_TREE / (SOURCE + "svg/SVGLength.h")).read_text()
        self.assertIn("Ref<SVGLength> clone() const", length_header)
        self.assertIn("return SVGLength::create(m_value);", length_header)
        value_property = (SOURCE_TREE / (SOURCE + "svg/properties/SVGValueProperty.h")).read_text()
        self.assertIn("void setValue(const PropertyType& value)", value_property)

    def test_native_fixture_covers_length_and_lifecycle_cases(self):
        fixture = (ROOT / "tests/wpe/svg-length-parse.js").read_text()
        for behavior in (
            "repeated equal attribute writes preserve mutation notifications",
            "changed attribute value reparses",
            "attribute removal resets the reflected length",
            "empty length uses its default value",
            "invalid length remains observable",
            "negative forbidden length remains observable",
            "pixel units survive repeated writes",
            "percentage units survive repeated writes",
            "calc length is rejected",
            "baseVal writes synchronize before an equal attribute write",
            "cloned element has independent animated length state",
            "active SMIL animation retains parser update semantics",
        ):
            with self.subTest(behavior=behavior):
                self.assertIn(behavior, fixture)


if __name__ == "__main__":
    unittest.main()
