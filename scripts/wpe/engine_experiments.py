import subprocess
import tarfile

from wpe.patch_support import replace_once, without_comments
from wpe.adopted.editor_window import editor_patch
from wpe.adopted.scene_completion import compositor_patch
from wpe.adopted.scene_completion import compositor_header_patch
from wpe.adopted.scene_completion import host_patch
from wpe.adopted.helper_priority import threads_patch
from wpe.adopted.jit_pages import jit_patch
from wpe.adopted.text_pages import entry_patch
from wpe.adopted.scaled_font import font_patch
from wpe.adopted.sparse_paint import paint_patch


PATHS = {'editor': 'Source/WebKit/WebProcess/WebPage/glib/WebPageGLib.cpp', 'compositor': 'Source/WebKit/WebProcess/WebPage/CoordinatedGraphics/ThreadedCompositor.cpp', 'compositor_header': 'Source/WebKit/WebProcess/WebPage/CoordinatedGraphics/ThreadedCompositor.h', 'host': 'Source/WebKit/WebProcess/WebPage/CoordinatedGraphics/LayerTreeHost.cpp', 'threads': 'Source/WTF/wtf/Threading.cpp', 'jit': 'Source/JavaScriptCore/jit/ExecutableAllocator.cpp', 'entry': 'Source/WebKit/WebProcess/EntryPoint/unix/WebProcessMain.cpp', 'font': 'Source/WebCore/platform/graphics/skia/SkiaHarfBuzzFont.cpp', 'paint': 'Source/WebCore/platform/graphics/texmap/coordinated/CoordinatedBackingStoreProxy.cpp'}
PATCHES = {'editor': editor_patch, 'compositor': compositor_patch, 'compositor_header': compositor_header_patch, 'host': host_patch, 'threads': threads_patch, 'jit': jit_patch, 'entry': entry_patch, 'font': font_patch, 'paint': paint_patch}


def prepare_engine_experiments(root, source, version, enabled=True, names=None):
    selected = tuple(PATHS if names is None else names)
    unknown = sorted(set(selected) - set(PATHS))
    if unknown:
        raise RuntimeError("Unknown engine performance patches: " + ", ".join(unknown))
    archive = root / 'third_party/wpe/packages' / f'wpewebkit-{version}.tar.xz'
    with tarfile.open(archive) as package:
        originals = {name: package.extractfile(f'wpewebkit-{version}/{PATHS[name]}').read().decode() for name in selected}
    for name, text in originals.items():
        if enabled:
            text = PATCHES[name](text)
            text = subprocess.run(['clang-format', '--style=file:' + str(root / '.clang-format')], input=without_comments(text).replace('#include "config.h"\n', '#include "config.h"\n\n'), text=True, capture_output=True, check=True).stdout
        destination = source / PATHS[name]
        if destination.read_text() != text:
            destination.write_text(text)


