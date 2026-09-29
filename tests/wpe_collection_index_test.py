from pathlib import Path
import re
import sys
import tarfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
ARCHIVE = ROOT / "third_party/wpe/packages/wpewebkit-2.53.92.tar.xz"
PREFIX = "wpewebkit-2.53.92/"
sys.path.insert(0, str(ROOT / "scripts"))

from wpe.adopted.collection_index import FILES, PATCHES, SOURCES
from wpe.adopted_patches import render
from wpe.patch_support import without_comments, without_source_list_comments


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


def _subroutine(text, name, following):
    start = text.index("sub " + name)
    end = text.index("sub " + following, start + len(name) + 4)
    return text[start:end]


class CollectionIndexPatchTest(unittest.TestCase):
    def test_archive_transforms_have_exact_scope_and_strip_source_comments(self):
        if not ARCHIVE.is_file():
            self.skipTest("The pinned WebKit source archive is required")
        self.assertEqual(
            set(PATCHES),
            {
                "Source/WebCore/html/HTMLCollection.idl",
                "Source/WebCore/bindings/js/JSHTMLCollectionCustom.cpp",
                "Source/WebCore/Sources.txt",
            },
        )
        self.assertEqual(
            FILES,
            {
                "LayoutTests/fast/dom/eden-html-collection-indexed-getter.html": "collection_index_test.html",
                "LayoutTests/fast/dom/eden-html-collection-indexed-getter-expected.txt": "collection_index_test_expected.txt",
                "LayoutTests/resources/eden-html-collection-indexed-getter.js": "collection_index_test.js",
            },
        )
        self.assertEqual(
            set(SOURCES),
            {
                "collection_index_test.html",
                "collection_index_test.js",
                "collection_index_test_expected.txt",
            },
        )
        with tarfile.open(ARCHIVE) as package:
            for path, patch in PATCHES.items():
                original = package.extractfile(PREFIX + path).read().decode()
                transformed = render(ROOT, original, [patch], path)
                self.assertEqual(without_comments(transformed), transformed)
                with self.assertRaises(RuntimeError):
                    patch(transformed)
                if path.endswith("HTMLCollection.idl"):
                    self.assertEqual(transformed.count("CustomGetOwnPropertySlot"), 1)
                    self.assertEqual(transformed.count("CustomPut"), 1)
                elif path.endswith("Sources.txt"):
                    self.assertEqual(transformed.count("bindings/js/JSHTMLCollectionCustom.cpp\n"), 1)
                    self.assertIn("bindings/js/JSHTMLAllCollectionCustom.cpp\nbindings/js/JSHTMLCollectionCustom.cpp\n", transformed)
                    self.assertNotIn("//", transformed)
                    entries = [line for line in without_source_list_comments(original).splitlines() if line.strip()]
                    anchor = entries.index("bindings/js/JSHTMLAllCollectionCustom.cpp") + 1
                    self.assertEqual(
                        [line for line in transformed.splitlines() if line.strip()],
                        entries[:anchor] + ["bindings/js/JSHTMLCollectionCustom.cpp"] + entries[anchor:],
                    )
                else:
                    self.assertNotIn("toJSNewlyCreated", transformed)
                    self.assertNotIn("JSValue toJS(", transformed)
                    self.assertNotIn("JSHTMLAllCollection.h", transformed)
                    self.assertEqual(transformed.count("static bool getHTMLCollectionOwnProperty("), 1)
                    self.assertNotIn("legacyPlatformObjectGetOwnProperty", transformed)
                    self.assertIn("bool JSHTMLCollection::getOwnPropertySlot(", transformed)
                    self.assertIn("bool JSHTMLCollection::getOwnPropertySlotByIndex(", transformed)
                    self.assertIn("bool JSHTMLCollection::put(", transformed)
                    self.assertIn("bool JSHTMLCollection::putByIndex(", transformed)
        for payload in SOURCES:
            self.assertTrue((ROOT / "scripts/wpe/adopted" / payload).is_file())

    def test_canonical_index_miss_falls_back_only_when_indexed_storage_exists(self):
        if not ARCHIVE.is_file():
            self.skipTest("The pinned WebKit source archive is required")
        path = "Source/WebCore/bindings/js/JSHTMLCollectionCustom.cpp"
        with tarfile.open(ARCHIVE) as package:
            original = package.extractfile(PREFIX + path).read().decode()
        transformed = render(ROOT, original, [PATCHES[path]], path)
        indexed = _braced_block(
            transformed,
            "bool JSHTMLCollection::getOwnPropertySlotByIndex(",
        )
        canonical = _braced_block(indexed, "if (index <= MAX_ARRAY_INDEX)")
        self.assertLess(
            canonical.index("if (!hasIndexedProperties(thisObject->indexingType()))"),
            canonical.index("return JSObject::getOwnPropertySlotByIndex("),
        )
        self.assertIn("return false;", canonical)
        self.assertIn("accessVisibleNamedProperty<LegacyOverrideBuiltIns::No>", indexed)
        self.assertIn("return JSObject::getOwnPropertySlotByIndex(object, lexicalGlobalObject, index, slot);", indexed)
        own_slot = _braced_block(
            transformed,
            "static bool getHTMLCollectionOwnProperty(",
        )
        numeric = _braced_block(own_slot, "if (auto index = parseIndex(propertyName))")
        self.assertLess(
            numeric.index("if (!hasIndexedProperties(thisObject->indexingType()))"),
            numeric.index("return WebCore::getOwnPropertySlot("),
        )
        self.assertLess(indexed.index("if (index <= MAX_ARRAY_INDEX)"), indexed.index("Identifier::from(vm, index)"))

    def test_custom_slot_generator_keeps_declarations_and_override_flags(self):
        if not ARCHIVE.is_file():
            self.skipTest("The pinned WebKit source archive is required")
        generator_path = "Source/WebCore/bindings/scripts/CodeGeneratorJS.pm"
        with tarfile.open(ARCHIVE) as package:
            generator = package.extractfile(PREFIX + generator_path).read().decode()
        name_slot = _subroutine(generator, "GenerateGetOwnPropertySlot", "GenerateGetOwnPropertySlotByIndex")
        index_slot = _subroutine(generator, "GenerateGetOwnPropertySlotByIndex", "GenerateGetOwnPropertyNames")
        put = _subroutine(generator, "GeneratePut", "GeneratePutByIndex")
        put_by_index = _subroutine(generator, "GeneratePutByIndex", "GenerateIsLegacyUnforgeablePropertyName")
        self.assertIn("return if $interface->extendedAttributes->{CustomGetOwnPropertySlot};", name_slot)
        self.assertIn("return if $interface->extendedAttributes->{CustomGetOwnPropertySlot};", index_slot)
        self.assertIn("return if $interface->extendedAttributes->{CustomPut};", put)
        self.assertIn("return if $interface->extendedAttributes->{CustomPut};", put_by_index)
        declarations = generator[generator.index("if (InstanceOverridesGetOwnPropertySlot($interface))", generator.index("sub GenerateHeader")):]
        declarations = declarations[:declarations.index("if (InstanceOverridesGetOwnPropertyNames($interface))")]
        put_declarations = generator[generator.index("if (InstanceOverridesPut($interface))", generator.index("sub GenerateHeader")):]
        put_declarations = put_declarations[:put_declarations.index("if (InstanceOverridesDefineOwnProperty($interface))")]
        self.assertIn("static bool getOwnPropertySlot(JSC::JSObject*", declarations)
        self.assertIn("static bool getOwnPropertySlotByIndex(JSC::JSObject*", declarations)
        self.assertIn("unless $interface->extendedAttributes->{CustomGetOwnPropertySlot};", declarations)
        self.assertIn("JSC::OverridesGetOwnPropertySlot", declarations)
        self.assertIn("JSC::InterceptsGetOwnPropertySlotByIndexEvenWhenLengthIsNotZero", declarations)
        self.assertIn("return $interface->extendedAttributes->{CustomPut}", generator)
        self.assertIn("static bool put(JSC::JSCell*", put_declarations)
        self.assertIn("static bool putByIndex(JSC::JSCell*", put_declarations)
        self.assertIn("JSC::OverridesPut", put_declarations)

    def test_named_getter_branch_matches_the_combined3_generated_body(self):
        generated = ROOT / "third_party/wpe/build-2.53.92-performance-combined3/WebCore/DerivedSources/JSHTMLCollection.cpp"
        if not generated.is_file() or not ARCHIVE.is_file():
            self.skipTest("The combined3 generated binding and pinned archive are required")
        path = "Source/WebCore/bindings/js/JSHTMLCollectionCustom.cpp"
        with tarfile.open(ARCHIVE) as package:
            original = package.extractfile(PREFIX + path).read().decode()
        custom = render(ROOT, original, [PATCHES[path]], path)
        baseline = render(ROOT, generated.read_text(), [], "JSHTMLCollection.cpp")
        custom_branch = _braced_block(custom, "if (!ignoreNamedProperties)")
        baseline_branch = _braced_block(baseline, "if (!ignoreNamedProperties)")
        normalize = lambda value: re.sub(r";{2,}", ";", re.sub(r"\s+", "", value))
        self.assertEqual(normalize(custom_branch), normalize(baseline_branch))
        custom_indexed = _braced_block(custom, "bool JSHTMLCollection::getOwnPropertySlotByIndex(")
        baseline_indexed = _braced_block(baseline, "bool JSHTMLCollection::getOwnPropertySlotByIndex(")
        boundary = "auto propertyName = Identifier::from(vm, index);"
        custom_tail = normalize(custom_indexed[custom_indexed.index(boundary):])
        scope = "autothrowScope=DECLARE_THROW_SCOPE(vm);"
        self.assertEqual(custom_tail.count(scope), 1)
        self.assertIn(scope, normalize(baseline_indexed[:baseline_indexed.index(boundary)]))
        self.assertEqual(
            custom_tail.replace(scope, ""),
            normalize(baseline_indexed[baseline_indexed.index(boundary):]),
        )
        for name in ("put", "putByIndex"):
            self.assertEqual(
                normalize(_braced_block(custom, "bool JSHTMLCollection::" + name + "(")),
                normalize(_braced_block(baseline, "bool JSHTMLCollection::" + name + "(")).replace(
                    "legacyPlatformObjectGetOwnProperty(", "getHTMLCollectionOwnProperty("
                ),
            )

    def test_runtime_fixture_covers_fallback_and_quirk_cases(self):
        fixture = (ROOT / "scripts/wpe/adopted/collection_index_test.js").read_text()
        for behavior in (
            "indexed wrapper identity",
            "maximum array index remains a named property",
            "ordinary indexed assignment creates no expando",
            "ordinary string-key indexed assignment creates no expando",
            "strict ordinary indexed assignment still throws",
            "indexed defineOwnProperty still throws",
            "prototype indexed getter remains visible",
            "maximum array index remains inherited",
            "HTMLFormControlsCollection indexed wrapper identity",
            "HTMLOptionsCollection indexed wrapper identity",
            "site quirk indexed expando remains readable",
            "site quirk string-key indexed expando remains readable",
            "quirk expando survives reads from a non-quirks realm",
            "site quirk does not alter indexed defineOwnProperty",
            "site quirk indexed expando remains deletable",
            "site quirk string-key indexed expando remains deletable",
        ):
            with self.subTest(behavior=behavior):
                self.assertIn(behavior, fixture)


if __name__ == "__main__":
    unittest.main()
