from pathlib import Path
import re
import shutil
import subprocess
import tarfile


def replace_once(text, before, after):
    if text.count(before) != 1:
        raise RuntimeError('The pinned source does not match the canvas replay patch')
    return text.replace(before, after, 1)


def prepare_canvas_replay(root, source, version, enabled=True):
    archive = root / 'third_party/wpe/packages' / f'wpewebkit-{version}.tar.xz'
    directory = 'Source/WebCore/platform/graphics/skia/'
    names = ['ImageBufferSkiaAcceleratedBackend.cpp', 'ImageBufferSkiaAcceleratedBackend.h']
    skia_path = 'Source/ThirdParty/skia/src/gpu/ganesh/GrDDLTask.cpp'
    with tarfile.open(archive) as package:
        originals = {name: package.extractfile(f'wpewebkit-{version}/{directory}{name}').read().decode() for name in names}
        original_task = package.extractfile(f'wpewebkit-{version}/{skia_path}').read().decode()
    if not enabled:
        for name, text in originals.items():
            destination = source / directory / name
            if destination.read_text() != text:
                destination.write_text(text)
        destination = source / skia_path
        if destination.read_text() != original_task:
            destination.write_text(original_task)
        (source / directory / 'EdenSkiaCanvasReplay.h').unlink(missing_ok=True)
        return
    implementation = originals[names[0]]
    implementation = replace_once(implementation, '#include "GraphicsContextSkia.h"', '#include "GraphicsContextSkia.h"\n#include "EdenSkiaCanvasReplay.h"')
    start = implementation.index('class SkiaSwitchableCanvas final')
    end = implementation.index('WTF_MAKE_TZONE_ALLOCATED_IMPL(ImageBufferSkiaAcceleratedBackend)', start)
    implementation = implementation[:start] + implementation[end:]
    implementation = replace_once(implementation, '    ensureCanvasRecordingContext();\n    return *m_canvasRecordingContext;', '    ensureCanvasRecordingContext();\n    submitCanvasRecordingIfNeeded();\n    return *m_canvasRecordingContext;')
    implementation = replace_once(implementation, '    m_switchableCanvas = makeUnique<SkiaSwitchableCanvas>(size());', '''    GrSurfaceCharacterization characterization;
    if (isMainThread() && SkiaCanvasReplay::workerCount() && m_surface->characterize(&characterization)) {
        m_canvasReplay = makeUnique<SkiaCanvasReplay>(WTF::move(characterization));
        m_switchableCanvas = makeUnique<SkiaReplayTrackingCanvas>(size());
    } else {
        m_switchableCanvas = makeUnique<SkiaSwitchableCanvas>(size());
    }''')
    implementation = replace_once(implementation, 'ImageBufferSkiaAcceleratedBackend::~ImageBufferSkiaAcceleratedBackend()\n{', 'ImageBufferSkiaAcceleratedBackend::~ImageBufferSkiaAcceleratedBackend()\n{\n    m_canvasReplay = nullptr;')
    implementation = replace_once(implementation, '    IntRect recordRect(IntPoint(), size());', '''    if (m_canvasReplay) {
        m_canvasReplay->drain(*m_surface);
    }

    IntRect recordRect(IntPoint(), size());''')
    implementation = replace_once(implementation, 'void ImageBufferSkiaAcceleratedBackend::flushContext()', '''void ImageBufferSkiaAcceleratedBackend::submitCanvasRecordingIfNeeded() {
    if (!m_canvasReplay || !m_hasActiveRecording || !m_switchableCanvas->canReplayOffThread()
        || m_switchableCanvas->recordedDrawCount() < SkiaCanvasReplay::batchSize()) {
        return;
    }
    if (!PlatformDisplay::sharedDisplay().skiaGLContext()->makeContextCurrent()) {
        return;
    }
    m_canvasRecordingContext->endRecording();
    auto picture = m_pictureRecorder.finishRecordingAsPicture();
    m_canvasReplay->submit(WTF::move(picture), *m_surface);
    auto *canvas = m_pictureRecorder.beginRecording(size().width(), size().height());
    m_switchableCanvas->switchToCanvas(canvas);
    m_canvasRecordingContext->replayStateOnCanvas(*canvas);
    m_canvasRecordingContext->beginRecording(GraphicsContextSkia::RecordingMode::Canvas);
    m_switchableCanvas->resetDrawCount();
}

void ImageBufferSkiaAcceleratedBackend::flushContext()''')
    if implementation.count('m_hasActiveRecording = true;') != 2:
        raise RuntimeError('The canvas recording lifecycle changed')
    implementation = implementation.replace(
        'm_hasActiveRecording = true;',
        'm_switchableCanvas->resetDrawCount();\n    m_hasActiveRecording = true;',
    )
    header = originals[names[1]]
    header = replace_once(header, 'class SkiaSwitchableCanvas;', 'class SkiaSwitchableCanvas;\nclass SkiaCanvasReplay;')
    header = replace_once(header, '    void ensureCanvasRecordingContext();', '    void ensureCanvasRecordingContext();\n    void submitCanvasRecordingIfNeeded();')
    header = replace_once(header, '    SkPictureRecorder m_pictureRecorder;', '    SkPictureRecorder m_pictureRecorder;\n    std::unique_ptr<SkiaCanvasReplay> m_canvasReplay;')
    task = replace_once(original_task, '    this->setFlag(kClosed_Flag);', '    this->setFlag(kBlocksReordering_Flag);\n    this->setFlag(kClosed_Flag);')
    outputs = {directory + names[0]: implementation, directory + names[1]: header, skia_path: task}
    for path, text in outputs.items():
        text = re.sub(r'/\*.*?\*/|//[^\n]*', '', text, flags=re.DOTALL)
        formatted = subprocess.run(
            ['clang-format', '--style=file:' + str(root / '.clang-format')],
            input=text, text=True, capture_output=True, check=True,
        ).stdout
        destination = source / path
        if destination.read_text() != formatted:
            destination.write_text(formatted)
    template = Path(__file__).with_name('EdenSkiaCanvasReplay.h')
    destination = source / directory / template.name
    if not destination.exists() or destination.read_bytes() != template.read_bytes():
        shutil.copy2(template, destination)
