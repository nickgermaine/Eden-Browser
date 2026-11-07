from pathlib import Path

from wpe.patch_support import replace_once


def cache_patch(text):
    text = replace_once(
        text,
        "#include <WebCore/TextRun.h>",
        "#include <algorithm>\n#include <cstdlib>\n#include <cstring>\n#include <WebCore/TextRun.h>",
    )
    text = replace_once(
        text,
        "namespace TextMeasurementCacheDefaults {\n",
        """namespace TextMeasurementCacheDefaults {
inline bool edenLengthBoundedKeysEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("WEBKIT_EDEN_TEXT_CACHE_KEYS");
        return !value || std::strcmp(value, "0");
    }();
    return enabled;
}

""",
    )
    return replace_once(
        text,
        "        friend bool operator==(const SmallStringKey&, const SmallStringKey&) = default;",
        """        friend bool operator==(const SmallStringKey& left, const SmallStringKey& right)
        {
            if constexpr (s_capacity > 255)
                return left.m_characters == right.m_characters && left.m_hashAndLength == right.m_hashAndLength;
            if (!TextMeasurementCacheDefaults::edenLengthBoundedKeysEnabled())
                return left.m_characters == right.m_characters && left.m_hashAndLength == right.m_hashAndLength;
            if (left.m_hashAndLength != right.m_hashAndLength)
                return false;
            bool result = std::ranges::equal(std::span { left.m_characters }.first(left.length()), std::span { right.m_characters }.first(right.length()));
#ifdef WEBKIT_EDEN_VERIFY_PATCHES
            RELEASE_ASSERT(result == (left.m_characters == right.m_characters));
#endif
            return result;
        }""",
    )


def cache_test_patch(text):
    if "EdenTextMeasurementCache" in text:
        raise RuntimeError("The text measurement cache regressions are already present")
    text = replace_once(
        text,
        "#include <WebCore/TextRun.h>",
        "#include <WebCore/TextMeasurementCache.h>\n#include <WebCore/TextRun.h>\n#include <array>",
    )
    anchor = "TEST_F(ComplexTextControllerTest, InitialAdvanceWithLeftRunInRTL)"
    tests = Path(__file__).with_name("text_cache_keys_tests.inc").read_text()
    return replace_once(text, anchor, tests + "\n" + anchor)


PATCHES = {
    "Source/WebCore/platform/graphics/TextMeasurementCache.h": cache_patch,
    "Tools/TestWebKitAPI/Tests/WebCore/ComplexTextController.cpp": cache_test_patch,
}
SOURCES = ["text_cache_keys_tests.inc"]
