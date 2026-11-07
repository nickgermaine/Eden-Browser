from pathlib import Path

from wpe.patch_support import replace_once


GRAPHICS = "Source/WebCore/platform/graphics/"


def font_patch(text):
    return replace_once(
        text,
        "#if !USE(CORE_TEXT)\nGlyphBufferAdvance Font::applyTransforms",
        "#if !USE(CORE_TEXT) && !USE(SKIA)\nGlyphBufferAdvance Font::applyTransforms",
    )


def skia_font_patch(text):
    text = replace_once(text, '#include "config.h"', '#include "config.h"\n#include <cstdlib>\n#include <cstring>')
    text = replace_once(
        text,
        '#include "GlyphBuffer.h"',
        '#include "FontCascadeInlines.h"\n#include "GlyphBuffer.h"\n#include "HbUniquePtr.h"\n#include "SurrogatePairAwareTextIterator.h"',
    )
    text = replace_once(
        text,
        "#include <wtf/StdLibExtras.h>",
        "#include <algorithm>\n#include <array>\n#include <limits>\n#include <span>\n#include <hb-icu.h>\n#include <hb.h>\n#include <unicode/uscript.h>\n#include <wtf/MainThread.h>\n#include <wtf/NeverDestroyed.h>\n#include <wtf/StdLibExtras.h>\n#include <wtf/text/AtomString.h>",
    )
    implementation = Path(__file__).with_name("simple_text_transforms.inc").read_text()
    return replace_once(text, "Path Font::platformPathForGlyph", implementation + "\nPath Font::platformPathForGlyph")


def platform_data_header_patch(text):
    text = replace_once(
        text,
        "    const Vector<hb_feature_t>& features() const LIFETIME_BOUND { return m_features; }",
        "    const Vector<hb_feature_t>& features() const LIFETIME_BOUND { return m_features; }\n    bool simpleTextNeedsShaping(bool enableKerning) const;\n    const Vector<hb_feature_t>& simpleTextFeatures(bool enableKerning) const;",
    )
    return replace_once(
        text,
        "    RefPtr<SkiaHarfBuzzFont> m_hbFont;\n    Vector<hb_feature_t> m_features;\n#endif",
        "    RefPtr<SkiaHarfBuzzFont> m_hbFont;\n    Vector<hb_feature_t> m_features;\n    uint8_t m_simpleTextShapingFlags { 0 };\n    Vector<hb_feature_t> m_simpleTextFeaturesWithoutKerning;\n#endif",
    )


def platform_data_patch(text):
    text = replace_once(text, "#include <wtf/Hasher.h>", "#include <algorithm>\n#include <array>\n#include <span>\n#include <hb-ot.h>\n#include <wtf/Hasher.h>")
    text = replace_once(
        text,
        "void FontPlatformData::platformDataInit()\n{",
        """static bool edenTableHasAnyFeature(hb_face_t* face, hb_tag_t table, std::span<const hb_tag_t> wanted)
{
    unsigned offset = 0;
    while (true) {
        std::array<hb_tag_t, 64> tags;
        unsigned count = tags.size();
        hb_ot_layout_table_get_feature_tags(face, table, offset, &count, tags.data());
        if (!count)
            return false;
        for (unsigned i = 0; i < count; ++i) {
            if (std::ranges::find(wanted, tags[i]) != wanted.end())
                return true;
        }
        offset += count;
    }
}

static bool edenFaceHasTable(hb_face_t* face, hb_tag_t tag)
{
    hb_blob_t* blob = hb_face_reference_table(face, tag);
    bool present = hb_blob_get_length(blob);
    hb_blob_destroy(blob);
    return present;
}

static uint8_t edenComputeSimpleTextShapingFlags(hb_face_t* face)
{
    static constexpr uint8_t substitution = 1;
    static constexpr uint8_t kernPositioning = 2;
    static constexpr uint8_t otherPositioning = 4;
    static constexpr std::array substitutionTags {
        HB_TAG('r', 'v', 'r', 'n'), HB_TAG('l', 't', 'r', 'a'), HB_TAG('l', 't', 'r', 'm'), HB_TAG('r', 't', 'l', 'a'),
        HB_TAG('r', 't', 'l', 'm'), HB_TAG('f', 'r', 'a', 'c'), HB_TAG('n', 'u', 'm', 'r'), HB_TAG('d', 'n', 'o', 'm'),
        HB_TAG('r', 'a', 'n', 'd'), HB_TAG('c', 'c', 'm', 'p'), HB_TAG('l', 'o', 'c', 'l'), HB_TAG('r', 'l', 'i', 'g'),
        HB_TAG('c', 'a', 'l', 't'), HB_TAG('c', 'l', 'i', 'g'), HB_TAG('l', 'i', 'g', 'a'), HB_TAG('r', 'c', 'l', 't'),
        HB_TAG('H', 'a', 'r', 'f'), HB_TAG('H', 'A', 'R', 'F'), HB_TAG('B', 'u', 'z', 'z'), HB_TAG('B', 'U', 'Z', 'Z'),
    };
    static constexpr std::array kernTags { HB_TAG('k', 'e', 'r', 'n') };
    static constexpr std::array otherPositioningTags {
        HB_TAG('m', 'a', 'r', 'k'), HB_TAG('m', 'k', 'm', 'k'), HB_TAG('c', 'u', 'r', 's'), HB_TAG('d', 'i', 's', 't'),
        HB_TAG('a', 'b', 'v', 'm'), HB_TAG('b', 'l', 'w', 'm'), HB_TAG('H', 'a', 'r', 'f'), HB_TAG('H', 'A', 'R', 'F'),
        HB_TAG('B', 'u', 'z', 'z'), HB_TAG('B', 'U', 'Z', 'Z'),
    };
    uint8_t flags = 0;
    if (edenFaceHasTable(face, HB_TAG('m', 'o', 'r', 'x')) || edenFaceHasTable(face, HB_TAG('m', 'o', 'r', 't'))
        || (hb_ot_layout_has_substitution(face) && edenTableHasAnyFeature(face, HB_OT_TAG_GSUB, substitutionTags)))
        flags |= substitution;
    if (edenFaceHasTable(face, HB_TAG('k', 'e', 'r', 'n')) || edenFaceHasTable(face, HB_TAG('k', 'e', 'r', 'x'))
        || (hb_ot_layout_has_positioning(face) && edenTableHasAnyFeature(face, HB_OT_TAG_GPOS, kernTags)))
        flags |= kernPositioning;
    if (edenFaceHasTable(face, HB_TAG('t', 'r', 'a', 'k'))
        || (hb_ot_layout_has_positioning(face) && edenTableHasAnyFeature(face, HB_OT_TAG_GPOS, otherPositioningTags)))
        flags |= otherPositioning;
    return flags;
}

void FontPlatformData::platformDataInit()
{""",
    )
    text = replace_once(
        text,
        "    m_isColorBitmapFont = m_hbFont->isColorBitmapFont();\n}",
        """    m_isColorBitmapFont = m_hbFont->isColorBitmapFont();
    m_simpleTextShapingFlags = edenComputeSimpleTextShapingFlags(hb_font_get_face(hbFont()));
    m_simpleTextFeaturesWithoutKerning.clear();
    if (!m_features.isEmpty()) {
        m_simpleTextFeaturesWithoutKerning.append({ HB_TAG('k', 'e', 'r', 'n'), 0, 0, static_cast<unsigned>(-1) });
        m_simpleTextFeaturesWithoutKerning.appendVector(m_features);
    }
}""",
    )
    return replace_once(
        text,
        "hb_font_t* FontPlatformData::hbFont() const\n{",
        """const Vector<hb_feature_t>& FontPlatformData::simpleTextFeatures(bool enableKerning) const
{
    static const Vector<hb_feature_t> featuresWithKerningDisabled = [] {
        Vector<hb_feature_t> features;
        features.append({ HB_TAG('k', 'e', 'r', 'n'), 0, 0, static_cast<unsigned>(-1) });
        return features;
    }();
    if (enableKerning)
        return m_features;
    return m_simpleTextFeaturesWithoutKerning.isEmpty() ? featuresWithKerningDisabled : m_simpleTextFeaturesWithoutKerning;
}

bool FontPlatformData::simpleTextNeedsShaping(bool enableKerning) const
{
    static constexpr uint8_t substitution = 1;
    static constexpr uint8_t kernPositioning = 2;
    static constexpr uint8_t otherPositioning = 4;
    if (!m_features.isEmpty())
        return true;
    if (m_simpleTextShapingFlags & (substitution | otherPositioning))
        return true;
    return enableKerning && (m_simpleTextShapingFlags & kernPositioning);
}

hb_font_t* FontPlatformData::hbFont() const
{""",
    )


def cascade_patch(text):
    text = replace_once(
        text,
        '#include "config.h"',
        '''#include "config.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unicode/uchar.h>

#if USE(SKIA)
static bool edenSimpleTextTransformsEnabled()
{
    static bool enabled = [] {
        const char* value = std::getenv("WEBKIT_EDEN_DISABLE_SIMPLE_TEXT");
        return !value || std::strcmp(value, "1");
    }();
    return enabled;
}
#endif''',
    )
    text = replace_once(
        text,
        """    auto result = 0.f;
    for (size_t i = 0; i < glyphBuffer.size(); ++i)
        result += WebCore::width(glyphBuffer.advanceAt(i));
    result += WebCore::width(initialAdvance);
#endif
    if (cacheEntry)""",
        """    auto result = 0.f;
#if USE(SKIA)
    float spaceWidth = font->spaceWidth(Font::SyntheticBoldInclusion::Exclude);
    for (size_t i = 0; i < glyphBuffer.size(); ++i)
        result += treatAsSpace(text[glyphBuffer.uncheckedStringOffsetAt(i)]) ? spaceWidth : WebCore::width(glyphBuffer.advanceAt(i));
#else
    for (size_t i = 0; i < glyphBuffer.size(); ++i)
        result += WebCore::width(glyphBuffer.advanceAt(i));
#endif
    result += WebCore::width(initialAdvance);
#ifdef WEBKIT_EDEN_VERIFY_PATCHES
    {
        TextRun verificationRun { text, 0, 0, ExpansionBehavior::defaultBehavior(), textDirection, false, false };
        ComplexTextController controller(*this, verificationRun, true, nullptr);
        float expected = controller.totalAdvance().width();
        if (std::abs(expected - result) > 0.01f) {
            static unsigned reports = 0;
            if (reports++ < 200)
                WTFLogAlways("Eden simple text: width mismatch simple=%f complex=%f length=%u text=%s", result, expected, text.length(), text.utf8().data());
            RELEASE_ASSERT_NOT_REACHED();
        }
    }
#endif
#endif
    if (cacheEntry)""",
    )
    text = replace_once(
        text,
        """    if (fontDescription().variantCaps() != FontVariantCaps::Normal || fontDescription().variantEmoji() == FontVariantEmoji::Emoji)
        return true;

    return enableKerning() || requiresShaping();""",
        """    if (fontDescription().variantCaps() != FontVariantCaps::Normal || fontDescription().variantEmoji() == FontVariantEmoji::Emoji)
        return true;

#if USE(SKIA)
    if (edenSimpleTextTransformsEnabled() && fontDescription().orientation() == FontOrientation::Horizontal)
        return false;
#endif

    return enableKerning() || requiresShaping();""",
    )
    text = replace_once(
        text,
        "FontCascade::CodePath FontCascade::codePath(const TextRun& run, std::optional<unsigned> from, std::optional<unsigned> to) const\n{",
        """static bool edenSimpleTextHasOppositeBidiRun(const TextRun& run)
{
    bool rightToLeft = run.rtl();
    if (run.is8Bit()) {
        if (!rightToLeft)
            return false;
        for (auto character : run.span8()) {
            if (isASCIIAlpha(character) || isASCIIDigit(character))
                return true;
        }
        return false;
    }

    auto characters = run.span16();
    for (size_t index = 0; index < characters.size(); ++index) {
        UChar32 character = characters[index];
        if (U16_IS_LEAD(characters[index]) && index + 1 < characters.size() && U16_IS_TRAIL(characters[index + 1])) {
            auto lead = characters[index];
            ++index;
            character = U16_GET_SUPPLEMENTARY(lead, characters[index]);
        }
        if ((character >= 0x202a && character <= 0x202e) || (character >= 0x2066 && character <= 0x2069))
            return true;
        auto bidiClass = u_charDirection(character);
        if (rightToLeft) {
            if (bidiClass == U_LEFT_TO_RIGHT || bidiClass == U_EUROPEAN_NUMBER || bidiClass == U_ARABIC_NUMBER)
                return true;
        } else if (bidiClass == U_RIGHT_TO_LEFT || bidiClass == U_RIGHT_TO_LEFT_ARABIC)
            return true;
    }
    return false;
}

FontCascade::CodePath FontCascade::codePath(const TextRun& run, std::optional<unsigned> from, std::optional<unsigned> to) const
{""",
    )
    text = replace_once(
        text,
        """    if (s_forcedCodePath)
        return *s_forcedCodePath;

    if (!canHandleRunAsSimpleText(run, from.value_or(0), to.value_or(run.length())))""",
        """    if (s_forcedCodePath)
        return *s_forcedCodePath;

#if USE(SKIA)
    if (edenSimpleTextTransformsEnabled() && fontDescription().orientation() == FontOrientation::Horizontal
        && edenSimpleTextHasOppositeBidiRun(run))
        return CodePath::Complex;
#endif

    if (!canHandleRunAsSimpleText(run, from.value_or(0), to.value_or(run.length())))""",
    )
    text = replace_once(
        text,
        """    if (shouldUseComplexTextController(codePathToUse))
        return layoutComplexText(run, from, to, forTextEmphasis);

    return layoutSimpleText(run, from, to, forTextEmphasis);""",
        """#if USE(SKIA)
    if (forTextEmphasis == ForTextEmphasis::Yes && edenSimpleTextTransformsEnabled())
        return layoutComplexText(run, from, to, forTextEmphasis);
#endif

    if (shouldUseComplexTextController(codePathToUse))
        return layoutComplexText(run, from, to, forTextEmphasis);

    return layoutSimpleText(run, from, to, forTextEmphasis);""",
    )
    return replace_once(
        text,
        """    if (run.rtl())
        result.glyphBuffer.reverse(0, result.glyphBuffer.size());

    return result;""",
        """    if (run.rtl())
        result.glyphBuffer.reverse(0, result.glyphBuffer.size());

#if defined(WEBKIT_EDEN_VERIFY_PATCHES) && USE(SKIA)
    if (!from && to == run.length()) {
        auto expected = layoutComplexText(run, from, to, forTextEmphasis);
        auto flattenedInitialX = [](const GlyphBuffer& buffer) {
            return WebCore::width(buffer.initialAdvance()) + (buffer.isEmpty() ? 0 : WebCore::x(buffer.originAt(0)));
        };
        auto flattenedInitialY = [](const GlyphBuffer& buffer) {
            return WebCore::height(buffer.initialAdvance()) + (buffer.isEmpty() ? 0 : WebCore::y(buffer.originAt(0)));
        };
        auto flattenedAdvanceX = [](const GlyphBuffer& buffer, unsigned index) {
            return -WebCore::x(buffer.originAt(index)) + WebCore::width(buffer.advanceAt(index))
                + (index + 1 < buffer.size() ? WebCore::x(buffer.originAt(index + 1)) : 0);
        };
        auto flattenedAdvanceY = [](const GlyphBuffer& buffer, unsigned index) {
            return -WebCore::y(buffer.originAt(index)) + WebCore::height(buffer.advanceAt(index))
                + (index + 1 < buffer.size() ? WebCore::y(buffer.originAt(index + 1)) : 0);
        };
        bool matches = expected.glyphBuffer.size() == result.glyphBuffer.size()
            && std::abs(expected.width - result.width) <= 0.01f
            && std::abs(flattenedInitialX(expected.glyphBuffer) - flattenedInitialX(result.glyphBuffer)) <= 0.01f
            && std::abs(flattenedInitialY(expected.glyphBuffer) - flattenedInitialY(result.glyphBuffer)) <= 0.01f;
        for (unsigned i = 0; matches && i < result.glyphBuffer.size(); ++i) {
            matches = expected.glyphBuffer.glyphAt(i) == result.glyphBuffer.glyphAt(i)
                && &expected.glyphBuffer.fontAt(i) == &result.glyphBuffer.fontAt(i)
                && expected.glyphBuffer.uncheckedStringOffsetAt(i) == result.glyphBuffer.uncheckedStringOffsetAt(i)
                && std::abs(flattenedAdvanceX(expected.glyphBuffer, i) - flattenedAdvanceX(result.glyphBuffer, i)) <= 0.01f
                && std::abs(flattenedAdvanceY(expected.glyphBuffer, i) - flattenedAdvanceY(result.glyphBuffer, i)) <= 0.01f;
        }
        if (!matches) {
            static unsigned reports = 0;
            if (reports++ < 200)
                WTFLogAlways("Eden simple text: layout mismatch glyphs=%u/%u width=%f/%f rtl=%d text=%s", result.glyphBuffer.size(), expected.glyphBuffer.size(), result.width, expected.width, run.rtl(), run.text().utf8().data());
            RELEASE_ASSERT_NOT_REACHED();
        }
    }
#endif

    return result;""",
    )


def measurement_cache_patch(text):
    text = replace_once(
        text,
        "static constexpr unsigned maxTextLength = 64;",
        "static constexpr unsigned maxTextLength = 128;",
    )
    return replace_once(
        text,
        "RapidHash::computeHashAndMaskTop8Bits(std::span<const char16_t> { m_characters }.first(s_capacity)) | (length << 24);",
        "RapidHash::computeHashAndMaskTop8Bits(std::span<const char16_t> { m_characters }.first(length)) | (length << 24);",
    )


PATCHES = {
    GRAPHICS + "Font.cpp": font_patch,
    GRAPHICS + "FontCascade.cpp": cascade_patch,
    GRAPHICS + "FontPlatformData.h": platform_data_header_patch,
    GRAPHICS + "TextMeasurementCache.h": measurement_cache_patch,
    GRAPHICS + "skia/FontPlatformDataSkia.cpp": platform_data_patch,
    GRAPHICS + "skia/FontSkia.cpp": skia_font_patch,
}
SOURCES = ["simple_text_transforms.inc"]
