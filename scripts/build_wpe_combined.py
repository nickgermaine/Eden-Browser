import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

from wpe.performance_patches import names, resolve_patches
from wpe.pgo import profile_identity


def build_commands(root, arguments):
    common = [
        sys.executable, str(root / "scripts/build_wpe.py"), "--experiments",
        "--mode", arguments.mode, "--jobs", str(arguments.jobs), "--cpus", arguments.cpus,
    ]
    if arguments.link_jobs is not None:
        common.extend(["--link-jobs", str(arguments.link_jobs)])
    if arguments.profile is not None:
        common.extend(["--profile", str(arguments.profile.resolve())])
    if arguments.prepare_only:
        common.append("--prepare-only")
    if arguments.configure_only:
        common.append("--configure-only")
    if arguments.verify_patches:
        common.append("--verify-patches")
    commands = [common + ["--variant", "combined"]]
    if arguments.mode == "performance":
        commands.append(common + ["--variant", "combined-frame-profile", "--frame-pointers"])
    return commands


def verify_pair(root, profile):
    manifests = []
    expected = list(names(resolve_patches(True)))
    with profile.open("rb") as stream:
        profile_hash = hashlib.file_digest(stream, "sha256").hexdigest()
    for variant, frame_pointers in (("combined", False), ("combined-frame-profile", True)):
        path = root / "third_party/wpe" / ("local-performance-" + variant) / "share/eden-wpe-build.json"
        manifest = json.loads(path.read_text())
        if (manifest.get("variant") != variant or manifest.get("mode") != "performance"
                or manifest.get("framePointers") != frame_pointers or manifest.get("patches") != expected
                or manifest.get("profileSHA256") != profile_hash):
            raise RuntimeError("Combined runtime identity mismatch: " + str(path))
        manifests.append(manifest)
    try:
        identities = [profile_identity(manifest) for manifest in manifests]
        for manifest, identity in zip(manifests, identities):
            if profile_identity(manifest.get("profileTrainingIdentity")) != identity:
                raise RuntimeError("Combined runtime PGO identity does not match its training profile")
        if identities[0] != identities[1]:
            raise RuntimeError("Combined runtimes disagree on their canonical PGO identity")
    except RuntimeError as error:
        raise RuntimeError("Combined runtimes do not share one canonical PGO identity") from error
    return manifests


def main():
    parser = argparse.ArgumentParser(description="Build the combined scoring and profiling runtimes in sequence")
    parser.add_argument("--mode", choices=("profile-generate", "performance"), default="performance")
    parser.add_argument("--profile", type=Path)
    parser.add_argument("--jobs", type=int, default=16)
    parser.add_argument("--link-jobs", type=int)
    parser.add_argument("--cpus", default="0-7,16-23")
    parser.add_argument("--prepare-only", action="store_true")
    parser.add_argument("--configure-only", action="store_true")
    parser.add_argument("--verify-patches", action="store_true")
    arguments = parser.parse_args()
    if arguments.prepare_only and arguments.configure_only:
        parser.error("Choose either source preparation or configuration")
    if arguments.mode == "profile-generate" and arguments.profile:
        parser.error("Profile generation cannot consume a profile")
    if arguments.mode == "performance" and not arguments.prepare_only:
        if not arguments.profile or not arguments.profile.is_file():
            parser.error("The combined performance build requires the freshly merged --profile")
    root = Path(__file__).resolve().parents[1]
    for command in build_commands(root, arguments):
        subprocess.run(command, cwd=root, check=True)
    if arguments.mode == "performance" and not arguments.prepare_only and not arguments.configure_only:
        verify_pair(root, arguments.profile)
        print("Both combined manifests match the selected registry and merged profile")


if __name__ == "__main__":
    main()
