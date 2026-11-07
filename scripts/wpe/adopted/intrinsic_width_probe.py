from wpe.patch_support import replace_once


INLINE = "Source/WebCore/layout/formattingContexts/inline/"


PROBE_HELPERS = r'''static bool edenIntrinsicWidthProbeEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("WEBKIT_EDEN_PATCH_COUNTERS");
        return value && !std::strcmp(value, "1");
    }();
    return enabled;
}

static bool edenIntrinsicWidthProbeReserveEvent(uint64_t& event)
{
    static std::atomic<uint64_t> nextEvent { 0 };
    static constexpr uint64_t eventLimit = 262144;
    auto next = nextEvent.fetch_add(1, std::memory_order_relaxed);
    if (next >= eventLimit) {
        if (next == eventLimit)
            WTFLogAlways("Eden intrinsic width probe: truncated=1 event_limit=%llu", static_cast<unsigned long long>(eventLimit));
        return false;
    }
    event = next + 1;
    return true;
}

static void edenIntrinsicWidthProbeLog(uint64_t event, const char* path, bool candidate, bool eligible, unsigned lines, uint64_t start, uint64_t duration, uint64_t eligibilityDuration)
{
    WTFLogAlways("Eden intrinsic width probe: event=%llu path=%s candidate=%u eligible=%u lines=%u start_ns=%llu duration_ns=%llu eligibility_ns=%llu",
        static_cast<unsigned long long>(event), path, static_cast<unsigned>(candidate), static_cast<unsigned>(eligible), lines,
        static_cast<unsigned long long>(start), static_cast<unsigned long long>(duration),
        static_cast<unsigned long long>(eligibilityDuration));
}

'''


def intrinsic_source_patch(text):
    text = replace_once(
        text,
        '#include "TextOnlySimpleLineBuilder.h"\n#include <wtf/unicode/CharacterNames.h>',
        '#include "TextOnlySimpleLineBuilder.h"\n#include <atomic>\n#include <cmath>\n#include <cstdint>\n#include <cstdlib>\n#include <cstring>\n#include <wtf/Logging.h>\n#include <wtf/MonotonicTime.h>\n#include <wtf/unicode/CharacterNames.h>',
    )
    text = replace_once(text, "namespace Layout {\n", "namespace Layout {\n\n" + PROBE_HELPERS)
    text = replace_once(
        text,
        '''InlineLayoutUnit IntrinsicWidthHandler::minimumContentSize()
{
    if (isContentEligibleForNonLineBuilderMinimumWidth(formattingContextRoot(), m_inlineItems, m_mayUseSimplifiedTextOnlyInlineLayoutInRange))
        return simplifiedMinimumWidth(formattingContextRoot());

    if (m_mayUseSimplifiedTextOnlyInlineLayoutInRange) {
        auto simplifiedLineBuilder = TextOnlySimpleLineBuilder { formattingContext(), lineBuilderRoot(), { }, inlineItemList() };
        return computedIntrinsicWidthForConstraint(IntrinsicWidthMode::Minimum, simplifiedLineBuilder, MayCacheLayoutResult::No);
    }

    auto lineBuilder = LineBuilder { formattingContext(), { }, inlineItemList() };
    return computedIntrinsicWidthForConstraint(IntrinsicWidthMode::Minimum, lineBuilder, MayCacheLayoutResult::No);
}''',
        '''InlineLayoutUnit IntrinsicWidthHandler::minimumContentSize()
{
    auto event = uint64_t { 0 };
    if (!edenIntrinsicWidthProbeEnabled() || !edenIntrinsicWidthProbeReserveEvent(event)) {
        if (isContentEligibleForNonLineBuilderMinimumWidth(formattingContextRoot(), m_inlineItems, m_mayUseSimplifiedTextOnlyInlineLayoutInRange))
            return simplifiedMinimumWidth(formattingContextRoot());

        if (m_mayUseSimplifiedTextOnlyInlineLayoutInRange) {
            auto simplifiedLineBuilder = TextOnlySimpleLineBuilder { formattingContext(), lineBuilderRoot(), { }, inlineItemList() };
            return computedIntrinsicWidthForConstraint(IntrinsicWidthMode::Minimum, simplifiedLineBuilder, MayCacheLayoutResult::No);
        }

        auto lineBuilder = LineBuilder { formattingContext(), { }, inlineItemList() };
        return computedIntrinsicWidthForConstraint(IntrinsicWidthMode::Minimum, lineBuilder, MayCacheLayoutResult::No);
    }

    auto candidate = false;
    auto eligible = false;
    auto eligibilityDuration = uint64_t { 0 };
    auto lineCount = 0u;
    auto path = "full";
    auto eligibilityStart = MonotonicTime { };
    auto result = InlineLayoutUnit { };

    if (isContentEligibleForNonLineBuilderMinimumWidth(formattingContextRoot(), m_inlineItems, m_mayUseSimplifiedTextOnlyInlineLayoutInRange)) {
        path = "existing-simplified";
        auto pathStart = MonotonicTime::now();
        result = simplifiedMinimumWidth(formattingContextRoot());
        auto pathDuration = static_cast<uint64_t>((MonotonicTime::now() - pathStart).nanoseconds());
        edenIntrinsicWidthProbeLog(event, path, candidate, eligible, lineCount,
            static_cast<uint64_t>(pathStart.secondsSinceEpoch().nanoseconds()), pathDuration, eligibilityDuration);
        return result;
    } else if (m_mayUseSimplifiedTextOnlyInlineLayoutInRange) {
        path = "text-only";
        candidate = true;
        eligibilityStart = MonotonicTime::now();
        auto proposedGate = [&] {
            if (!m_mayUseSimplifiedTextOnlyInlineLayoutInRange || m_inlineItemRange.isEmpty() || m_inlineItems.hasTextAutospace())
                return false;

            auto& rootBox = lineBuilderRoot();
            auto& rootStyle = rootBox.style();
            if (&rootStyle != &rootBox.firstLineStyle())
                return false;
            if (!TextUtil::isWrappingAllowed(rootStyle))
                return false;
            if (rootStyle.lineBreak() == LineBreak::Anywhere || rootStyle.lineBreak() == LineBreak::AfterWhiteSpace)
                return false;
            if (rootStyle.wordBreak() == WordBreak::BreakAll || rootStyle.wordBreak() == WordBreak::BreakWord)
                return false;
            if (rootStyle.overflowWrap() == OverflowWrap::Anywhere || rootStyle.hyphens() == Hyphens::Auto || rootStyle.usedLetterSpacing() < 0)
                return false;

            auto whiteSpaceCollapse = rootStyle.whiteSpaceCollapse();
            if (whiteSpaceCollapse != WhiteSpaceCollapse::Collapse
                && whiteSpaceCollapse != WhiteSpaceCollapse::PreserveBreaks
                && whiteSpaceCollapse != WhiteSpaceCollapse::Preserve
                && whiteSpaceCollapse != WhiteSpaceCollapse::BreakSpaces)
                return false;

            const InlineTextBox* textBox = nullptr;
            for (auto index = m_inlineItemRange.startIndex(); index < m_inlineItemRange.endIndex(); ++index) {
                auto& inlineItem = inlineItemList()[index];
                if (inlineItem.isLineBreak())
                    continue;
                auto* inlineTextItem = dynamicDowncast<InlineTextItem>(inlineItem);
                if (!inlineTextItem)
                    return false;
                auto width = inlineTextItem->width();
                if (!inlineTextItem->length() || !width || *width <= 0 || !std::isfinite(*width) || inlineTextItem->hasTrailingSoftHyphen()
                    || inlineTextItem->isZeroWidthSpaceSeparator() || inlineTextItem->isQuirkNonBreakingSpace())
                    return false;
                if (textBox && textBox != &inlineTextItem->inlineTextBox())
                    return false;
                textBox = &inlineTextItem->inlineTextBox();
            }
            if (!textBox)
                return false;
            auto& textStyle = textBox->style();
            return &textStyle == &textBox->firstLineStyle()
                && textStyle.whiteSpaceCollapse() == whiteSpaceCollapse
                && textStyle.textWrapMode() == rootStyle.textWrapMode()
                && textStyle.lineBreak() == rootStyle.lineBreak()
                && textStyle.wordBreak() == rootStyle.wordBreak()
                && textStyle.overflowWrap() == rootStyle.overflowWrap()
                && textStyle.hyphens() == rootStyle.hyphens();
        };
        eligible = proposedGate();
        eligibilityDuration = static_cast<uint64_t>((MonotonicTime::now() - eligibilityStart).nanoseconds());
        auto pathStart = MonotonicTime::now();
        auto simplifiedLineBuilder = TextOnlySimpleLineBuilder { formattingContext(), lineBuilderRoot(), { }, inlineItemList() };
        result = computedIntrinsicWidthForConstraint(IntrinsicWidthMode::Minimum, simplifiedLineBuilder, MayCacheLayoutResult::No, &lineCount);
        auto pathDuration = static_cast<uint64_t>((MonotonicTime::now() - pathStart).nanoseconds());
        edenIntrinsicWidthProbeLog(event, path, candidate, eligible, lineCount,
            static_cast<uint64_t>(pathStart.secondsSinceEpoch().nanoseconds()), pathDuration, eligibilityDuration);
        return result;
    } else {
        auto pathStart = MonotonicTime::now();
        auto lineBuilder = LineBuilder { formattingContext(), { }, inlineItemList() };
        result = computedIntrinsicWidthForConstraint(IntrinsicWidthMode::Minimum, lineBuilder, MayCacheLayoutResult::No, &lineCount);
        auto pathDuration = static_cast<uint64_t>((MonotonicTime::now() - pathStart).nanoseconds());
        edenIntrinsicWidthProbeLog(event, path, candidate, eligible, lineCount,
            static_cast<uint64_t>(pathStart.secondsSinceEpoch().nanoseconds()), pathDuration, eligibilityDuration);
        return result;
    }
}''',
    )
    text = replace_once(
        text,
        'InlineLayoutUnit IntrinsicWidthHandler::computedIntrinsicWidthForConstraint(IntrinsicWidthMode intrinsicWidthMode, AbstractLineBuilder& lineBuilder, MayCacheLayoutResult mayCacheLayoutResult)',
        'InlineLayoutUnit IntrinsicWidthHandler::computedIntrinsicWidthForConstraint(IntrinsicWidthMode intrinsicWidthMode, AbstractLineBuilder& lineBuilder, MayCacheLayoutResult mayCacheLayoutResult, unsigned* lineCount)',
    )
    return replace_once(
        text,
        '''    m_maximumContentWidthBetweenLineBreaks = std::max(contentWidthBetweenLineBreaks.current, contentWidthBetweenLineBreaks.maximum);
    return maximumContentWidth;''',
        '''    m_maximumContentWidthBetweenLineBreaks = std::max(contentWidthBetweenLineBreaks.current, contentWidthBetweenLineBreaks.maximum);
    if (lineCount)
        *lineCount = static_cast<unsigned>(lineIndex + 1);
    return maximumContentWidth;''',
    )


def intrinsic_header_patch(text):
    return replace_once(
        text,
        'InlineLayoutUnit computedIntrinsicWidthForConstraint(IntrinsicWidthMode, AbstractLineBuilder&, MayCacheLayoutResult = MayCacheLayoutResult::No);',
        'InlineLayoutUnit computedIntrinsicWidthForConstraint(IntrinsicWidthMode, AbstractLineBuilder&, MayCacheLayoutResult = MayCacheLayoutResult::No, unsigned* lineCount = nullptr);',
    )


PATCHES = {
    INLINE + "IntrinsicWidthHandler.cpp": intrinsic_source_patch,
    INLINE + "IntrinsicWidthHandler.h": intrinsic_header_patch,
}
