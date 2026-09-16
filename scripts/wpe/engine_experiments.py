from pathlib import Path
import re
import shutil
import subprocess
import tarfile


PATHS = {
    'editor': 'Source/WebKit/WebProcess/WebPage/glib/WebPageGLib.cpp',
}


def replace_once(text, before, after):
    if text.count(before) != 1:
        raise RuntimeError('The pinned source does not match the engine experiment: ' + before[:100])
    return text.replace(before, after, 1)


def without_comments(text):
    tokens = re.compile(r'R"(?P<delimiter>[^ ()\\\t\r\n]{0,16})\(.*?\)(?P=delimiter)"|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/', re.DOTALL)
    return tokens.sub(lambda match: '\n' * match[0].count('\n') if match[0].startswith('/') else match[0], text)


def editor_patch(text):
    text = replace_once(text, '#include <wtf/text/MakeString.h>', '#include <wtf/text/MakeString.h>\n#include <wtf/SystemTracing.h>\n#include <cstdlib>')
    helper = '''static SimpleRange edenSurroundingWindow(const SimpleRange& fullRange, const SimpleRange& selectionRange) {
    static unsigned limit = [] {
        const char* setting = std::getenv("WEBKIT_EDEN_EDITOR_WINDOW");
        char* end = nullptr;
        unsigned long value = setting ? std::strtoul(setting, &end, 10) : 0;
        return setting && end != setting && !*end && value >= 128 && value <= 16384 ? static_cast<unsigned>(value) : 0U;
    }();
    if (!limit || !contains<ComposedTree>(fullRange, selectionRange)) {
        return fullRange;
    }
    CharacterIterator selected(selectionRange);
    selected.advance(limit);
    if (!selected.atEnd()) {
        return fullRange;
    }
    BackwardsCharacterIterator before({ fullRange.start, selectionRange.start });
    before.advance(limit - 1);
    if (!before.atEnd() && !before.text().isEmpty() && U16_IS_TRAIL(before.text()[before.text().length() - 1])) {
        before.advance(1);
    }
    CharacterIterator after({ selectionRange.end, fullRange.end });
    after.advance(limit);
    if (!after.atEnd() && !after.text().isEmpty() && U16_IS_TRAIL(after.text()[0])) {
        after.advance(1);
    }
    return { before.atEnd() ? fullRange.start : before.range().start, after.atEnd() ? fullRange.end : after.range().start };
}

'''
    text = replace_once(text, 'void WebPage::getPlatformEditorState(', helper + 'void WebPage::getPlatformEditorState(')
    text = replace_once(text, '        auto compositionRange = frame.editor().compositionRange();', '''        auto compositionRange = frame.editor().compositionRange();
        WTFBeginSignpost(this, EdenEditorSurroundingText);
        auto selectedRange = makeSimpleRange(selection.visibleStart(), selection.visibleEnd());
        if (surroundingRange && selectedRange) {
            auto window = edenSurroundingWindow(*surroundingRange, *selectedRange);
            if (!compositionRange || contains<ComposedTree>(window, *compositionRange)) {
                surroundingRange = WTF::move(window);
            }
        }''')
    text = replace_once(text, 'auto cursorPositionRange = makeSimpleRange(surroundingStart, selectionStart);', 'auto cursorPositionRange = surroundingRange ? makeSimpleRange(surroundingRange->start, selectionStart) : std::nullopt;')
    text = replace_once(text, 'auto selectionPositionRange = makeSimpleRange(surroundingStart, selection.visibleEnd());', 'auto selectionPositionRange = surroundingRange ? makeSimpleRange(surroundingRange->start, selection.visibleEnd()) : std::nullopt;')
    anchor = '            postLayoutData.surroundingContextSelectionPosition = selectionPositionRange ? characterCount(*selectionPositionRange): 0;\n        }'
    text = replace_once(text, anchor, anchor + '''
        WTFEndSignpost(this, EdenEditorSurroundingText);
        if (std::getenv("WEBKIT_EDEN_EXPERIMENT_TRACE")) {
            WTFLogAlways("Eden editor window: length=%u cursor=%u selection=%u", postLayoutData.surroundingContext.length(), postLayoutData.surroundingContextCursorPosition, postLayoutData.surroundingContextSelectionPosition);
        }''')
    return text


PATCHES = {'editor': editor_patch}


def prepare_engine_experiments(root, source, version, enabled=True):
    archive = root / 'third_party/wpe/packages' / f'wpewebkit-{version}.tar.xz'
    with tarfile.open(archive) as package:
        originals = {name: package.extractfile(f'wpewebkit-{version}/{path}').read().decode() for name, path in PATHS.items()}
    for name, text in originals.items():
        if enabled:
            text = PATCHES[name](text)
            text = subprocess.run(['clang-format', '--style=file:' + str(root / '.clang-format')], input=without_comments(text).replace('#include "config.h"\n', '#include "config.h"\n\n'), text=True, capture_output=True, check=True).stdout
        destination = source / PATHS[name]
        if destination.read_text() != text:
            destination.write_text(text)


def compositor_header_patch(text):
    text = replace_once(text, 'void requestCompositionForRenderingUpdate(Function<void()>&&);', 'void requestCompositionForRenderingUpdate(Function<void()>&&, bool notifyAfterSceneFlush = false);')
    return replace_once(text, '        Function<void()> didCompositeRenderingUpdateFunction WTF_GUARDED_BY_LOCK(lock);', '        Function<void()> didCompositeRenderingUpdateFunction WTF_GUARDED_BY_LOCK(lock);\n        bool notifyAfterSceneFlush WTF_GUARDED_BY_LOCK(lock) { false };')


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


def host_patch(text):
    text = replace_once(text, '#include "config.h"', '#include "config.h"\n#include <cstdlib>\n#include <cstring>')
    text = replace_once(text, '    m_compositor->requestCompositionForRenderingUpdate([this] {', '''    static bool earlyCompletion = [] {
        const char* value = std::getenv("WEBKIT_EDEN_EARLY_SCENE_COMPLETION");
        return value && !std::strcmp(value, "1");
    }();
    const bool notifyAfterSceneFlush = earlyCompletion && !m_pendingForceRepaint && !m_forcedRepaintAsyncCallback;
    m_compositor->requestCompositionForRenderingUpdate([this, notifyAfterSceneFlush] {''')
    text = replace_once(text, '        if (!m_pendingForceRepaint && m_forcedRepaintAsyncCallback)', '        if (!notifyAfterSceneFlush && !m_pendingForceRepaint && m_forcedRepaintAsyncCallback)')
    return replace_once(text, '        WTFEndSignpost(this, DidComposite);\n    });', '        WTFEndSignpost(this, DidComposite);\n    }, notifyAfterSceneFlush);')


def threads_patch(text):
    text = replace_once(text, '#include <wtf/linux/RealTimeThreads.h>', '#include <wtf/linux/RealTimeThreads.h>\n#include <sys/resource.h>\n#include <cstdlib>\n#include <cstring>')
    anchor = '        Thread::initializeCurrentThreadInternal(context->name);'
    return replace_once(text, anchor, anchor + '''
#if OS(LINUX)
        static int helperNice = [] {
            const char* setting = std::getenv("WEBKIT_EDEN_HELPER_NICE");
            char* end = nullptr;
            long value = setting ? std::strtol(setting, &end, 10) : 0;
            return setting && end != setting && !*end && value >= 1 && value <= 19 ? static_cast<int>(value) : 0;
        }();
        const char* name = context->name.characters();
        if (helperNice && (!std::strcmp(name, "JITWorker") || !std::strcmp(name, "HeapHelper") || !std::strcmp(name, "SkiaCPUWorker") || !std::strcmp(name, "CanvasDDL"))) {
            if (getpriority(PRIO_PROCESS, 0) < helperNice) {
                int result = setpriority(PRIO_PROCESS, 0, helperNice);
                if (std::getenv("WEBKIT_EDEN_EXPERIMENT_TRACE")) {
                    WTFLogAlways("Eden helper priority: %s nice=%d result=%d", name, getpriority(PRIO_PROCESS, 0), result);
                }
            }
        }
#endif''')


def jit_patch(text):
    text = replace_once(text, '#include "config.h"', '#include "config.h"\n#if OS(LINUX)\n#include <sys/mman.h>\n#include <cstdlib>\n#include <cstring>\n#endif')
    anchor = '        g_jscConfig.endExecutableMemory = reservationEnd;'
    return replace_once(text, anchor, anchor + '''
#if OS(LINUX)
        const char* hugePages = std::getenv("WEBKIT_EDEN_JIT_HUGE_PAGES");
        if (hugePages && !std::strcmp(hugePages, "1")) {
            constexpr uintptr_t alignment = 2 * 1024 * 1024;
            uintptr_t begin = (reinterpret_cast<uintptr_t>(reservation.base) + alignment - 1) & ~(alignment - 1);
            uintptr_t end = reinterpret_cast<uintptr_t>(reservationEnd) & ~(alignment - 1);
            if (end > begin) {
                int result = madvise(reinterpret_cast<void*>(begin), end - begin, MADV_HUGEPAGE);
                if (std::getenv("WEBKIT_EDEN_EXPERIMENT_TRACE")) {
                    dataLog("Eden JIT huge pages: advised ", end - begin, " bytes result=", result, "\\n");
                }
            }
        }
#endif''')


def entry_patch(text):
    text = replace_once(text, '#include "WebProcessMain.h"', '''#include "WebProcessMain.h"
#if PLATFORM(WPE) && OS(LINUX)
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <link.h>
#include <sys/mman.h>

static void edenAdviseTextHugePages() {
    const char* setting = std::getenv("WEBKIT_EDEN_TEXT_HUGE_PAGES");
    if (!setting || std::strcmp(setting, "1")) {
        return;
    }
    dl_iterate_phdr([](dl_phdr_info* information, size_t, void*) -> int {
        if (!information->dlpi_name || !std::strstr(information->dlpi_name, "libWPEWebKit-2.0.so")) {
            return 0;
        }
        constexpr uintptr_t alignment = 2 * 1024 * 1024;
        for (unsigned index = 0; index < information->dlpi_phnum; ++index) {
            const auto& segment = information->dlpi_phdr[index];
            if (segment.p_type != PT_LOAD || segment.p_flags != (PF_R | PF_X)) {
                continue;
            }
            uintptr_t start = (information->dlpi_addr + segment.p_vaddr + alignment - 1) & ~(alignment - 1);
            uintptr_t end = (information->dlpi_addr + segment.p_vaddr + segment.p_memsz) & ~(alignment - 1);
            if (end > start) {
                int result = madvise(reinterpret_cast<void*>(start), end - start, MADV_HUGEPAGE);
                if (std::getenv("WEBKIT_EDEN_EXPERIMENT_TRACE")) {
                    std::fprintf(stderr, "Eden text huge pages: advised %zu bytes result=%d\\n", static_cast<size_t>(end - start), result);
                }
            }
        }
        return 1;
    }, nullptr);
}
#endif''')
    return replace_once(text, '    return WebKit::WebProcessMain(argc, argv);', '#if PLATFORM(WPE) && OS(LINUX)\n    edenAdviseTextHugePages();\n#endif\n    return WebKit::WebProcessMain(argc, argv);')


def font_patch(text):
    text = replace_once(text, '#include "config.h"', '#include "config.h"\n#include <cstdlib>\n#include <cstring>')
    anchor = '    float size = fontPlatformData.size();'
    return replace_once(text, anchor, anchor + '''
    static bool reuseScaledFont = [] {
        const char* value = std::getenv("WEBKIT_EDEN_REUSE_SCALED_FONT");
        return value && !std::strcmp(value, "1");
    }();
    if (reuseScaledFont && m_scaledFont == fontPlatformData.skFont() && m_isColorBitmapFont == fontPlatformData.isColorBitmapFont() && hb_font_get_ptem(m_font.get()) == size) {
        return m_font.get();
    }''')


def paint_patch(text):
    text = replace_once(text, '    unsigned dirtyTilesCount = 0;', '    unsigned dirtyTilesCount = 0;\n    uint64_t dirtyArea = 0;')
    text = replace_once(text, '        tileDirtyRectUnion.unite(tile.dirtyRect);', '        tileDirtyRectUnion.unite(tile.dirtyRect);\n        dirtyArea += static_cast<uint64_t>(tile.dirtyRect.width()) * tile.dirtyRect.height();')
    text = replace_once(text, '        auto recording = paintingEngine.record(*layer.owner(), tileDirtyRectUnion, contentsOpaque, contentsScale, dirtyTilesCount);', '''        static unsigned partitionRatio = [] {
            const char* value = getenv("WEBKIT_EDEN_PAINT_PARTITION_RATIO");
            auto ratio = value ? parseInteger<unsigned>(StringView::fromLatin1(value)) : std::nullopt;
            return ratio && *ratio >= 1 && *ratio <= 64 ? *ratio : 0U;
        }();
        uint64_t unionArea = static_cast<uint64_t>(tileDirtyRectUnion.width()) * tileDirtyRectUnion.height();
        bool partition = partitionRatio && dirtyTilesCount > 1 && unionArea > dirtyArea * partitionRatio;
        RefPtr<SkiaRecordingResult> recording;
        if (!partition) {
            recording = paintingEngine.record(*layer.owner(), tileDirtyRectUnion, contentsOpaque, contentsScale, dirtyTilesCount);
        }
        if (getenv("WEBKIT_EDEN_EXPERIMENT_TRACE")) {
            WTFLogAlways("Eden paint regions: tiles=%u union=%llu dirty=%llu partition=%d", dirtyTilesCount, static_cast<unsigned long long>(unionArea), static_cast<unsigned long long>(dirtyArea), partition);
        }''')
    return replace_once(text, '            auto buffer = paintingEngine.replay(*layer.owner(), recording.copyRef(), tile.rect, tile.dirtyRect);', '''            auto tileRecording = partition ? paintingEngine.record(*layer.owner(), tile.dirtyRect, contentsOpaque, contentsScale, 1) : Ref { *recording };
            auto buffer = paintingEngine.replay(*layer.owner(), WTF::move(tileRecording), tile.rect, tile.dirtyRect);''')


PATHS.update({
    'compositor': 'Source/WebKit/WebProcess/WebPage/CoordinatedGraphics/ThreadedCompositor.cpp',
    'compositor_header': 'Source/WebKit/WebProcess/WebPage/CoordinatedGraphics/ThreadedCompositor.h',
    'host': 'Source/WebKit/WebProcess/WebPage/CoordinatedGraphics/LayerTreeHost.cpp',
    'threads': 'Source/WTF/wtf/Threading.cpp',
    'entry': 'Source/WebKit/WebProcess/EntryPoint/unix/WebProcessMain.cpp',
    'jit': 'Source/JavaScriptCore/jit/ExecutableAllocator.cpp',
    'font': 'Source/WebCore/platform/graphics/skia/SkiaHarfBuzzFont.cpp',
    'paint': 'Source/WebCore/platform/graphics/texmap/coordinated/CoordinatedBackingStoreProxy.cpp',
})
PATCHES.update({'compositor': compositor_patch, 'compositor_header': compositor_header_patch, 'host': host_patch, 'threads': threads_patch, 'entry': entry_patch, 'jit': jit_patch, 'font': font_patch, 'paint': paint_patch})
