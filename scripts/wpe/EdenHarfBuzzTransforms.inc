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
    if ((!enableKerning && !requiresShaping) || beginningGlyphIndex == glyphBuffer.size() || !platformData().size()) {
        return makeGlyphBufferAdvance();
    }
    auto inputCount = glyphBuffer.size() - beginningGlyphIndex;
    auto end = static_cast<unsigned>(glyphBuffer.uncheckedStringOffsetAt(glyphBuffer.size() - 1)) + 1;
    RELEASE_ASSERT(beginningStringIndex <= end && end <= text.length());
    auto characters = text.substring(beginningStringIndex, end - beginningStringIndex).upconvertedCharacters<256>();
    static thread_local HbUniquePtr<hb_buffer_t> buffer(hb_buffer_create());
    hb_buffer_reset(buffer.get());
    hb_buffer_set_cluster_level(buffer.get(), HB_BUFFER_CLUSTER_LEVEL_CHARACTERS);
    hb_buffer_set_script(buffer.get(), HB_SCRIPT_LATIN);
    hb_buffer_set_language(buffer.get(), hb_language_from_string(locale.string().utf8().data(), -1));
    hb_buffer_set_direction(buffer.get(), direction == TextDirection::LTR ? HB_DIRECTION_LTR : HB_DIRECTION_RTL);
    hb_buffer_add_utf16(
        buffer.get(),
        reinterpret_cast<const uint16_t *>(characters.get()),
        characters.span().size(),
        0,
        characters.span().size()
    );
    auto &features = platformData().features();
    Vector<hb_feature_t, 8> overrides;
    auto featureSpan = features.span();
    if (!enableKerning) {
        overrides.append({HB_TAG('k', 'e', 'r', 'n'), 0, 0, HB_FEATURE_GLOBAL_END});
        overrides.appendVector(features);
        featureSpan = overrides.span();
    }
    hb_shape(platformData().hbFont(), buffer.get(), featureSpan.data(), featureSpan.size());
    RELEASE_ASSERT(hb_buffer_allocation_successful(buffer.get()));
    auto outputCount = hb_buffer_get_length(buffer.get());
    auto infos = unsafeMakeSpan(hb_buffer_get_glyph_infos(buffer.get(), nullptr), outputCount);
    auto positions = unsafeMakeSpan(hb_buffer_get_glyph_positions(buffer.get(), nullptr), outputCount);
    if (outputCount < inputCount) {
        glyphBuffer.remove(beginningGlyphIndex + outputCount, inputCount - outputCount);
    } else if (outputCount > inputCount) {
        glyphBuffer.makeHole(beginningGlyphIndex + inputCount, outputCount - inputCount, this);
    }
    for (unsigned i = 0; i < outputCount; ++i) {
        auto target = beginningGlyphIndex + i;
        glyphBuffer.glyphs()[target] = infos[i].codepoint;
        bool zeroWidth = isZeroWidthSpaceGlyph(infos[i].codepoint);
        glyphBuffer.offsetsInString()[target] = beginningStringIndex + infos[i].cluster;
        glyphBuffer.advances()[target] =
            zeroWidth ? makeGlyphBufferAdvance()
                      : makeGlyphBufferAdvance(positions[i].x_advance / 65536.0f, positions[i].y_advance / 65536.0f);
        glyphBuffer.origins()[target] =
            zeroWidth ? makeGlyphBufferOrigin()
                      : makeGlyphBufferOrigin({positions[i].x_offset / 65536.0f, positions[i].y_offset / 65536.0f});
        if (text[beginningStringIndex + infos[i].cluster] == ' ') {
            glyphBuffer.advances()[target] = makeGlyphBufferAdvance(spaceWidth(SyntheticBoldInclusion::Exclude), 0);
        }
    }
    if (direction == TextDirection::RTL && outputCount) {
        glyphBuffer.reverse(beginningGlyphIndex, outputCount);
    }
    return makeGlyphBufferAdvance();
}
