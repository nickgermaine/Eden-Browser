from wpe.patch_support import replace_once


SCOPE_DECLARATION = """class RenderBlock;
class RenderElement;

class RenderTreeBulkTeardownScope {
    WTF_MAKE_NONCOPYABLE(RenderTreeBulkTeardownScope);
public:
    explicit RenderTreeBulkTeardownScope(ContainerNode&);
    ~RenderTreeBulkTeardownScope();

    static bool isInProgress() { return !!s_current; }
    static bool repaintAndMarkContainerInsteadOfRoot(const Element&, const RenderElement&);

private:
    bool coverRoot(const Element&, const RenderElement&);

    static RenderTreeBulkTeardownScope* s_current;

    RenderTreeBulkTeardownScope* const m_previous;
    const ContainerNode& m_container;
    SingleThreadWeakPtr<RenderBlock> m_repaintedContainerRenderer;
};

"""


SCOPE_IMPLEMENTATION = """static bool edenBulkTeardownEnabled()
{
    static bool enabled = [] {
        const char* value = std::getenv("WEBKIT_EDEN_DISABLE_BULK_TEARDOWN");
        return !value || std::strcmp(value, "1");
    }();
    return enabled;
}

static bool edenBulkTeardownCountersEnabled()
{
    static bool enabled = [] {
        const char* value = std::getenv("WEBKIT_EDEN_PATCH_COUNTERS");
        return value && !std::strcmp(value, "1");
    }();
    return enabled;
}

struct EdenBulkTeardownCounters {
    unsigned long long scopes { 0 };
    unsigned long long roots { 0 };
    unsigned long long batchedRoots { 0 };
    unsigned long long containerRepaints { 0 };
};

static EdenBulkTeardownCounters& edenBulkTeardownCounters()
{
    static EdenBulkTeardownCounters counters;
    return counters;
}

static void edenNoteBulkTeardownRoot(bool batched)
{
    auto& counters = edenBulkTeardownCounters();
    ++counters.roots;
    if (batched)
        ++counters.batchedRoots;
    if (!(counters.roots % 4096))
        WTFLogAlways("Eden bulk teardown: scopes=%llu roots=%llu batched=%llu containerRepaints=%llu", counters.scopes, counters.roots, counters.batchedRoots, counters.containerRepaints);
}

static bool edenBulkTeardownIsUnsupportedBoxKind(const RenderBlock& renderer)
{
    return renderer.isAnonymous()
        || renderer.isRenderView()
        || renderer.isBody()
        || renderer.isDocumentElementRenderer()
        || renderer.isRenderTable()
        || renderer.isRenderTableCell()
        || renderer.isRenderTableCaption()
        || renderer.isRenderTableSection()
        || renderer.isRenderSVGBlock()
        || renderer.isRenderFragmentedFlow()
        || renderer.isRenderMultiColumnSet()
        || renderer.isRenderMultiColumnSpannerPlaceholder()
        || renderer.isRenderTextControl();
}

static bool edenBulkTeardownIsFragmented(const RenderBlock& renderer)
{
    if (renderer.fragmentedFlowState() != RenderObject::FragmentedFlowState::NotInsideFlow)
        return true;
    auto* blockFlow = dynamicDowncast<RenderBlockFlow>(renderer);
    return blockFlow && blockFlow->multiColumnFlow();
}

static RenderBlock* edenBulkTeardownContainerRenderer(const ContainerNode& container)
{
    if (!is<Element>(container))
        return nullptr;
    auto* renderer = dynamicDowncast<RenderBlock>(container.renderer());
    if (!renderer)
        return nullptr;
    if (renderer->renderTreeBeingDestroyed() || !renderer->everHadLayout())
        return nullptr;
    if (edenBulkTeardownIsUnsupportedBoxKind(*renderer) || edenBulkTeardownIsFragmented(*renderer))
        return nullptr;
    if (renderer->childrenInline())
        return nullptr;
    if (renderer->usesCompositedScrolling())
        return nullptr;
    if (renderer->capturedInViewTransition())
        return nullptr;
    if (renderer->document().activeViewTransition())
        return nullptr;
    if (renderer->view().frameView().layoutContext().isInLayout())
        return nullptr;
    if (renderer->view().printing())
        return nullptr;
    return renderer;
}

static bool edenBulkTeardownRootIsCovered(const RenderElement& rootRenderer, const RenderBlock& containerRenderer)
{
    auto* rootBlock = dynamicDowncast<RenderBlock>(rootRenderer);
    if (!rootBlock)
        return false;
    if (rootBlock->parent() != &containerRenderer || rootBlock->container() != &containerRenderer)
        return false;
    if (!rootBlock->everHadLayout())
        return false;
    if (rootBlock->hasLayer() || rootBlock->isInline())
        return false;
    if (rootBlock->isFloating() || rootBlock->isPositioned())
        return false;
    if (rootBlock->style().position() != PositionType::Static)
        return false;
    if (edenBulkTeardownIsUnsupportedBoxKind(*rootBlock) || edenBulkTeardownIsFragmented(*rootBlock))
        return false;
    if (rootBlock->hasOutlineAutoAncestor() || rootBlock->capturedInViewTransition())
        return false;
    if (rootBlock->isWritingModeRoot() || rootBlock->shouldApplyAnyContainment())
        return false;
    if (rootBlock->hasReflection() || rootBlock->hasFilter())
        return false;
    unsigned remaining = 256;
    for (auto* descendant = rootRenderer.firstChild(); descendant; descendant = descendant->nextInPreOrder(&rootRenderer)) {
        if (!remaining-- || descendant->hasLayer() || descendant->isFloating() || descendant->isPositioned()) {
            return false;
        }
        if (auto* element = dynamicDowncast<RenderElement>(*descendant)) {
            if (element->hasReflection() || element->hasFilter() || element->shouldApplyAnyContainment()
                || element->capturedInViewTransition() || element->hasOutlineAutoAncestor()) {
                return false;
            }
        }
    }
    return true;
}

static bool edenBulkTeardownVerifyCoverage(const RenderElement& rootRenderer, const RenderBlock& containerRenderer)
{
    auto rootStatus = rootRenderer.containerForRepaint();
    auto containerStatus = containerRenderer.containerForRepaint();
    if (rootStatus.renderer.get() != containerStatus.renderer.get()) {
        if (edenBulkTeardownCountersEnabled())
            WTFLogAlways("Eden bulk teardown verify: repaint container differs between removed root %p and its container %p, falling back to the stock root repaint", &rootRenderer, &containerRenderer);
        return false;
    }
    if (containerStatus.fullRepaintIsScheduled != rootStatus.fullRepaintIsScheduled) {
        if (edenBulkTeardownCountersEnabled())
            WTFLogAlways("Eden bulk teardown verify: scheduled full repaint state differs between removed root %p and its container %p, falling back to the stock root repaint", &rootRenderer, &containerRenderer);
        return false;
    }
    auto rootRect = rootRenderer.clippedOverflowRectForRepaint(rootStatus.renderer.get());
    if (rootRect.isEmpty())
        return true;
    auto containerRect = containerRenderer.clippedOverflowRectForRepaint(containerStatus.renderer.get());
    if (containerRect.contains(rootRect))
        return true;
    if (edenBulkTeardownCountersEnabled())
        WTFLogAlways("Eden bulk teardown verify: removed root %p repaint rect (%d,%d %dx%d) is not covered by container %p repaint rect (%d,%d %dx%d), falling back to the stock root repaint", &rootRenderer, rootRect.x().toInt(), rootRect.y().toInt(), rootRect.width().toInt(), rootRect.height().toInt(), &containerRenderer, containerRect.x().toInt(), containerRect.y().toInt(), containerRect.width().toInt(), containerRect.height().toInt());
    return false;
}

RenderTreeBulkTeardownScope* RenderTreeBulkTeardownScope::s_current = nullptr;

RenderTreeBulkTeardownScope::RenderTreeBulkTeardownScope(ContainerNode& container)
    : m_previous(s_current)
    , m_container(container)
{
    s_current = this;
    if (edenBulkTeardownCountersEnabled())
        ++edenBulkTeardownCounters().scopes;
}

RenderTreeBulkTeardownScope::~RenderTreeBulkTeardownScope()
{
    s_current = m_previous;
}

bool RenderTreeBulkTeardownScope::repaintAndMarkContainerInsteadOfRoot(const Element& root, const RenderElement& rootRenderer)
{
    auto* scope = s_current;
    if (!scope || !edenBulkTeardownEnabled())
        return false;
    bool batched = scope->coverRoot(root, rootRenderer);
    if (edenBulkTeardownCountersEnabled())
        edenNoteBulkTeardownRoot(batched);
    return batched;
}

bool RenderTreeBulkTeardownScope::coverRoot(const Element& root, const RenderElement& rootRenderer)
{
    if (root.parentNode() != &m_container)
        return false;
    auto* containerRenderer = edenBulkTeardownContainerRenderer(m_container);
    if (!containerRenderer)
        return false;
    if (!edenBulkTeardownRootIsCovered(rootRenderer, *containerRenderer))
        return false;
    if (!edenBulkTeardownVerifyCoverage(rootRenderer, *containerRenderer))
        return false;
    if (m_repaintedContainerRenderer.get() != containerRenderer) {
        containerRenderer->repaint();
        containerRenderer->setChildNeedsLayout();
        containerRenderer->invalidateContentLogicalWidths();
        m_repaintedContainerRenderer = containerRenderer;
        if (edenBulkTeardownCountersEnabled())
            ++edenBulkTeardownCounters().containerRepaints;
    }
    return true;
}

"""


def updater_header_patch(text):
    text = replace_once(
        text,
        "#include <wtf/Vector.h>\n",
        "#include <wtf/Noncopyable.h>\n#include <wtf/Vector.h>\n#include <wtf/WeakPtr.h>\n",
    )
    return replace_once(
        text,
        "class RenderTreeUpdater {\npublic:\n    class GeneratedContent;\n",
        SCOPE_DECLARATION + "class RenderTreeUpdater {\npublic:\n    class GeneratedContent;\n",
    )


def updater_patch(text):
    text = replace_once(text, '#include "config.h"', '#include "config.h"\n#include <cstdlib>\n#include <cstring>')
    text = replace_once(
        text,
        '#include "RenderBoxInlines.h"\n',
        '#include "RenderBlockFlow.h"\n#include "RenderBlockInlines.h"\n#include "RenderBoxInlines.h"\n',
    )
    text = replace_once(
        text,
        '#include "RenderElementInlines.h"\n',
        '#include "RenderElementInlines.h"\n#include "RenderElementStyleInlines.h"\n#include "RenderObjectNode.h"\n',
    )
    text = replace_once(
        text,
        "enum class DidRepaintAndMarkContainingBlock : bool { No, Yes };\n",
        SCOPE_IMPLEMENTATION + "enum class DidRepaintAndMarkContainingBlock : bool { No, Yes };\n",
    )
    return replace_once(
        text,
        """    if (destroyRootRenderer) {
        repaintRoot(*destroyRootRenderer);
        repaintBackdropIfApplicable(*destroyRootRenderer);
        markContainingBlockDirty(*destroyRootRenderer);
    }
""",
        """    if (destroyRootRenderer) {
        bool edenBatchedIntoContainer = RenderTreeBulkTeardownScope::repaintAndMarkContainerInsteadOfRoot(root, *destroyRootRenderer);
        if (!edenBatchedIntoContainer)
            repaintRoot(*destroyRootRenderer);
        repaintBackdropIfApplicable(*destroyRootRenderer);
        if (!edenBatchedIntoContainer)
            markContainingBlockDirty(*destroyRootRenderer);
    }
""",
    )


def container_node_patch(text):
    return replace_once(
        text,
        """        Ref<Document> { document() }->nodeChildrenWillBeRemoved(*this);

        while (RefPtr child = m_firstChild.get()) {
""",
        """        Ref<Document> { document() }->nodeChildrenWillBeRemoved(*this);

        RenderTreeBulkTeardownScope edenBulkTeardownScope(*this);
        while (RefPtr child = m_firstChild.get()) {
""",
    )


PATCHES = {
    "Source/WebCore/rendering/updating/RenderTreeUpdater.h": updater_header_patch,
    "Source/WebCore/rendering/updating/RenderTreeUpdater.cpp": updater_patch,
    "Source/WebCore/dom/ContainerNode.cpp": container_node_patch,
}
