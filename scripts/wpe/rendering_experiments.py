from pathlib import Path
import subprocess
import tarfile

from wpe.engine_experiments import replace_once, without_comments


GRAPHICS = "Source/WebCore/platform/graphics/"


def font_patch(text):
    return replace_once(
        text,
        "#if !USE(CORE_TEXT)\nGlyphBufferAdvance Font::applyTransforms",
        "#if !USE(CORE_TEXT) && !(PLATFORM(WPE) && USE(SKIA))\nGlyphBufferAdvance Font::applyTransforms",
    )


def skia_font_patch(text):
    text = replace_once(
        text,
        '#include "GlyphBuffer.h"',
        '#include "GlyphBuffer.h"\n#include "HbUniquePtr.h"\n#include <hb.h>',
    )
    implementation = Path(__file__).with_name("EdenHarfBuzzTransforms.inc").read_text()
    return replace_once(
        text,
        "Path Font::platformPathForGlyph",
        "#if PLATFORM(WPE)\n"
        + implementation
        + "\n#endif\n\nPath Font::platformPathForGlyph",
    )


def cascade_patch(text):
    text = replace_once(
        text,
        '#include "config.h"',
        '#include "config.h"\n#include <cstdlib>\n#include <cstring>',
    )
    begin = "#if PLATFORM(GTK) || PLATFORM(WPE)\n    TextRun run { text, 0, 0, ExpansionBehavior::defaultBehavior(), textDirection, false, false };\n    float result = width(CodePath::Simple, run);\n#else\n    GlyphBuffer glyphBuffer;"
    replacement = """#if PLATFORM(GTK) || PLATFORM(WPE)
    bool useTransforms = false;
#if USE(SKIA) && PLATFORM(WPE)
    static bool enabled = [] {
        auto* value = std::getenv("WEBKIT_EDEN_SIMPLE_TEXT");
        return value && !std::strcmp(value, "1");
    }();
    useTransforms = enabled && (enableKerning() || requiresShaping())
        && primaryFont().platformData().orientation() == FontOrientation::Horizontal
        && fontDescription().variantCaps() == FontVariantCaps::Normal
        && fontDescription().variantEmoji() != FontVariantEmoji::Emoji;
    if (useTransforms) {
        bool hasLatinLetter = false;
        for (unsigned i = 0; i < text.length(); ++i) {
            auto character = text[i];
            if (character < 0x20 || (character >= 0x7f && character <= 0xa0) || character == 0xad || character == 0xb5 || character > 0xff) {
                useTransforms = false;
                break;
            }
            hasLatinLetter |= (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z')
                || (character >= 0xc0 && character != 0xd7 && character != 0xf7) || character == 0xaa || character == 0xba;
        }
        useTransforms &= hasLatinLetter;
    }
#endif
    if (!useTransforms) {
        TextRun run { text, 0, 0, ExpansionBehavior::defaultBehavior(), textDirection, false, false };
        float result = width(CodePath::Simple, run);
        if (cacheEntry) {
            cacheEntry->width = result;
        }
        return result;
    }
#endif
    GlyphBuffer glyphBuffer;"""
    text = replace_once(text, begin, replacement)
    return replace_once(
        text,
        "    result += WebCore::width(initialAdvance);\n#endif\n    if (cacheEntry)",
        "    result += WebCore::width(initialAdvance);\n    if (cacheEntry)",
    )


def canvas_header_patch(text):
    text = replace_once(
        text,
        "#include <wtf/Forward.h>",
        "#include <wtf/Forward.h>\n#include <wtf/WeakPtr.h>",
    )
    return replace_once(
        text,
        "    bool m_ignoreDidUpdateSizeProperties",
        "    SingleThreadWeakPtr<RenderBox> m_edenFullyDirtyRenderer;\n    bool m_ignoreDidUpdateSizeProperties",
    )


def canvas_patch(text):
    text = replace_once(
        text,
        '#include "config.h"',
        '#include "config.h"\n#include <cstdlib>\n#include <cstring>',
    )
    start = text.index("void HTMLCanvasElement::didDraw(")
    end = text.index("\nvoid HTMLCanvasElement::didUpdateSizeProperties()", start)
    text = (
        text[:start]
        + """void HTMLCanvasElement::didDraw(const std::optional<FloatRect>& rect, ShouldApplyPostProcessingToDirtyRect shouldApplyPostProcessingToDirtyRect) {
    static bool coalesce = [] {
        auto* value = std::getenv("WEBKIT_EDEN_CANVAS_DAMAGE");
        return value && !std::strcmp(value, "1");
    }();
    clearCopiedImage();
    if (CheckedPtr renderer = renderBox()) {
        bool layerContents = usesContentsAsLayerContents();
        bool eligible = coalesce && layerContents && is<CanvasRenderingContext2D>(m_context.get());
        if (!eligible || m_edenFullyDirtyRenderer.get() != renderer.get()) {
            const std::optional<FloatRect> dirtyRect = computeDirtyRectangleIfNeeded(rect);
            if (layerContents) {
                renderer->contentChanged(ContentChangeType::CanvasPixels, dirtyRect);
                if (eligible && (!rect || rect->contains(FloatRect { { }, size() }))) {
                    m_edenFullyDirtyRenderer = renderer.get();
                }
            } else if (dirtyRect) {
                renderer->repaintRectangle(enclosingIntRect(*dirtyRect));
            }
        }
    }
    CanvasBase::didDraw(rect, shouldApplyPostProcessingToDirtyRect);
}
"""
        + text[end:]
    )
    for anchor in [
        "void HTMLCanvasElement::didUpdateSizeProperties()\n{",
        "void HTMLCanvasElement::paint(GraphicsContext& context, const LayoutRect& r)\n{",
        "void HTMLCanvasElement::didMoveToNewDocument(Document& oldDocument, Document& newDocument)\n{",
        "void HTMLCanvasElement::prepareForDisplay()\n{",
    ]:
        text = replace_once(
            text, anchor, anchor + "\n    m_edenFullyDirtyRenderer = nullptr;"
        )
    return text


PATCHES = {
    GRAPHICS + "Font.cpp": font_patch,
    GRAPHICS + "FontCascade.cpp": cascade_patch,
    GRAPHICS + "skia/FontSkia.cpp": skia_font_patch,
    "Source/WebCore/html/HTMLCanvasElement.h": canvas_header_patch,
    "Source/WebCore/html/HTMLCanvasElement.cpp": canvas_patch,
}


def prepare_rendering_experiments(root, source, version, enabled):
    archive = root / "third_party/wpe/packages" / f"wpewebkit-{version}.tar.xz"
    with tarfile.open(archive) as package:
        originals = {
            path: package.extractfile(f"wpewebkit-{version}/{path}").read().decode()
            for path in PATCHES
        }
    for path, original in originals.items():
        text = original
        if enabled:
            text = PATCHES[path](original)
            text = subprocess.run(
                ["clang-format", "--style=file:" + str(root / ".clang-format")],
                input=without_comments(text).replace(
                    '#include "config.h"\n', '#include "config.h"\n\n'
                ),
                text=True,
                capture_output=True,
                check=True,
            ).stdout
        destination = source / path
        if destination.read_text() != text:
            destination.write_text(text)

    if enabled:
        prepare_cpu_raster(root, source)
    else:
        (source / (GRAPHICS + "skia/EdenSkiaCanvasCpuRaster.h")).unlink(missing_ok=True)


def prepare_cpu_raster(root, source):
    directory = source / (GRAPHICS + "skia")
    header_path = directory / "ImageBufferSkiaAcceleratedBackend.h"
    header = header_path.read_text()
    header = replace_once(
        header,
        "class SkiaCanvasReplay;",
        "class SkiaCanvasReplay;\nclass SkiaCanvasCpuRaster;",
    )
    header = replace_once(
        header,
        "std::unique_ptr<SkiaCanvasReplay> m_canvasReplay;",
        "std::unique_ptr<SkiaCanvasReplay> m_canvasReplay;\nRefPtr<SkiaCanvasCpuRaster> m_canvasCpuRaster;",
    )
    path = directory / "ImageBufferSkiaAcceleratedBackend.cpp"
    text = path.read_text()
    text = replace_once(
        text,
        '#include "EdenSkiaCanvasReplay.h"',
        '#include "EdenSkiaCanvasReplay.h"\n#include "EdenSkiaCanvasCpuRaster.h"',
    )
    text = replace_once(
        text,
        "        m_canvasReplay = nullptr;",
        "        if (m_canvasCpuRaster) { m_canvasCpuRaster->wait(); }\n        m_canvasReplay = nullptr;",
    )
    anchor = "        GrSurfaceCharacterization characterization;\n        if (isMainThread() && SkiaCanvasReplay::workerCount() && m_surface->characterize(&characterization)) {"
    text = replace_once(
        text,
        anchor,
        """        if (isMainThread()) {
            m_canvasCpuRaster = SkiaCanvasCpuRaster::create(*m_surface);
        }
        GrSurfaceCharacterization characterization;
        if (m_canvasCpuRaster) {
            m_switchableCanvas = makeUnique<SkiaReplayTrackingCanvas>(size());
        } else if (isMainThread() && SkiaCanvasReplay::workerCount() && m_surface->characterize(&characterization)) {""",
    )
    anchor = "        if (m_canvasReplay) {\n            m_canvasReplay->drain(*m_surface);\n        }"
    text = replace_once(
        text,
        anchor,
        """        if (m_canvasCpuRaster) {
            if (m_switchableCanvas->canReplayOffThread()) {
                m_canvasRecordingContext->endRecording();
                m_canvasCpuRaster->submit(m_pictureRecorder.finishRecordingAsPicture());
                m_canvasCpuRaster->upload(*m_surface);
                auto* canvas = m_pictureRecorder.beginRecording(size().width(), size().height());
                m_switchableCanvas->switchToCanvas(canvas);
                m_canvasRecordingContext->replayStateOnCanvas(*canvas);
                m_canvasRecordingContext->beginRecording(GraphicsContextSkia::RecordingMode::Canvas);
                m_switchableCanvas->resetDrawCount();
                return;
            }
            if (!m_canvasCpuRaster->upload(*m_surface)) {
                return;
            }
            m_canvasCpuRaster = nullptr;
        }
"""
        + anchor,
    )
    text = replace_once(
        text,
        "if (!m_canvasReplay || !m_hasActiveRecording ||",
        "if ((!m_canvasReplay && !m_canvasCpuRaster) || !m_hasActiveRecording ||",
    )
    text = replace_once(
        text,
        "        m_canvasReplay->submit(WTF::move(picture), *m_surface);",
        """        if (m_canvasCpuRaster) {
            m_canvasCpuRaster->submit(WTF::move(picture));
        } else {
            m_canvasReplay->submit(WTF::move(picture), *m_surface);
        }""",
    )
    text = replace_once(
        text,
        "        if (m_canvasRecordingContext) {\n            m_surface->getCanvas()->restoreToCount(1);",
        "        if (m_canvasRecordingContext && !m_canvasCpuRaster) {\n            m_surface->getCanvas()->restoreToCount(1);",
    )
    anchor = "        replayCanvasRecordingContextIfNeeded();\n\n        UNUSED_PARAM(destFormat);"
    text = replace_once(
        text,
        anchor,
        """        replayCanvasRecordingContextIfNeeded();
        m_canvasCpuRaster = nullptr;

        UNUSED_PARAM(destFormat);""",
    )
    for destination, contents in [(header_path, header), (path, text)]:
        formatted = subprocess.run(
            ["clang-format", "--style=file:" + str(root / ".clang-format")],
            input=contents,
            text=True,
            capture_output=True,
            check=True,
        ).stdout
        if destination.read_text() != formatted:
            destination.write_text(formatted)
    target = directory / "EdenSkiaCanvasCpuRaster.h"
    contents = Path(__file__).with_name(target.name).read_text()
    if not target.exists() or target.read_text() != contents:
        target.write_text(contents)


def code_cache_patch(text):
    text = replace_once(
        text,
        "#include <wtf/ApproximateTime.h>",
        "#include <wtf/ApproximateTime.h>\n#include <wtf/MonotonicTime.h>\n#include <wtf/ProcessID.h>\n#include <cstdio>\n#include <cstdlib>\n#include <cstring>",
    )
    text = replace_once(
        text,
        "        iterator findResult = m_map.find(key);",
        """        iterator findResult = m_map.find(key);
        static bool trace = [] {
            auto* value = std::getenv("WEBKIT_EDEN_CODE_CACHE_TRACE");
            return value && !std::strcmp(value, "1");
        }();
        if (trace && key.length() >= 4096) {
            std::fprintf(stderr, "Eden code cache: pid=%lld time=%.6f hash=%u units=%zu hit=%d size=%lld capacity=%lld\\n", static_cast<long long>(getCurrentProcessID()), MonotonicTime::now().secondsSinceEpoch().value(), key.hash(), key.length(), findResult != m_map.end(), static_cast<long long>(m_size), static_cast<long long>(m_capacity));
        }""",
    )
    return text


PATCHES["Source/JavaScriptCore/runtime/CodeCache.h"] = code_cache_patch
