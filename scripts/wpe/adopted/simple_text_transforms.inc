static bool edenSimpleTextTransformsEnabled() {
    static bool enabled = [] {
        const char *value = std::getenv("WEBKIT_EDEN_DISABLE_SIMPLE_TEXT");
        return !value || std::strcmp(value, "1");
    }();
    return enabled;
}

static hb_language_t edenLanguageForLocale(const AtomString &locale) {
    struct Entry {
        String locale;
        hb_language_t language{HB_LANGUAGE_INVALID};
    };
    static thread_local std::array<Entry, 8> entries;
    static thread_local unsigned next = 0;
    for (auto &entry : entries) {
        if (entry.language != HB_LANGUAGE_INVALID && entry.locale == locale.string()) {
            return entry.language;
        }
    }
    auto language = hb_language_from_string(locale.string().utf8().data(), -1);
    if (locale.length() <= 128) {
        entries[next] = {locale.string().isolatedCopy(), language};
        next = (next + 1) % entries.size();
    }
    return language;
}

struct EdenSimpleScriptRun {
    unsigned start;
    unsigned end;
    UScriptCode script;
};

static std::optional<UScriptCode> edenCharacterScript(char32_t character) {
    UErrorCode errorCode = U_ZERO_ERROR;
    UScriptCode script = uscript_getScript(character, &errorCode);
    if (U_FAILURE(errorCode)) {
        return std::nullopt;
    }
    return script;
}

static bool edenIsLatinBelowIPA(char16_t character) {
    if (character < 0x80) {
        return isASCIIAlpha(character);
    }
    if (character < 0x100) {
        return character == 0xaa || character == 0xba || (character >= 0xc0 && character != 0xd7 && character != 0xf7);
    }
    return true;
}

static std::optional<EdenSimpleScriptRun> edenNextScriptRun(std::span<const char16_t> characters, unsigned offset) {
    SurrogatePairAwareTextIterator textIterator(characters.subspan(offset), offset, characters.size());
    char32_t character;
    unsigned clusterLength = 0;
    if (!textIterator.consume(character, clusterLength)) {
        return std::nullopt;
    }

    auto currentScript = edenCharacterScript(character);
    if (!currentScript) {
        return std::nullopt;
    }

    unsigned startIndex = offset;
    for (textIterator.advance(clusterLength); textIterator.consume(character, clusterLength);
         textIterator.advance(clusterLength)) {
        if (FontCascade::treatAsZeroWidthSpace(character)) {
            continue;
        }

        auto nextScript = edenCharacterScript(character);
        if (!nextScript) {
            return std::nullopt;
        }

        if (nextScript == USCRIPT_INHERITED || nextScript == USCRIPT_COMMON) {
            continue;
        }
        if (currentScript == USCRIPT_INHERITED || currentScript == USCRIPT_COMMON) {
            currentScript = nextScript;
            continue;
        }

        if (currentScript != nextScript && !uscript_hasScript(character, currentScript.value())) {
            return EdenSimpleScriptRun{startIndex, textIterator.currentIndex(), currentScript.value()};
        }
    }

    return EdenSimpleScriptRun{startIndex, textIterator.currentIndex(), currentScript.value()};
}

static bool edenCharactersAreShapingNeutral(StringView text) {
    if (text.is8Bit()) {
        for (auto character : text.span8()) {
            if (character == 0xad) {
                return false;
            }
        }
        return true;
    }
    for (auto character : text.span16()) {
        if (character >= 0x1100 || character == 0xad || character == 0x34f || character == 0x61c) {
            return false;
        }
    }
    return true;
}

GlyphBufferAdvance Font::applyTransforms(
    GlyphBuffer &glyphBuffer,
    unsigned beginningGlyphIndex,
    unsigned beginningStringIndex,
    bool enableKerning,
    bool requiresShaping,
    const AtomString &locale,
    StringView text,
    TextDirection direction
) const {
    if (!edenSimpleTextTransformsEnabled() || beginningGlyphIndex >= glyphBuffer.size() || !platformData().size()) {
        return makeGlyphBufferAdvance();
    }
    if (!enableKerning && !requiresShaping && !platformData().simpleTextNeedsShaping(enableKerning)) {
        return makeGlyphBufferAdvance();
    }

    auto lastStringIndex = static_cast<unsigned>(glyphBuffer.uncheckedStringOffsetAt(glyphBuffer.size() - 1));
    if (lastStringIndex >= text.length() || text.length() > std::numeric_limits<unsigned>::max()) {
        return makeGlyphBufferAdvance();
    }
    unsigned end = lastStringIndex + 1;
    if (end < text.length() && U16_IS_LEAD(text[end - 1]) && U16_IS_TRAIL(text[end])) {
        ++end;
    }
    if (beginningStringIndex >= end || end > text.length()) {
        return makeGlyphBufferAdvance();
    }

    auto substring = text.substring(beginningStringIndex, end - beginningStringIndex);

    if (!platformData().simpleTextNeedsShaping(enableKerning) && edenCharactersAreShapingNeutral(substring)) {
        auto advances = glyphBuffer.advances(beginningGlyphIndex);
        for (auto &advance : advances) {
            WebCore::setWidth(advance, static_cast<float>(clampTo<int>(WebCore::width(advance) * 65536.0f)) / 65536.0f);
        }
        return makeGlyphBufferAdvance();
    }

    auto inputGlyphs = glyphBuffer.glyphs(beginningGlyphIndex);
    if (std::ranges::find(inputGlyphs, deletedGlyph) != inputGlyphs.end()) {
        return makeGlyphBufferAdvance();
    }

    auto upconverted = substring.upconvertedCharacters<256>();
    std::span<const char16_t> characters = upconverted.span();
    if (characters.empty() || characters.size() > std::numeric_limits<unsigned>::max() ||
        characters.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return makeGlyphBufferAdvance();
    }

    Vector<EdenSimpleScriptRun, 1> runs;
    bool belowIPA = true;
    bool hasLatin = false;
    for (auto character : characters) {
        if (character >= 0x250) {
            belowIPA = false;
            break;
        }
        hasLatin |= edenIsLatinBelowIPA(character);
    }
    if (belowIPA) {
        runs.append({0, static_cast<unsigned>(characters.size()), hasLatin ? USCRIPT_LATIN : USCRIPT_COMMON});
    } else {
        unsigned offset = 0;
        while (offset < characters.size()) {
            auto run = edenNextScriptRun(characters, offset);
            if (!run || run->start != offset || run->end <= offset) {
                return makeGlyphBufferAdvance();
            }
            runs.append(*run);
            offset = run->end;
        }
    }

    if (runs.isEmpty() || runs.last().end != characters.size()) {
        return makeGlyphBufferAdvance();
    }

    auto *hbFont = platformData().hbFont();
    if (!hbFont) {
        return makeGlyphBufferAdvance();
    }
    auto featureSpan = platformData().simpleTextFeatures(enableKerning).span();

    static thread_local HbUniquePtr<hb_buffer_t> buffer(hb_buffer_create());
    if (!buffer) {
        return makeGlyphBufferAdvance();
    }
    auto language = edenLanguageForLocale(locale);
    bool rtl = direction == TextDirection::RTL;
    struct ShapedGlyph {
        Glyph glyph;
        unsigned stringOffset;
        GlyphBufferAdvance advance;
        GlyphBufferOrigin origin;
    };
    Vector<ShapedGlyph, 16> shapedGlyphs;

    for (auto &run : runs) {
        hb_buffer_reset(buffer.get());
        hb_buffer_set_cluster_level(buffer.get(), HB_BUFFER_CLUSTER_LEVEL_CHARACTERS);
        hb_buffer_set_flags(
            buffer.get(),
            static_cast<hb_buffer_flags_t>(
                (run.start ? 0 : HB_BUFFER_FLAG_BOT) | (run.end == characters.size() ? HB_BUFFER_FLAG_EOT : 0)
            )
        );
        hb_buffer_set_language(buffer.get(), language);
        hb_buffer_set_script(buffer.get(), hb_icu_script_to_script(run.script));
        hb_buffer_set_direction(buffer.get(), rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
        hb_buffer_add_utf16(
            buffer.get(),
            reinterpret_cast<const uint16_t *>(characters.data()),
            characters.size(),
            run.start,
            run.end - run.start
        );
        if (!hb_buffer_allocation_successful(buffer.get())) {
            return makeGlyphBufferAdvance();
        }
        hb_shape(hbFont, buffer.get(), featureSpan.empty() ? nullptr : featureSpan.data(), featureSpan.size());
        if (!hb_buffer_allocation_successful(buffer.get())) {
            return makeGlyphBufferAdvance();
        }

        unsigned count = hb_buffer_get_length(buffer.get());
        if (shapedGlyphs.size() > std::numeric_limits<unsigned>::max() - count) {
            return makeGlyphBufferAdvance();
        }
        if (!count) {
            continue;
        }
        unsigned infoCount = 0;
        unsigned positionCount = 0;
        auto *infos = hb_buffer_get_glyph_infos(buffer.get(), &infoCount);
        auto *positions = hb_buffer_get_glyph_positions(buffer.get(), &positionCount);
        if (!infos || !positions || infoCount != count || positionCount != count) {
            return makeGlyphBufferAdvance();
        }
        for (unsigned i = 0; i < count; ++i) {
            unsigned source = rtl ? count - 1 - i : i;
            auto cluster = infos[source].cluster;
            if (cluster < run.start || cluster >= run.end || cluster > text.length() - beginningStringIndex) {
                return makeGlyphBufferAdvance();
            }
            Glyph glyph = infos[source].codepoint;
            auto advance =
                makeGlyphBufferAdvance(positions[source].x_advance / 65536.0f, positions[source].y_advance / 65536.0f);
            auto origin = isZeroWidthSpaceGlyph(glyph)
                              ? makeGlyphBufferOrigin()
                              : makeGlyphBufferOrigin(
                                    {positions[source].x_offset / 65536.0f, positions[source].y_offset / 65536.0f}
                                );
            if (isZeroWidthSpaceGlyph(glyph)) {
                advance = makeGlyphBufferAdvance();
            }
            shapedGlyphs.append({glyph, beginningStringIndex + cluster, advance, origin});
        }
    }

    if (shapedGlyphs.size() > std::numeric_limits<unsigned>::max() - beginningGlyphIndex) {
        return makeGlyphBufferAdvance();
    }
    auto newEnd = beginningGlyphIndex + shapedGlyphs.size();
    if (newEnd > glyphBuffer.size()) {
        glyphBuffer.makeHole(glyphBuffer.size(), newEnd - glyphBuffer.size(), this);
    }
    auto glyphs = glyphBuffer.glyphs(beginningGlyphIndex);
    auto advances = glyphBuffer.advances(beginningGlyphIndex);
    auto origins = glyphBuffer.origins(beginningGlyphIndex);
    auto offsets = glyphBuffer.offsetsInString(beginningGlyphIndex);
    for (size_t i = 0; i < shapedGlyphs.size(); ++i) {
        glyphs[i] = shapedGlyphs[i].glyph;
        advances[i] = shapedGlyphs[i].advance;
        origins[i] = shapedGlyphs[i].origin;
        offsets[i] = shapedGlyphs[i].stringOffset;
    }
    if (newEnd < glyphBuffer.size()) {
        glyphBuffer.shrink(newEnd);
    }
    return makeGlyphBufferAdvance();
}
