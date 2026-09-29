from wpe.patch_support import replace_once


def compositor_patch(text):
    text = replace_once(text, '    bool shouldNotifiyDidComposite = false;', '    bool shouldNotifiyDidComposite = false;\n    bool notifyAfterSceneFlush = false;')
    text = replace_once(text, '                shouldNotifiyDidComposite = !!m_state.didCompositeRenderingUpdateFunction;', '                shouldNotifiyDidComposite = !!m_state.didCompositeRenderingUpdateFunction;\n            notifyAfterSceneFlush = shouldNotifiyDidComposite && m_state.notifyAfterSceneFlush;')
    text = replace_once(text, '    WTFEndSignpost(this, FlushCompositingState);', '''    WTFEndSignpost(this, FlushCompositingState);
    if (notifyAfterSceneFlush) {
        m_didCompositeRunLoopObserver->schedule(&RunLoop::mainSingleton());
    }''')
    text = replace_once(text, '    if (shouldNotifiyDidComposite)\n', '    if (shouldNotifiyDidComposite && !notifyAfterSceneFlush)\n')
    text = replace_once(text, 'void ThreadedCompositor::requestCompositionForRenderingUpdate(Function<void()>&& didCompositeFunction)', 'void ThreadedCompositor::requestCompositionForRenderingUpdate(Function<void()>&& didCompositeFunction, bool notifyAfterSceneFlush)')
    return replace_once(text, '    m_state.didCompositeRenderingUpdateFunction = WTF::move(didCompositeFunction);', '    m_state.didCompositeRenderingUpdateFunction = WTF::move(didCompositeFunction);\n    m_state.notifyAfterSceneFlush = notifyAfterSceneFlush;')


def compositor_header_patch(text):
    text = replace_once(text, 'void requestCompositionForRenderingUpdate(Function<void()>&&);', 'void requestCompositionForRenderingUpdate(Function<void()>&&, bool notifyAfterSceneFlush = false);')
    return replace_once(text, '        Function<void()> didCompositeRenderingUpdateFunction WTF_GUARDED_BY_LOCK(lock);', '        Function<void()> didCompositeRenderingUpdateFunction WTF_GUARDED_BY_LOCK(lock);\n        bool notifyAfterSceneFlush WTF_GUARDED_BY_LOCK(lock) { false };')


def host_patch(text):
    text = replace_once(text, '#include "config.h"', '#include "config.h"\n#include <cstdlib>\n#include <cstring>')
    text = replace_once(text, '    m_compositor->requestCompositionForRenderingUpdate([this] {', '''    static bool earlyCompletion = [] {
        const char* value = std::getenv("WEBKIT_EDEN_EARLY_SCENE_COMPLETION");
        return !value || std::strcmp(value, "0");
    }();
    const bool notifyAfterSceneFlush = earlyCompletion && !m_pendingForceRepaint && !m_forcedRepaintAsyncCallback;
    m_compositor->requestCompositionForRenderingUpdate([this, notifyAfterSceneFlush] {''')
    text = replace_once(text, '        if (!m_pendingForceRepaint && m_forcedRepaintAsyncCallback)', '        if (!notifyAfterSceneFlush && !m_pendingForceRepaint && m_forcedRepaintAsyncCallback)')
    return replace_once(text, '        WTFEndSignpost(this, DidComposite);\n    });', '        WTFEndSignpost(this, DidComposite);\n    }, notifyAfterSceneFlush);')


PATCHES = {
    'Source/WebKit/WebProcess/WebPage/CoordinatedGraphics/ThreadedCompositor.cpp': compositor_patch,
    'Source/WebKit/WebProcess/WebPage/CoordinatedGraphics/ThreadedCompositor.h': compositor_header_patch,
    'Source/WebKit/WebProcess/WebPage/CoordinatedGraphics/LayerTreeHost.cpp': host_patch,
}
