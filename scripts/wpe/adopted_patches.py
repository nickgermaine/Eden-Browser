import importlib
from pathlib import Path
import subprocess
import tarfile

from wpe.patch_support import without_cmake_comments, without_comments, without_source_list_comments


MODULES = [
    "editor_window",
    "scene_completion",
    "helper_priority",
    "jit_pages",
    "text_pages",
    "scaled_font",
    "sparse_paint",
    "jsc_assign",
    "jsc_rest",
    "jsc_regexp",
    "jsc_json_keys",
    "simple_text",
    "text_cache_keys",
    "canvas_thread",
    "bulk_teardown",
    "ua_rule_memo",
    "flex_invalidation",
    "text_control",
    "matched_declarations",
    "process_prewarm",
    "navigation_preconnect",
    "opportunistic_yield",
    "intrinsic_width_probe",
    "intrinsic_width",
    "list_renumbering",
    "author_rule_memo",
    "collection_index",
    "svg_length_parse",
    "removal_aggregate",
    "selector_filter_storage",
]


def load(names=None):
    patches = {}
    files = {}
    sources = [Path(__file__), Path(__file__).with_name("patch_support.py")]
    selected = tuple(MODULES if names is None else names)
    if len(selected) != len(set(selected)) or set(selected) - set(MODULES):
        raise RuntimeError("Unknown or duplicate adopted patch selection: " + ", ".join(selected))
    for name in selected:
        path = Path(__file__).with_name("adopted") / f"{name}.py"
        if not path.is_file():
            raise RuntimeError("Missing adopted patch module: " + str(path))
        module = importlib.import_module(f"wpe.adopted.{name}")
        sources.append(path)
        for target, patch in module.PATCHES.items():
            patches.setdefault(target, []).append(patch)
        for target, payload in getattr(module, "FILES", {}).items():
            if target in files:
                raise RuntimeError("Duplicate generated patch file: " + target)
            files[target] = Path(__file__).with_name("adopted") / payload
            sources.append(files[target])
        for payload in getattr(module, "SOURCES", []):
            sources.append(Path(__file__).with_name("adopted") / payload)
    return patches, files, list(dict.fromkeys(sources))


def adopted_sources(names=None):
    return load(names)[2]


def render(root, original, patches, path="source.cpp"):
    text = original
    for patch in patches:
        text = patch(text)
    if Path(path).suffix == ".cmake":
        return without_cmake_comments(text)
    if Path(path).suffix == ".txt":
        return without_source_list_comments(text)
    text = without_comments(text)
    if Path(path).suffix in (".idl", ".json", ".in"):
        return text
    return subprocess.run(
        ["clang-format", "--style=file:" + str(root / ".clang-format")],
        input=text.replace('#include "config.h"\n', '#include "config.h"\n\n'),
        text=True,
        capture_output=True,
        check=True,
    ).stdout


def prepare_adopted_patches(root, source, version, enabled, names=None, skip=()):
    patches, files, _ = load(names)
    archive = root / "third_party/wpe/packages" / f"wpewebkit-{version}.tar.xz"
    if enabled:
        originals = {path: (source / path).read_text() for path in patches if path not in skip}
    else:
        with tarfile.open(archive) as package:
            originals = {
                path: package.extractfile(f"wpewebkit-{version}/{path}").read().decode()
                for path in patches
                if path not in skip
            }
    for path, original in originals.items():
        text = render(root, original, patches[path], path) if enabled else original
        destination = source / path
        if destination.read_text() != text:
            destination.write_text(text)
    for path, payload in files.items():
        destination = source / path
        if not enabled:
            destination.unlink(missing_ok=True)
            continue
        contents = payload.read_text()
        if not destination.exists() or destination.read_text() != contents:
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(contents)
