import argparse
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from wpe.adopted_patches import load


def unified_sources(build, names):
    found = {}
    for unified in build.glob("**/unified-sources/*.cpp"):
        text = unified.read_text()
        for name in names:
            if re.search(r'[/"]' + re.escape(name) + '"', text):
                found[name] = unified
    return found


def shadow_includes(build, reference_source, source, shadow):
    listing = subprocess.check_output(
        ["find", str(build), "-type", "l", "-lname", str(reference_source) + "/*"], text=True
    ).splitlines()
    roots = {}
    for link in map(Path, listing):
        relative = link.relative_to(build)
        roots.setdefault(build / relative.parts[0] / relative.parts[1], []).append(link)
    mapping = {}
    for include, links in roots.items():
        target = shadow / include.relative_to(build)
        mapping[str(include)] = str(target)
        retargeted = set(links)
        for entry in include.rglob("*"):
            destination = target / entry.relative_to(include)
            if entry in retargeted:
                wanted = str(source / Path(os.readlink(entry)).relative_to(reference_source))
            elif entry.is_dir() and not entry.is_symlink():
                destination.mkdir(parents=True, exist_ok=True)
                continue
            else:
                wanted = str(entry)
            destination.parent.mkdir(parents=True, exist_ok=True)
            if not destination.is_symlink() or os.readlink(destination) != wanted:
                destination.unlink(missing_ok=True)
                destination.symlink_to(wanted)
    return mapping


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--module", action="append")
    parser.add_argument("--tu", action="append", default=[])
    parser.add_argument("--reference", default="frame-profile")
    parser.add_argument("--variant", default="combined")
    parser.add_argument("--cpus", default="0-7,16-23")
    parser.add_argument("--define", action="append", default=[])
    arguments = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    dependency = root / "third_party/wpe"
    versions = (root / "cmake/WpeVersion.cmake").read_text()
    version = re.search(r'EDEN_WPE_PERFORMANCE_VERSION "([^"]+)"', versions)[1]
    source = dependency / "source" / (f"wpewebkit-{version}" + (f"-{arguments.variant}" if arguments.variant else ""))
    reference_source = dependency / "source" / (
        f"wpewebkit-{version}" + (f"-{arguments.reference}" if arguments.reference else "")
    )
    build = dependency / f"build-{version}-performance-{arguments.reference}"
    patches, _, _ = load(arguments.module)
    if not (source / "CMakeLists.txt").is_file():
        parser.error("Prepare the selected source variant before checking its translation units")
    for path in patches:
        if not (source / path).is_file():
            parser.error("The selected source variant is missing " + path)
    targets = [path for path in patches if path.endswith(".cpp")] + arguments.tu
    commands = {entry["file"]: entry for entry in json.loads((build / "compile_commands.json").read_text())}
    units = unified_sources(build, {Path(path).name for path in targets})
    shadow = dependency / f"build-check-{arguments.variant}"
    includes = shadow_includes(build, reference_source, source, shadow)
    failed = False
    checked = set()
    for path in targets:
        unit = units.get(Path(path).name)
        if unit in checked:
            continue
        if unit:
            checked.add(unit)
        entry = commands.get(str(unit)) if unit else commands.get(str(reference_source / path))
        if not entry:
            print(f"No compile command covers {path}", flush=True)
            failed = True
            continue
        text = entry["command"].replace(str(reference_source), str(source))
        for include, replacement in includes.items():
            text = re.sub(re.escape(include) + r"(?=[\s/]|$)", replacement, text)
        words = shlex.split(text)
        command = []
        skip = 0
        for index, word in enumerate(words):
            if skip:
                skip -= 1
            elif word in ("-o", "-MT", "-MF"):
                skip = 1
            elif word == "-Xclang" and words[index + 1] == "-include-pch":
                skip = 3
            elif word == "-Xclang" and words[index + 1] == "-include":
                skip = 3
                prefix = shadow / Path(words[index + 3]).name
                prefix.write_text(Path(words[index + 3]).read_text().replace(str(reference_source), str(source)))
                command += ["-include", str(prefix)]
            elif word in ("-MD", "-Winvalid-pch") or word.startswith("-fprofile") or word.startswith("-flto"):
                continue
            else:
                command.append(word)
        if not unit:
            command = [word for word in command if word != str(source / path)] + [str(source / path)]
        command += ["-fsyntax-only", "-Wno-profile-instr-unprofiled", "-Wno-backend-plugin"]
        command += ["-D" + value for value in arguments.define]
        print(f"Checking {path} through {Path(entry['file']).name}", flush=True)
        result = subprocess.run(["taskset", "--cpu-list", arguments.cpus] + command, cwd=entry["directory"])
        failed |= result.returncode != 0
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
