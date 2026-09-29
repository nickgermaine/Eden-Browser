from wpe.patch_support import replace_once


RENDERING = "Source/WebCore/rendering/"

HELPERS = """enum class EdenFlexInvalidationReason : uint8_t {
    Skipped,
    Dirty,
    CrossAxisIsInline,
    FlexBasis,
    ItemShape,
    DescendantShape,
    Budget,
    Count
};

static constexpr unsigned edenFlexInvalidationWalkBudget = 64;

static bool edenFlexInvalidationSkipEnabled()
{
    static bool enabled = [] {
        const char* value = std::getenv("WEBKIT_EDEN_DISABLE_FLEX_INVALIDATION_SKIP");
        return !value || std::strcmp(value, "1");
    }();
    return enabled;
}

static bool edenFlexInvalidationCountersEnabled()
{
    static bool enabled = [] {
        const char* value = std::getenv("WEBKIT_EDEN_PATCH_COUNTERS");
        return value && !std::strcmp(value, "1");
    }();
    return enabled;
}

static void edenFlexInvalidationCount(EdenFlexInvalidationReason reason)
{
    static std::array<uint64_t, static_cast<size_t>(EdenFlexInvalidationReason::Count)> counts { };
    static uint64_t events = 0;
    ++counts[static_cast<size_t>(reason)];
    if (++events % 4096)
        return;
    WTFLogAlways("Eden flex invalidation: events %llu skipped %llu dirty %llu crossAxisIsInline %llu flexBasis %llu itemShape %llu descendantShape %llu budget %llu",
        static_cast<unsigned long long>(events),
        static_cast<unsigned long long>(counts[static_cast<size_t>(EdenFlexInvalidationReason::Skipped)]),
        static_cast<unsigned long long>(counts[static_cast<size_t>(EdenFlexInvalidationReason::Dirty)]),
        static_cast<unsigned long long>(counts[static_cast<size_t>(EdenFlexInvalidationReason::CrossAxisIsInline)]),
        static_cast<unsigned long long>(counts[static_cast<size_t>(EdenFlexInvalidationReason::FlexBasis)]),
        static_cast<unsigned long long>(counts[static_cast<size_t>(EdenFlexInvalidationReason::ItemShape)]),
        static_cast<unsigned long long>(counts[static_cast<size_t>(EdenFlexInvalidationReason::DescendantShape)]),
        static_cast<unsigned long long>(counts[static_cast<size_t>(EdenFlexInvalidationReason::Budget)]));
}

static bool edenFlexOverflowReservesNoInlineSpace(Overflow overflow)
{
    return overflow == Overflow::Visible || overflow == Overflow::Hidden || overflow == Overflow::Clip;
}

static bool edenFlexRendererIsCrossSizeNeutral(const RenderElement& renderer, bool isHorizontalWritingMode, bool& shouldDescend)
{
    shouldDescend = false;
    const auto& styleToCheck = renderer.style();

    switch (renderer.type()) {
    case RenderObject::Type::LineBreak:
        return true;
    case RenderObject::Type::Inline:
        if (styleToCheck.display().value != Style::DisplayType::InlineFlow)
            return false;
        shouldDescend = true;
        return true;
    case RenderObject::Type::BlockFlow:
    case RenderObject::Type::ListItem:
        break;
    default:
        return false;
    }

    switch (styleToCheck.display().value) {
    case Style::DisplayType::BlockFlow:
    case Style::DisplayType::BlockFlowRoot:
    case Style::DisplayType::InlineFlowRoot:
    case Style::DisplayType::BlockFlowListItem:
        break;
    default:
        return false;
    }

    const auto* box = dynamicDowncast<RenderBox>(renderer);
    if (!box)
        return false;
    if (box->isHorizontalWritingMode() != isHorizontalWritingMode)
        return false;
    if (box->isExcludedFromNormalLayout() || box->isFieldset() || box->isLegend())
        return false;
    if (styleToCheck.aspectRatio().hasRatio() || box->hasIntrinsicAspectRatio())
        return false;
    if (styleToCheck.hasUsedAppearance())
        return false;

    const auto& logicalHeight = styleToCheck.logicalHeight();
    if (!logicalHeight.isAuto() && !logicalHeight.tryFixed())
        return false;
    const auto& logicalMinHeight = styleToCheck.logicalMinHeight();
    if (!logicalMinHeight.isAuto() && !logicalMinHeight.tryFixed())
        return false;
    const auto& logicalMaxHeight = styleToCheck.logicalMaxHeight();
    if (!logicalMaxHeight.isNone() && !logicalMaxHeight.tryFixed())
        return false;

    if (box->shouldInvalidateContentWidths())
        return false;
    if (!edenFlexOverflowReservesNoInlineSpace(styleToCheck.overflowX()) || !edenFlexOverflowReservesNoInlineSpace(styleToCheck.overflowY()))
        return false;
    if (!styleToCheck.scrollbarGutter().isAuto())
        return false;
    if (renderer.shouldApplySizeOrInlineSizeContainment())
        return false;
    if (styleToCheck.contentVisibility() != ContentVisibility::Visible)
        return false;
    if (styleToCheck.specifiesColumns())
        return false;
    if (!styleToCheck.lineClamp().isNone())
        return false;
    if (styleToCheck.maxLines().tryValue())
        return false;
    if (const auto* block = dynamicDowncast<RenderBlock>(renderer); block && block->hasPercentHeightDescendants())
        return false;

    shouldDescend = true;
    return true;
}

static EdenFlexInvalidationReason edenFlexInvalidationReasonFor(const RenderBox& flexItem, bool crossAxisOverrideIsBlockAxis)
{
    if (flexItem.hasInvalidContentLogicalWidths())
        return EdenFlexInvalidationReason::Dirty;
    if (!crossAxisOverrideIsBlockAxis)
        return EdenFlexInvalidationReason::CrossAxisIsInline;
    if (!flexItem.style().flexBasis().isAuto())
        return EdenFlexInvalidationReason::FlexBasis;

    auto isHorizontalWritingMode = flexItem.isHorizontalWritingMode();
    auto budget = edenFlexInvalidationWalkBudget;
    const RenderObject* current = &flexItem;
    while (current) {
        if (!budget--)
            return EdenFlexInvalidationReason::Budget;

        bool shouldDescend = false;
        bool isFlexItem = current == &flexItem;
        if (const auto* renderer = dynamicDowncast<RenderElement>(*current)) {
            if (isFlexItem || !renderer->isOutOfFlowPositioned()) {
                if (!edenFlexRendererIsCrossSizeNeutral(*renderer, isHorizontalWritingMode, shouldDescend))
                    return isFlexItem ? EdenFlexInvalidationReason::ItemShape : EdenFlexInvalidationReason::DescendantShape;
            }
        } else if (current->type() != RenderObject::Type::Text && current->type() != RenderObject::Type::TextFragment)
            return EdenFlexInvalidationReason::DescendantShape;

        if (shouldDescend) {
            if (auto* child = downcast<RenderElement>(*current).firstChild()) {
                current = child;
                continue;
            }
        }
        if (current == &flexItem)
            return EdenFlexInvalidationReason::Skipped;
        while (!current->nextSibling()) {
            current = current->parent();
            if (!current)
                return EdenFlexInvalidationReason::DescendantShape;
            if (current == &flexItem)
                return EdenFlexInvalidationReason::Skipped;
        }
        current = current->nextSibling();
    }
    return EdenFlexInvalidationReason::DescendantShape;
}

bool RenderFlexibleBox::edenShouldSkipContentWidthsInvalidation(const RenderBox& flexItem, bool crossAxisOverrideIsBlockAxis) const
{
    if (!edenFlexInvalidationSkipEnabled())
        return false;
    if (flexItem.hasInvalidContentLogicalWidths() || !crossAxisOverrideIsBlockAxis) {
        if (edenFlexInvalidationCountersEnabled()) {
            edenFlexInvalidationCount(flexItem.hasInvalidContentLogicalWidths() ? EdenFlexInvalidationReason::Dirty : EdenFlexInvalidationReason::CrossAxisIsInline);
        }
        return false;
    }
    auto generation = document().edenRenderMutationGeneration();
    if (m_edenCrossSizeMemoGeneration != generation) {
        m_edenCrossSizeMemo.clear();
        m_edenCrossSizeMemoGeneration = generation;
    }
    if (auto it = m_edenCrossSizeMemo.find(&flexItem); it != m_edenCrossSizeMemo.end()) {
        if (edenFlexInvalidationCountersEnabled()) {
            edenFlexInvalidationCount(it->value ? EdenFlexInvalidationReason::Skipped : EdenFlexInvalidationReason::DescendantShape);
        }
        return it->value;
    }
    auto reason = edenFlexInvalidationReasonFor(flexItem, crossAxisOverrideIsBlockAxis);
    if (m_edenCrossSizeMemo.size() >= 512) {
        m_edenCrossSizeMemo.clear();
    }
    m_edenCrossSizeMemo.add(&flexItem, reason == EdenFlexInvalidationReason::Skipped);
    if (edenFlexInvalidationCountersEnabled())
        edenFlexInvalidationCount(reason);
    return reason == EdenFlexInvalidationReason::Skipped;
}

"""

SCOPE_CONSTRUCTOR = "RenderFlexibleBox::ScopedCrossAxisOverrideForFlexItem::ScopedCrossAxisOverrideForFlexItem(const RenderFlexibleBox& flexBox, RenderBox& flexItem, InvalidateContentWidths invalidateContentWidths)"

STOCK_INVALIDATION = """        if (invalidateContentWidths == InvalidateContentWidths::Yes) {
            flexItem.invalidateContentLogicalWidths(MarkingBehavior::MarkOnlyThis);
#if ASSERT_ENABLED
            m_didInvalidateContentLogicalWidths = true;
#endif
        }
"""

GUARDED_INVALIDATION = """        if (invalidateContentWidths == InvalidateContentWidths::Yes) {
            if (flexBox.edenShouldSkipContentWidthsInvalidation(flexItem, axis == OverridingSizesScope::Axis::Block)) {
#ifdef WEBKIT_EDEN_VERIFY_PATCHES
                m_edenVerifiedFlexItem = &flexItem;
                m_edenCachedMinContribution = flexItem.minContentLogicalWidthContribution();
                m_edenCachedMaxContribution = flexItem.maxContentLogicalWidthContribution();
                flexItem.invalidateContentLogicalWidths(MarkingBehavior::MarkOnlyThis);
#if ASSERT_ENABLED
                m_didInvalidateContentLogicalWidths = true;
#endif
#endif
            } else {
                flexItem.invalidateContentLogicalWidths(MarkingBehavior::MarkOnlyThis);
#if ASSERT_ENABLED
                m_didInvalidateContentLogicalWidths = true;
#endif
            }
        }
"""

SCOPE_DESTRUCTOR = "RenderFlexibleBox::ScopedCrossAxisOverrideForFlexItem::~ScopedCrossAxisOverrideForFlexItem()\n{\n"

VERIFIED_DESTRUCTOR = (
    SCOPE_DESTRUCTOR
    + """#ifdef WEBKIT_EDEN_VERIFY_PATCHES
    if (CheckedPtr edenFlexItem = m_edenVerifiedFlexItem) {
        auto edenStockMinContribution = edenFlexItem->minContentLogicalWidthContribution();
        auto edenStockMaxContribution = edenFlexItem->maxContentLogicalWidthContribution();
        if (edenStockMinContribution != m_edenCachedMinContribution || edenStockMaxContribution != m_edenCachedMaxContribution) {
            WTFLogAlways("Eden flex invalidation skip mismatch: %s skipped min %.2f max %.2f stock min %.2f max %.2f",
                edenFlexItem->renderName().characters(),
                m_edenCachedMinContribution.toFloat(), m_edenCachedMaxContribution.toFloat(),
                edenStockMinContribution.toFloat(), edenStockMaxContribution.toFloat());
            RELEASE_ASSERT_NOT_REACHED();
        }
    }
#endif
"""
)

SCOPE_MEMBERS = """        SetForScope<bool> m_intrinsicWidthComputation;
        std::optional<OverridingSizesScope> m_overridingScope;
"""

VERIFIED_SCOPE_MEMBERS = (
    SCOPE_MEMBERS
    + """#ifdef WEBKIT_EDEN_VERIFY_PATCHES
        CheckedPtr<RenderBox> m_edenVerifiedFlexItem;
        LayoutUnit m_edenCachedMinContribution;
        LayoutUnit m_edenCachedMaxContribution;
#endif
"""
)


def flexible_box_patch(text):
    text = replace_once(text, '#include "config.h"', '#include "config.h"\n#include <cstdlib>\n#include <cstring>')
    text = replace_once(text, "#include <wtf/SetForScope.h>", "#include <array>\n#include <wtf/SetForScope.h>")
    text = replace_once(text, SCOPE_CONSTRUCTOR, HELPERS + SCOPE_CONSTRUCTOR)
    text = replace_once(text, STOCK_INVALIDATION, GUARDED_INVALIDATION)
    return replace_once(text, SCOPE_DESTRUCTOR, VERIFIED_DESTRUCTOR)


def flexible_box_header_patch(text):
    text = replace_once(text, "#include <wtf/WeakHashSet.h>", "#include <wtf/WeakHashSet.h>\n#include <wtf/HashMap.h>")
    text = replace_once(text, "    ASCIILiteral renderName() const override;", """    ASCIILiteral renderName() const override;
    bool edenShouldSkipContentWidthsInvalidation(const RenderBox&, bool) const;
private:
    mutable uint64_t m_edenCrossSizeMemoGeneration { 0 };
    mutable HashMap<const RenderBox*, bool> m_edenCrossSizeMemo;
public:""")
    return replace_once(text, SCOPE_MEMBERS, VERIFIED_SCOPE_MEMBERS)


def document_header_patch(text):
    text = replace_once(text, "    uint64_t domTreeVersion() const { return m_domTreeVersion; }", """    uint64_t domTreeVersion() const { return m_domTreeVersion; }
    uint64_t edenRenderMutationGeneration() const { return m_edenRenderMutationGeneration; }
    void edenDidMutateRenderTree();""")
    return replace_once(text, "    uint64_t m_domTreeVersion;", "    uint64_t m_domTreeVersion;\n    uint64_t m_edenRenderMutationGeneration { 1 };")


def document_patch(text):
    text = replace_once(text, '#include "config.h"', '#include "config.h"\n#include <cstdlib>\n#include <cstring>')
    return replace_once(
        text,
        "Document::Document(LocalFrame* frame, const Settings& settings, const URL& url, DocumentClasses documentClasses, OptionSet<ConstructionFlag> constructionFlags, std::optional<ScriptExecutionContextIdentifier> identifier)\n",
        """void Document::edenDidMutateRenderTree()
{
    static bool enabled = [] {
        const char* value = std::getenv("WEBKIT_EDEN_DISABLE_FLEX_INVALIDATION_SKIP");
        return !value || std::strcmp(value, "1");
    }();
    if (enabled)
        RELEASE_ASSERT(++m_edenRenderMutationGeneration);
}

Document::Document(LocalFrame* frame, const Settings& settings, const URL& url, DocumentClasses documentClasses, OptionSet<ConstructionFlag> constructionFlags, std::optional<ScriptExecutionContextIdentifier> identifier)
""",
    )


def render_element_patch(text):
    anchor = "void RenderElement::styleDidChange(Style::Difference diff, const Style::ComputedStyle* oldStyle)\n{"
    return replace_once(text, anchor, anchor + "\n    document().edenDidMutateRenderTree();")


def render_object_patch(text):
    anchor = "void RenderObject::setParent(RenderElement* parent)\n{"
    return replace_once(text, anchor, anchor + "\n    document().edenDidMutateRenderTree();")


PATCHES = {
    "Source/WebCore/dom/Document.h": document_header_patch,
    "Source/WebCore/dom/Document.cpp": document_patch,
    RENDERING + "RenderElement.cpp": render_element_patch,
    RENDERING + "RenderObject.cpp": render_object_patch,
    RENDERING + "RenderFlexibleBox.cpp": flexible_box_patch,
    RENDERING + "RenderFlexibleBox.h": flexible_box_header_patch,
}
