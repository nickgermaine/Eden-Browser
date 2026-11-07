static bool edenCachedTextIntrinsicWidthEnabled() {
    static const bool enabled = [] {
        const char *value = std::getenv("WEBKIT_EDEN_INTRINSIC_WIDTH");
        return !value || std::strcmp(value, "0");
    }();
    return enabled;
}

static bool edenVerifyCachedTextIntrinsicWidth() {
#ifdef WEBKIT_EDEN_VERIFY_PATCHES
    return true;
#else
    static const bool enabled = [] {
        const char *value = std::getenv("WEBKIT_EDEN_VERIFY_INTRINSIC_WIDTH");
        return value && !std::strcmp(value, "1");
    }();
    return enabled;
#endif
}

InlineLayoutUnit IntrinsicWidthHandler::minimumContentSize() {
    if (edenCachedTextIntrinsicWidthEnabled()) {
        auto recordProbe = edenIntrinsicWidthProbeEnabled();
        auto start = recordProbe ? MonotonicTime::now() : MonotonicTime{};
        if (auto result = minimumContentSizeFromCachedText()) {
            auto event = uint64_t{};
            if (recordProbe && edenIntrinsicWidthProbeReserveEvent(event)) {
                edenIntrinsicWidthProbeLog(
                    event,
                    "cached-text",
                    true,
                    true,
                    0,
                    static_cast<uint64_t>(start.secondsSinceEpoch().nanoseconds()),
                    static_cast<uint64_t>((MonotonicTime::now() - start).nanoseconds()),
                    0
                );
            }
            if (edenVerifyCachedTextIntrinsicWidth()) {
                auto maximumBetweenBreaks = m_maximumContentWidthBetweenLineBreaks;
                m_maximumContentWidthBetweenLineBreaks = std::nullopt;
                auto reference = minimumContentSizeUsingLineBuilder();
                RELEASE_ASSERT(*result == reference);
                RELEASE_ASSERT(m_maximumContentWidthBetweenLineBreaks);
                RELEASE_ASSERT(maximumBetweenBreaks == m_maximumContentWidthBetweenLineBreaks);
                RELEASE_ASSERT(!m_maximumIntrinsicWidthResultForSingleLine);
            }
            return *result;
        }
    }
    return minimumContentSizeUsingLineBuilder();
}

std::optional<InlineLayoutUnit> IntrinsicWidthHandler::minimumContentSizeFromCachedText() {
    if (!m_mayUseSimplifiedTextOnlyInlineLayoutInRange || m_inlineItemRange.isEmpty() ||
        m_inlineItems.hasTextAutospace()) {
        return std::nullopt;
    }
    if (isContentEligibleForNonLineBuilderMinimumWidth(
            formattingContextRoot(),
            m_inlineItems,
            m_mayUseSimplifiedTextOnlyInlineLayoutInRange
        )) {
        return std::nullopt;
    }
    auto &rootBox = lineBuilderRoot();
    auto &rootStyle = rootBox.style();
    if (&rootStyle != &rootBox.firstLineStyle() || !TextUtil::isWrappingAllowed(rootStyle)) {
        return std::nullopt;
    }
    if (rootStyle.lineBreak() == LineBreak::Anywhere || rootStyle.lineBreak() == LineBreak::AfterWhiteSpace ||
        rootStyle.wordBreak() == WordBreak::BreakAll || rootStyle.wordBreak() == WordBreak::BreakWord ||
        rootStyle.overflowWrap() == OverflowWrap::Anywhere || rootStyle.hyphens() == Hyphens::Auto ||
        rootStyle.usedLetterSpacing() < 0) {
        return std::nullopt;
    }
    auto whitespace = rootStyle.whiteSpaceCollapse();
    if (whitespace != WhiteSpaceCollapse::Collapse && whitespace != WhiteSpaceCollapse::PreserveBreaks &&
        whitespace != WhiteSpaceCollapse::Preserve && whitespace != WhiteSpaceCollapse::BreakSpaces) {
        return std::nullopt;
    }

    auto &items = inlineItemList();
    const InlineTextBox *textBox = nullptr;
    for (auto index = m_inlineItemRange.startIndex(); index < m_inlineItemRange.endIndex(); ++index) {
        auto &item = items[index];
        if (item.isLineBreak()) {
            continue;
        }
        auto *text = dynamicDowncast<InlineTextItem>(item);
        if (!text || !text->length() || !text->width() || *text->width() <= 0 || !std::isfinite(*text->width()) ||
            text->hasTrailingSoftHyphen() || text->isZeroWidthSpaceSeparator() || text->isQuirkNonBreakingSpace()) {
            return std::nullopt;
        }
        if (textBox && textBox != &text->inlineTextBox()) {
            return std::nullopt;
        }
        textBox = &text->inlineTextBox();
    }
    if (!textBox) {
        return std::nullopt;
    }
    auto &textStyle = textBox->style();
    if (&textStyle != &textBox->firstLineStyle() || textStyle.whiteSpaceCollapse() != whitespace ||
        textStyle.textWrapMode() != rootStyle.textWrapMode() || textStyle.lineBreak() != rootStyle.lineBreak() ||
        textStyle.wordBreak() != rootStyle.wordBreak() || textStyle.overflowWrap() != rootStyle.overflowWrap() ||
        textStyle.hyphens() != rootStyle.hyphens()) {
        return std::nullopt;
    }

    auto preserveWhitespace =
        whitespace == WhiteSpaceCollapse::Preserve || whitespace == WhiteSpaceCollapse::BreakSpaces;
    auto hangWhitespace = whitespace == WhiteSpaceCollapse::Preserve;
    auto wrapBeforeWhitespace = whitespace != WhiteSpaceCollapse::BreakSpaces;
    auto maximumWidth = InlineLayoutUnit{};
    auto maximumBetweenBreaks = InlineLayoutUnit{};
    auto currentBetweenBreaks = InlineLayoutUnit{};
    auto lineWidth = InlineLayoutUnit{};
    auto hangingWidth = InlineLayoutUnit{};
    auto lineHasContent = false;
    auto finishLine = [&](bool forcedBreak) {
        auto width = lineWidth - hangingWidth;
        maximumWidth = std::max(maximumWidth, width);
        currentBetweenBreaks += width + hangingWidth;
        if (forcedBreak) {
            maximumBetweenBreaks = std::max(maximumBetweenBreaks, currentBetweenBreaks);
            currentBetweenBreaks = {};
        }
        lineWidth = {};
        hangingWidth = {};
        lineHasContent = false;
    };

    auto index = m_inlineItemRange.startIndex();
    while (index < m_inlineItemRange.endIndex()) {
        if (items[index].isLineBreak()) {
            finishLine(true);
            ++index;
            continue;
        }
        if (lineHasContent) {
            finishLine(false);
        }
        bool candidateFinished = false;
        while (!candidateFinished) {
            auto &text = downcast<InlineTextItem>(items[index++]);
            auto isWhitespace = text.isWhitespace();
            if (!isWhitespace || preserveWhitespace) {
                lineWidth += *text.width();
                lineHasContent = true;
                hangingWidth = isWhitespace && hangWhitespace ? *text.width() : 0.f;
            }
            candidateFinished = isWhitespace || index == m_inlineItemRange.endIndex() || items[index].isLineBreak() ||
                                !downcast<InlineTextItem>(items[index]).isWhitespace() || wrapBeforeWhitespace;
        }
    }
    if (lineHasContent) {
        finishLine(false);
    }
    maximumBetweenBreaks = std::max(maximumBetweenBreaks, currentBetweenBreaks);
    if (!std::isfinite(maximumWidth) || !std::isfinite(maximumBetweenBreaks)) {
        return std::nullopt;
    }
    m_maximumContentWidthBetweenLineBreaks = maximumBetweenBreaks;
    m_maximumIntrinsicWidthResultForSingleLine = {};
    return maximumWidth;
}
