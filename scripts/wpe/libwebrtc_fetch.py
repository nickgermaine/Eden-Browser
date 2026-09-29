import argparse
import concurrent.futures
import contextlib
import hashlib
import json
import lzma
from pathlib import Path
import re
import tarfile
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

API = "https://api.github.com/repos/WebKit/WebKit"
RAW = "https://raw.githubusercontent.com/WebKit/WebKit"
SUBTREE = "Source/ThirdParty/libwebrtc"
WORKERS = 8


def pinned_release(root):
    versions = (root / "cmake" / "WpeVersion.cmake").read_text()
    version = re.search(r'EDEN_WPE_PERFORMANCE_VERSION "([^"]+)"', versions)[1]
    commit = re.search(r'EDEN_LIBWEBRTC_COMMIT "([^"]+)"', versions)
    return version, commit[1] if commit else None


def request_json(path, attempts=5):
    last_error = None
    for attempt in range(attempts):
        try:
            request = urllib.request.Request(
                API + path, headers={"Accept": "application/vnd.github+json", "User-Agent": "eden-libwebrtc-fetch"}
            )
            with urllib.request.urlopen(request, timeout=60) as response:
                return json.load(response)
        except Exception as error:
            last_error = error
            time.sleep(2 * (attempt + 1))
    raise RuntimeError(f"GitHub API request failed: {path}: {last_error}")


def raw_fetch(commit, path, attempts=12):
    url = f"{RAW}/{commit}/{urllib.parse.quote(path, safe='/')}"
    last_error = None
    for attempt in range(attempts):
        try:
            request = urllib.request.Request(url, headers={"User-Agent": "eden-libwebrtc-fetch"})
            with urllib.request.urlopen(request, timeout=120) as response:
                return response.read()
        except urllib.error.HTTPError as error:
            last_error = error
            if error.code == 429:
                delay = float(error.headers.get("Retry-After") or 20) + attempt * 5
                time.sleep(delay)
                continue
            time.sleep(2 * (attempt + 1))
        except Exception as error:
            last_error = error
            time.sleep(2 * (attempt + 1))
    raise RuntimeError(f"Raw file fetch failed: {path}: {last_error}")


def blob_sha(data):
    return hashlib.sha1(b"blob " + str(len(data)).encode() + b"\0" + data).hexdigest()


def commit_for_tag(tag):
    reference = request_json(f"/git/ref/tags/{urllib.parse.quote(tag)}")
    target = reference["object"]
    if target["type"] != "commit":
        target = request_json(f"/git/tags/{target['sha']}")["object"]
    return target["sha"]


def subtree_entries(commit):
    root = request_json(f"/git/trees/{commit}")["tree"]
    source = next(item for item in root if item["path"] == "Source")
    third_party = next(item for item in request_json(f"/git/trees/{source['sha']}")["tree"] if item["path"] == "ThirdParty")
    libwebrtc = next(item for item in request_json(f"/git/trees/{third_party['sha']}")["tree"] if item["path"] == "libwebrtc")
    listing = request_json(f"/git/trees/{libwebrtc['sha']}?recursive=1")
    if listing.get("truncated"):
        raise RuntimeError("GitHub truncated the libwebrtc tree listing")
    entries = listing["tree"]
    submodules = [entry for entry in entries if entry["type"] == "commit"]
    if submodules:
        raise RuntimeError("The libwebrtc subtree contains submodules: " + ", ".join(entry["path"] for entry in submodules))
    return entries


def download_subtree(commit, entries, destination):
    def download(entry):
        relative = entry["path"]
        target = destination / SUBTREE / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        if entry["type"] == "tree":
            return
        if entry["mode"] == "120000":
            if target.is_symlink() and target.readlink().as_posix() == raw_fetch(commit, f"{SUBTREE}/{relative}").decode():
                return
            if target.exists() or target.is_symlink():
                target.unlink()
            target.symlink_to(raw_fetch(commit, f"{SUBTREE}/{relative}").decode())
            return
        if target.is_file() and blob_sha(target.read_bytes()) == entry["sha"]:
            if entry["mode"] == "100755":
                target.chmod(0o755)
            return
        data = raw_fetch(commit, f"{SUBTREE}/{relative}")
        if blob_sha(data) != entry["sha"]:
            raise RuntimeError(f"Blob hash mismatch for {relative}")
        target.write_bytes(data)
        if entry["mode"] == "100755":
            target.chmod(0o755)

    with concurrent.futures.ThreadPoolExecutor(max_workers=WORKERS) as pool:
        for _ in pool.map(download, entries):
            pass


def pack_archive(version, staging, output):
    prefix = Path(f"libwebrtc-wpewebkit-{version}")
    with lzma.LZMAFile(output, "w", preset=6) as compressed, tarfile.open(fileobj=compressed, mode="w", format=tarfile.PAX_FORMAT) as package:
        for path in sorted((staging / SUBTREE).rglob("*")):
            relative = prefix / path.relative_to(staging)
            if path.is_dir():
                info = tarfile.TarInfo(str(relative))
                info.type = tarfile.DIRTYPE
                info.mode = 0o755
                package.addfile(info)
                continue
            info = tarfile.TarInfo(str(relative))
            info.mtime = 0
            info.uid = 0
            info.gid = 0
            info.uname = ""
            info.gname = ""
            if path.is_symlink():
                info.type = tarfile.SYMTYPE
                info.linkname = path.readlink().as_posix()
                package.addfile(info)
                continue
            info.type = tarfile.REGTYPE
            info.mode = 0o755 if path.stat().st_mode & 0o111 else 0o644
            info.size = path.stat().st_size
            with path.open("rb") as stream:
                package.addfile(info, stream)


def main():
    parser = argparse.ArgumentParser(
        description="Fetch WebKit's vendored libwebrtc subtree and pack the pinned archive for the WPE release"
    )
    parser.add_argument("--output", type=Path, help="archive path, default: third_party/wpe/packages")
    parser.add_argument(
        "--staging",
        type=Path,
        help="persistent staging directory, resumed across interrupted runs instead of a temporary one",
    )
    arguments = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    version, pinned_commit = pinned_release(root)
    commit = commit_for_tag(f"wpewebkit-{version}")
    if pinned_commit and pinned_commit != commit:
        raise RuntimeError(f"The tag wpewebkit-{version} now points at {commit}, not the pinned {pinned_commit}")
    print(f"Tag wpewebkit-{version} resolves to commit {commit}", flush=True)
    entries = subtree_entries(commit)
    print(f"Tree lists {len(entries)} entries", flush=True)
    output = arguments.output or root / "third_party" / "wpe" / "packages" / f"libwebrtc-wpewebkit-{version}.tar.xz"
    with contextlib.ExitStack() as stack:
        if arguments.staging:
            staging = arguments.staging
            staging.mkdir(parents=True, exist_ok=True)
        else:
            staging = Path(stack.enter_context(tempfile.TemporaryDirectory(prefix="eden-libwebrtc-fetch-")))
        download_subtree(commit, entries, staging)
        output.parent.mkdir(parents=True, exist_ok=True)
        pending = output.with_suffix(output.suffix + ".staging")
        pack_archive(version, staging, pending)
        pending.replace(output)
    with output.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    print(f"Packed {output}")
    print(f"SHA-256 {digest}")
    if not pinned_commit:
        print(f"Pin this fetch with EDEN_LIBWEBRTC_COMMIT \"{commit}\" and EDEN_LIBWEBRTC_SHA256 \"{digest}\"")


if __name__ == "__main__":
    main()
