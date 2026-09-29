from wpe.patch_support import replace_once


SINGLE_PASS_HELPERS = """static bool edenTextControlSinglePassEnabled()
{
    static bool enabled = [] {
        const char* value = std::getenv("WEBKIT_EDEN_DISABLE_TEXT_CONTROL_SINGLE_PASS");
        return !value || std::strcmp(value, "1");
    }();
    return enabled;
}

static bool edenTextControlCountersEnabled()
{
    static bool enabled = [] {
        const char* value = std::getenv("WEBKIT_EDEN_PATCH_COUNTERS");
        return value && !std::strcmp(value, "1");
    }();
    return enabled;
}

struct EdenTextControlCounters {
    unsigned long long layouts { 0 };
    unsigned long long secondPasses { 0 };
    unsigned long long presets { 0 };
    unsigned long long presetSecondPasses { 0 };
};

static EdenTextControlCounters& edenTextControlCounters()
{
    static EdenTextControlCounters counters;
    return counters;
}

static void edenNoteTextControlLayout(bool secondPass, bool preset)
{
    auto& counters = edenTextControlCounters();
    ++counters.layouts;
    if (secondPass)
        ++counters.secondPasses;
    if (preset) {
        ++counters.presets;
        if (secondPass)
            ++counters.presetSecondPasses;
    }
    if (!(counters.layouts % 4096))
        WTFLogAlways("Eden text control: layouts=%llu secondPasses=%llu presets=%llu presetSecondPasses=%llu", counters.layouts, counters.secondPasses, counters.presets, counters.presetSecondPasses);
}

static RenderBox* edenSoleInFlowChildBox(const RenderBox& parent)
{
    RenderBox* found = nullptr;
    for (auto* child = parent.firstChild(); child; child = child->nextSibling()) {
        if (child->isOutOfFlowPositioned())
            continue;
        auto* box = dynamicDowncast<RenderBox>(*child);
        if (!box || box->isFloating() || box->isInline() || found)
            return nullptr;
        found = box;
    }
    return found;
}

static bool edenBoxAddsNoBlockExtent(const RenderBox& box)
{
    return box.borderAndPaddingLogicalHeight() == 0_lu
        && box.marginLogicalHeight() == 0_lu
        && box.style().logicalHeight().isAuto()
        && box.style().logicalMinHeight().isAuto()
        && box.style().logicalMaxHeight().isNone()
        && !box.scrollbarLogicalHeight()
        && !box.isWritingModeRoot();
}

static std::optional<LayoutUnit> edenSingleLineHeightUpperBound(const RenderBox& innerTextRenderer)
{
    const auto& style = innerTextRenderer.style();
    if (!innerTextRenderer.isHorizontalWritingMode() || !innerTextRenderer.document().inNoQuirksMode()
        || !style.lineHeight().isNormal()
        || style.lineBoxContain() != Style::ComputedStyle::initialLineBoxContain()
        || !style.textEmphasisStyle().isNone() || style.textBoxTrim() != Style::ComputedStyle::initialTextBoxTrim()
        || style.textBoxEdge() != Style::ComputedStyle::initialTextBoxEdge()) {
        return { };
    }
    auto& primary = style.fontCascade().primaryFont();
    if (primary.hasVerticalGlyphs()) {
        return { };
    }
    float ascent = 0;
    float descent = 0;
    auto includeFont = [&](const Font& font) {
        auto& metrics = font.fontMetrics();
        auto leading = std::max(0.f, (metrics.lineSpacing() - metrics.ascent() - metrics.descent()) / 2);
        ascent = std::max(ascent, metrics.ascent() + leading);
        descent = std::max(descent, metrics.descent() + leading);
    };
    includeFont(primary);
    unsigned remaining = 4096;
    for (auto* child = innerTextRenderer.firstChild(); child; child = child->nextSibling()) {
        auto* text = dynamicDowncast<RenderText>(*child);
        if (!text || text->text().length() > remaining) {
            return { };
        }
        remaining -= text->text().length();
        auto fonts = Layout::TextUtil::fallbackFontsForText(text->text(), style, Layout::TextUtil::IncludeHyphen::No);
        for (Ref font : fonts) {
            if (font->hasVerticalGlyphs()) {
                return { };
            }
            includeFont(font.get());
        }
    }
    auto height = innerTextRenderer.lineHeight();
    height = std::max(height, LayoutUnit::fromFloatCeil(ascent) + LayoutUnit::fromFloatCeil(descent));
    return height + innerTextRenderer.borderAndPaddingLogicalHeight();
}

static std::optional<LayoutUnit> edenNaturalContainerLogicalHeightForSinglePass(const RenderTextControlSingleLine& control, const RenderBox& innerTextRenderer, const RenderBox* innerBlockRenderer, const RenderBox& containerRenderer, bool hasAutofillStrongPasswordButton)
{
    if (!edenTextControlSinglePassEnabled() || hasAutofillStrongPasswordButton)
        return { };
    if (control.style().fieldSizing() == FieldSizing::Content)
        return { };
    if (control.isOutOfFlowPositioned() || control.scrollbarLogicalHeight() || control.isWritingModeRoot())
        return { };
    auto* controlParent = control.parent();
    if (!controlParent || controlParent->isFlexibleBoxIncludingDeprecated() || controlParent->isRenderGrid())
        return { };
    if (!control.style().logicalHeight().tryFixed())
        return { };
    if (!control.style().logicalMinHeight().isAuto() || !control.style().logicalMaxHeight().isNone())
        return { };
    if (containerRenderer.style().usedZoomForLength().value != 1)
        return { };
    if (!edenBoxAddsNoBlockExtent(containerRenderer))
        return { };
    auto* soleContainerChild = edenSoleInFlowChildBox(containerRenderer);
    if (!soleContainerChild)
        return { };
    if (innerBlockRenderer) {
        if (soleContainerChild != innerBlockRenderer || !edenBoxAddsNoBlockExtent(*innerBlockRenderer))
            return { };
        if (edenSoleInFlowChildBox(*innerBlockRenderer) != &innerTextRenderer)
            return { };
    } else if (soleContainerChild != &innerTextRenderer)
        return { };
    if (!innerTextRenderer.style().logicalHeight().isAuto()
        || !innerTextRenderer.style().logicalMinHeight().isAuto()
        || !innerTextRenderer.style().logicalMaxHeight().isNone()
        || innerTextRenderer.scrollbarLogicalHeight()
        || innerTextRenderer.isWritingModeRoot()
        || innerTextRenderer.style().textWrapMode() != TextWrapMode::NoWrap)
        return { };
    auto heightUpperBound = edenSingleLineHeightUpperBound(innerTextRenderer);
    if (!heightUpperBound) {
        return { };
    }
    return *heightUpperBound + innerTextRenderer.marginLogicalHeight();
}

"""


PRESET_CALL = """    std::optional<LayoutUnit> edenPresetContainerLogicalHeight;
    if (innerTextRenderer && containerRenderer) {
        if (auto edenNaturalContainerLogicalHeight = edenNaturalContainerLogicalHeightForSinglePass(*this, *innerTextRenderer, innerBlockRenderer, *containerRenderer, inputElement().hasAutofillStrongPasswordButton())) {
            auto edenPredictedContentBoxLogicalHeight = computeLogicalHeight(logicalHeight(), logicalTop()).extent - borderAndPaddingLogicalHeight();
            if (edenPredictedContentBoxLogicalHeight > *edenNaturalContainerLogicalHeight + 1_lu)
                edenPresetContainerLogicalHeight = edenPredictedContentBoxLogicalHeight;
        }
    }
    if (edenPresetContainerLogicalHeight) {
        containerRenderer->mutableStyle().setLogicalHeight(Style::PreferredSize::Fixed { *edenPresetContainerLogicalHeight });
        for (RenderObject* renderer = containerRenderer; renderer != this; renderer = renderer->parent())
            renderer->setNeedsLayout(MarkingBehavior::MarkOnlyThis);
    }

"""


SECOND_PASS_COUNTER = """    bool edenNeedsSecondPass = needsLayout();
    if (edenTextControlCountersEnabled())
        edenNoteTextControlLayout(edenNeedsSecondPass, !!edenPresetContainerLogicalHeight);
#ifdef WEBKIT_EDEN_VERIFY_PATCHES
    if (edenPresetContainerLogicalHeight && *edenPresetContainerLogicalHeight != contentBoxLogicalHeight())
        WTFLogAlways("Eden text control: predicted content box %d does not match the laid out content box %d, the stock container fixup runs a second pass", edenPresetContainerLogicalHeight->toInt(), contentBoxLogicalHeight().toInt());
#endif
    if (edenNeedsSecondPass) {
"""


def single_line_patch(text):
    text = replace_once(text, '#include "config.h"', '#include "config.h"\n#include <cstdlib>\n#include <cstring>')
    text = replace_once(
        text,
        '#include "StyleComputedStyle+SettersInlines.h"\n',
        '#include "StyleComputedStyle+GettersInlines.h"\n#include "StyleComputedStyle+InitialInlines.h"\n#include "StyleComputedStyle+SettersInlines.h"\n#include "RenderText.h"\n#include "TextUtil.h"\n',
    )
    text = replace_once(
        text,
        "static void resetOverriddenHeight(RenderBox* box, const RenderObject* ancestor)\n",
        SINGLE_PASS_HELPERS + "static void resetOverriddenHeight(RenderBox* box, const RenderObject* ancestor)\n",
    )
    text = replace_once(
        text,
        """    {
        auto scope = LayoutScope { *this };
        RenderBlockFlow::layoutBlock(RelayoutChildren::No);
    }
""",
        PRESET_CALL + """    {
        auto scope = LayoutScope { *this };
        RenderBlockFlow::layoutBlock(RelayoutChildren::No);
    }
""",
    )
    return replace_once(
        text,
        """    if (needsLayout()) {
        auto scope = LayoutScope { *this };
        RenderBlockFlow::layoutBlock(RelayoutChildren::Yes);
    }
""",
        SECOND_PASS_COUNTER + """        auto scope = LayoutScope { *this };
        RenderBlockFlow::layoutBlock(RelayoutChildren::Yes);
    }
""",
    )


BREAK_CACHE_HELPERS = """static bool edenBreakingPositionCacheCountersEnabled()
{
    static bool enabled = [] {
        const char* value = std::getenv("WEBKIT_EDEN_PATCH_COUNTERS");
        return value && !std::strcmp(value, "1");
    }();
    return enabled;
}

struct EdenBreakingPositionCacheCounters {
    unsigned long long populations { 0 };
    unsigned long long teardownPopulations { 0 };
    unsigned long long teardownPopulationsSkipped { 0 };
    unsigned long long taggedEntries { 0 };
    unsigned long long lookups { 0 };
    unsigned long long hits { 0 };
    unsigned long long taggedLookups { 0 };
    unsigned long long taggedHits { 0 };
};

static EdenBreakingPositionCacheCounters& edenBreakingPositionCacheCounters()
{
    static EdenBreakingPositionCacheCounters counters;
    return counters;
}

static HashSet<uint64_t>& edenTeardownPopulatedTexts()
{
    static NeverDestroyed<HashSet<uint64_t>> texts;
    return texts.get();
}

static void edenReportBreakingPositionCacheCounters()
{
    auto& counters = edenBreakingPositionCacheCounters();
    WTFLogAlways("Eden teardown break cache: populations=%llu teardown=%llu skipped=%llu tagged=%llu lookups=%llu hits=%llu taggedLookups=%llu taggedHits=%llu", counters.populations, counters.teardownPopulations, counters.teardownPopulationsSkipped, counters.taggedEntries, counters.lookups, counters.hits, counters.taggedLookups, counters.taggedHits);
}

static void edenNoteBreakingPositionCachePopulation(InlineItemsBuilder::BreakingPositionCacheMode mode)
{
    auto& counters = edenBreakingPositionCacheCounters();
    ++counters.populations;
    if (mode != InlineItemsBuilder::BreakingPositionCacheMode::Populate)
        ++counters.teardownPopulations;
    if (mode == InlineItemsBuilder::BreakingPositionCacheMode::SkipInsideBulkTeardown || mode == InlineItemsBuilder::BreakingPositionCacheMode::RecordSkippedInsideBulkTeardown)
        ++counters.teardownPopulationsSkipped;
    if (!(counters.populations % 4096))
        edenReportBreakingPositionCacheCounters();
}

static uint64_t edenBreakingPositionCacheFingerprint(const TextBreakingPositionCache::Key& key)
{
    return (static_cast<uint64_t>(WTF::computeHash(key)) << 32) | WTF::computeHash(0x9e3779b9u, key);
}

static void edenTagTeardownBreakingPositionEntry(const TextBreakingPositionCache::Key& key)
{
    auto& texts = edenTeardownPopulatedTexts();
    const auto& text = std::get<0>(key);
    if (text.length() > 256)
        return;
    if (texts.size() >= 4096)
        texts.clear();
    if (texts.add(edenBreakingPositionCacheFingerprint(key)).isNewEntry)
        ++edenBreakingPositionCacheCounters().taggedEntries;
}

static void edenForgetTeardownBreakingPositionEntry(const TextBreakingPositionCache::Key& key)
{
    edenTeardownPopulatedTexts().remove(edenBreakingPositionCacheFingerprint(key));
}

static void edenNoteBreakingPositionCacheLookup(const TextBreakingPositionCache::Key& key, bool hit)
{
    auto& counters = edenBreakingPositionCacheCounters();
    ++counters.lookups;
    if (hit)
        ++counters.hits;
    if (edenTeardownPopulatedTexts().contains(edenBreakingPositionCacheFingerprint(key))) {
        ++counters.taggedLookups;
        if (hit)
            ++counters.taggedHits;
    }
    if (!(counters.lookups % 65536))
        edenReportBreakingPositionCacheCounters();
}

"""


def inline_items_builder_header_patch(text):
    return replace_once(
        text,
        "    static void populateBreakingPositionCache(const InlineItemList&, const Document&);\n",
        """    enum class BreakingPositionCacheMode : uint8_t { Populate, PopulateInsideBulkTeardown, SkipInsideBulkTeardown, RecordSkippedInsideBulkTeardown };
    static void populateBreakingPositionCache(const InlineItemList&, const Document&, BreakingPositionCacheMode = BreakingPositionCacheMode::Populate);
""",
    )


def inline_items_builder_patch(text):
    text = replace_once(text, '#include "config.h"', '#include "config.h"\n#include <cstdlib>\n#include <cstring>')
    text = replace_once(
        text,
        "#include <wtf/BitVector.h>\n",
        "#include <wtf/BitVector.h>\n#include <wtf/HashSet.h>\n#include <wtf/Hasher.h>\n#include <wtf/NeverDestroyed.h>\n#include <wtf/text/StringHash.h>\n",
    )
    text = replace_once(
        text,
        "struct WhitespaceContent {\n",
        BREAK_CACHE_HELPERS + "struct WhitespaceContent {\n",
    )
    text = replace_once(
        text,
        """    auto* breakingPositions = TextBreakingPositionCache::singleton().get({ text, { inlineTextBox.style() }, m_securityOrigin.data() });
    if (!breakingPositions)
        return false;
""",
        """    TextBreakingPositionCache::Key cacheKey { text, { inlineTextBox.style() }, m_securityOrigin.data() };
    auto* breakingPositions = TextBreakingPositionCache::singleton().get(cacheKey);
    if (edenBreakingPositionCacheCountersEnabled())
        edenNoteBreakingPositionCacheLookup(cacheKey, !!breakingPositions);
    if (!breakingPositions)
        return false;
""",
    )
    text = replace_once(
        text,
        """void InlineItemsBuilder::populateBreakingPositionCache(const InlineItemList& inlineItemList, const Document& document)
{
    if (inlineItemList.size() < TextBreakingPositionCache::minimumRequiredContentBreaks)
        return;
""",
        """void InlineItemsBuilder::populateBreakingPositionCache(const InlineItemList& inlineItemList, const Document& document, BreakingPositionCacheMode mode)
{
    if (edenBreakingPositionCacheCountersEnabled())
        edenNoteBreakingPositionCachePopulation(mode);
    if (mode == BreakingPositionCacheMode::SkipInsideBulkTeardown)
        return;
    bool edenRecordOnly = mode == BreakingPositionCacheMode::RecordSkippedInsideBulkTeardown;
    bool edenTagEntries = edenRecordOnly || mode == BreakingPositionCacheMode::PopulateInsideBulkTeardown;
    if (inlineItemList.size() < TextBreakingPositionCache::minimumRequiredContentBreaks)
        return;
""",
    )
    text = replace_once(
        text,
        """        TextBreakingPositionContext context { inlineTextBox->style() };
        if (breakingPositionCache.get({ inlineTextBox->content(), context, securityOrigin->data() })) {
""",
        """        TextBreakingPositionContext context { inlineTextBox->style() };
        TextBreakingPositionCache::Key cacheKey { inlineTextBox->content(), context, securityOrigin->data() };
        if (breakingPositionCache.get(cacheKey)) {
""",
    )
    return replace_once(
        text,
        """        ASSERT(!breakingPositionList.isEmpty());
        if (breakingPositionList.size() >= TextBreakingPositionCache::minimumRequiredContentBreaks)
            breakingPositionCache.set({ inlineTextBox->content(), context, securityOrigin->data() }, WTF::move(breakingPositionList));
""",
        """        ASSERT(!breakingPositionList.isEmpty());
        if (breakingPositionList.size() >= TextBreakingPositionCache::minimumRequiredContentBreaks) {
            if (edenTagEntries)
                edenTagTeardownBreakingPositionEntry(cacheKey);
            else if (edenBreakingPositionCacheCountersEnabled())
                edenForgetTeardownBreakingPositionEntry(cacheKey);
            if (!edenRecordOnly)
                breakingPositionCache.set(cacheKey, WTF::move(breakingPositionList));
        }
""",
    )


LINE_LAYOUT_HELPERS = """static bool edenTeardownBreakCacheSkipEnabled()
{
    static bool enabled = [] {
        const char* value = std::getenv("WEBKIT_EDEN_DISABLE_TEARDOWN_BREAK_CACHE_SKIP");
        return !value || std::strcmp(value, "1");
    }();
    return enabled;
}

static bool edenLineLayoutCountersEnabled()
{
    static bool enabled = [] {
        const char* value = std::getenv("WEBKIT_EDEN_PATCH_COUNTERS");
        return value && !std::strcmp(value, "1");
    }();
    return enabled;
}

static Layout::InlineItemsBuilder::BreakingPositionCacheMode edenBreakingPositionCacheMode()
{
    using Mode = Layout::InlineItemsBuilder::BreakingPositionCacheMode;
    if (!RenderTreeBulkTeardownScope::isInProgress())
        return Mode::Populate;
    bool shouldSkip = edenTeardownBreakCacheSkipEnabled();
    if (!shouldSkip)
        return edenLineLayoutCountersEnabled() ? Mode::PopulateInsideBulkTeardown : Mode::Populate;
    return edenLineLayoutCountersEnabled() ? Mode::RecordSkippedInsideBulkTeardown : Mode::SkipInsideBulkTeardown;
}

"""


def line_layout_patch(text):
    text = replace_once(text, '#include "config.h"', '#include "config.h"\n#include <cstdlib>\n#include <cstring>')
    text = replace_once(
        text,
        '#include "RenderObjectInlines.h"\n',
        '#include "RenderObjectInlines.h"\n#include "RenderTreeUpdater.h"\n',
    )
    text = replace_once(
        text,
        "LineLayout::~LineLayout()\n{\n",
        LINE_LAYOUT_HELPERS + "LineLayout::~LineLayout()\n{\n",
    )
    return replace_once(
        text,
        "    if (shouldPopulateBreakingPositionCache())\n        Layout::InlineItemsBuilder::populateBreakingPositionCache(m_inlineContentCache.inlineItems().content(), protect(rootRenderer->document()));\n",
        "    if (shouldPopulateBreakingPositionCache())\n        Layout::InlineItemsBuilder::populateBreakingPositionCache(m_inlineContentCache.inlineItems().content(), protect(rootRenderer->document()), edenBreakingPositionCacheMode());\n",
    )


PATCHES = {
    "Source/WebCore/rendering/RenderTextControlSingleLine.cpp": single_line_patch,
    "Source/WebCore/layout/formattingContexts/inline/InlineItemsBuilder.h": inline_items_builder_header_patch,
    "Source/WebCore/layout/formattingContexts/inline/InlineItemsBuilder.cpp": inline_items_builder_patch,
    "Source/WebCore/layout/integration/inline/LayoutIntegrationLineLayout.cpp": line_layout_patch,
}
