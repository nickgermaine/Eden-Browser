import subprocess
import tarfile

from wpe.patch_support import replace_once, without_comments
from wpe.adopted.jsc_switch import switched
from wpe.adopted.jsc_assign import assign_patch
from wpe.adopted.jsc_rest import rest_patch
from wpe.adopted.jsc_regexp import regexp_patch


PATCHES = {
    "Source/JavaScriptCore/runtime/ObjectConstructor.cpp": assign_patch,
    "Source/JavaScriptCore/runtime/JSGlobalObjectFunctions.cpp": rest_patch,
    "Source/JavaScriptCore/yarr/YarrJIT.cpp": regexp_patch,
}

SELECTORS = {
    "assign": "Source/JavaScriptCore/runtime/ObjectConstructor.cpp",
    "rest": "Source/JavaScriptCore/runtime/JSGlobalObjectFunctions.cpp",
    "regexp": "Source/JavaScriptCore/yarr/YarrJIT.cpp",
}


def prepare_jsc_experiments(root, source, version, enabled=True, names=None):
    selected = tuple(SELECTORS if names is None else names)
    unknown = sorted(set(selected) - set(SELECTORS))
    if unknown:
        raise RuntimeError("Unknown JavaScriptCore performance patches: " + ", ".join(unknown))
    paths = tuple(SELECTORS[name] for name in selected)
    archive = root / "third_party/wpe/packages" / f"wpewebkit-{version}.tar.xz"
    with tarfile.open(archive) as package:
        originals = {path: package.extractfile(f"wpewebkit-{version}/{path}").read().decode() for path in paths}
    for path, original in originals.items():
        text = original
        if enabled:
            text = PATCHES[path](text)
            text = subprocess.run(
                ["clang-format", "--style=file:" + str(root / ".clang-format")],
                input=without_comments(text).replace('#include "config.h"\n', '#include "config.h"\n\n'),
                text=True, capture_output=True, check=True,
            ).stdout
        destination = source / path
        if destination.read_text() != text:
            destination.write_text(text)
