from pathlib import Path

from wpe.patch_support import replace_once


HEADER = "Source/WebCore/css/SelectorFilter.h"
SOURCE = "Source/WebCore/css/SelectorFilter.cpp"
HEADERS_LIST = "Source/WebCore/Headers.cmake"
TEST = "Tools/TestWebKitAPI/Tests/WebCore/CachedMatchFinder.cpp"
PARENT_FRAME_INLINE_HASHES = 10


def selector_filter_header_patch(text):
    if "parentFrameInlineHashCapacity" in text:
        raise RuntimeError("The selector filter parent frame capacity is already present")
    text = replace_once(
        text,
        "    struct ParentStackFrame {\n",
        f"    static constexpr size_t parentFrameInlineHashCapacity = {PARENT_FRAME_INLINE_HASHES};\n\n"
        "    struct ParentStackFrame {\n",
    )
    return replace_once(
        text,
        "        Element* element;\n        Vector<unsigned, 4> identifierHashes;\n",
        "        Element* element;\n        Vector<unsigned, parentFrameInlineHashCapacity> identifierHashes;\n",
    )


def selector_filter_source_patch(text):
    if "collectIdentifierHashesInto(" in text:
        raise RuntimeError("The selector filter hash collection helper is already present")
    before = "void SelectorFilter::collectElementIdentifierHashes(const Element& element, Vector<unsigned, 4>& identifierHashes)\n{\n"
    start = text.index(before)
    body_start = start + len(before)
    body_end = text.index("\n}\n", body_start)
    body = text[body_start:body_end]
    helper = (
        "template<size_t inlineCapacity>\n"
        "static void collectIdentifierHashesInto(const Element& element, Vector<unsigned, inlineCapacity>& identifierHashes)\n{\n"
        + body
        + "\n}\n\n"
        + before
        + "    collectIdentifierHashesInto(element, identifierHashes);\n}\n"
    )
    text = text[:start] + helper + text[body_end + len("\n}\n"):]
    return replace_once(
        text,
        "    collectElementIdentifierHashes(*parent, parentFrame.identifierHashes);\n",
        "    collectIdentifierHashesInto(*parent, parentFrame.identifierHashes);\n",
    )


def headers_list_patch(text):
    if "    css/SelectorFilter.h\n" in text:
        raise RuntimeError("The selector filter header is already exported")
    return replace_once(text, "    css/ShorthandSerializer.h\n", "    css/SelectorFilter.h\n    css/ShorthandSerializer.h\n")


def selector_filter_test_patch(text):
    if "TEST(SelectorFilterStorage," in text:
        raise RuntimeError("The selector filter storage regression tests are already present")
    text = replace_once(
        text,
        '#include "TestPageHarness.h"',
        '#include "TestPageHarness.h"\n'
        '#include <WebCore/CSSParserContext.h>\n'
        '#include <WebCore/CSSSelectorList.h>\n'
        '#include <WebCore/SelectorFilter.h>\n'
        '#include <WebCore/StyleRule.h>\n'
        '#include <WebCore/StyleSheetContents.h>\n'
        '#include <wtf/text/StringBuilder.h>',
    )
    tests = Path(__file__).with_name("selector_filter_storage_tests.inc").read_text()
    return replace_once(text, "TEST(CachedMatchFinder, CountMatchesFindsMatches)", tests + "\nTEST(CachedMatchFinder, CountMatchesFindsMatches)")


PATCHES = {
    HEADER: selector_filter_header_patch,
    SOURCE: selector_filter_source_patch,
    HEADERS_LIST: headers_list_patch,
    TEST: selector_filter_test_patch,
}

SOURCES = ["selector_filter_storage_tests.inc"]
