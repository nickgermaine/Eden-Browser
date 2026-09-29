import argparse
import contextlib
import datetime
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
WPT_REVISION = "f085a1efc1f58fbe263d384b1e335d656fe58e66"
WPT_ARCHIVE_SHA256 = "ae13de3517d1bb65fd5b2d985e4544ddd1576fb6212a853a6bb030de8df969f1"
SAMPLES_REVISION = "6e2c5a117aa860d66d874f7c0df0ef9a3e2cd74e"
SAMPLES_ARCHIVE_SHA256 = "20e262d67a73f4204dcc4db57bdf2952d4270a41983e352d171373388b628503"
ADAPTER_VERSION = "9.0.6"
ADAPTER_ARCHIVE_SHA256 = "361e50645a020ab2f598b5ea740ab81e49db32a22e1c588ecfde36ce874b428e"
ADAPTER_SHA256 = "4c229747115c1545d5c35ab4dd1dce13de30817fc362697f44dc6abe3efa8d17"
WPT_DIRECTORIES = (
    "webrtc",
    "webrtc-encoded-transform",
    "webrtc-extensions",
    "webrtc-stats",
    "webrtc-svc",
    "mediacapture-streams",
    "mediacapture-record",
    "mediacapture-fromelement",
    "mediacapture-insertable-streams",
    "screen-capture",
    "webcodecs",
)
BROWSERS = ("eden", "chromium")
SUITES = ("wpt", "samples")
SERVER_PORTS = {
    "http": 2,
    "http-local": 1,
    "http-public": 1,
    "https": 2,
    "https-local": 1,
    "https-public": 1,
    "ws": 1,
    "wss": 1,
    "h2": 1,
    "webtransport-h3": 1,
    "dns": 1,
}
PASSING = {"PASS", "OK"}
WPTRUNNER_LAUNCHER = (
    "import sys\n"
    "root = sys.argv.pop(1)\n"
    "sys.path.insert(0, root)\n"
    "sys.path.insert(0, root + '/tools')\n"
    "import localpaths\n"
    "sys.path.insert(0, root + '/tools/wptrunner')\n"
    "from wptrunner import wptrunner\n"
    "sys.exit(wptrunner.main())\n"
)
HINTS = (
    ("harness", re.compile(r"set_permission|set permission|test_driver|testdriver|WebDriver|unimplemented|Action \w+ not implemented|Unsupported action|unknown command", re.I)),
    ("missing feature", re.compile(r"is not a function|is not defined|is not a constructor|Can't find variable|undefined is not an object|not supported|should be supported|NotSupportedError|not implemented|Unrecognized|is undefined|got undefined", re.I)),
)


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as source:
        for block in iter(lambda: source.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def ensure_source(directory, archive, url, expected_sha256):
    if directory.is_dir():
        return directory
    directory.parent.mkdir(parents=True, exist_ok=True)
    if not archive.is_file():
        partial = archive.with_name(archive.name + ".partial")
        with urllib.request.urlopen(url, timeout=120) as response, open(partial, "wb") as target:
            shutil.copyfileobj(response, target)
        partial.replace(archive)
    actual = sha256_of(archive)
    if actual != expected_sha256:
        raise RuntimeError(f"{archive} has SHA-256 {actual}, expected {expected_sha256}")
    with tempfile.TemporaryDirectory(dir=directory.parent) as staging:
        with tarfile.open(archive) as bundle:
            bundle.extractall(staging, filter="data")
        extracted = Path(staging) / directory.name
        if not extracted.is_dir():
            raise RuntimeError(f"{archive} does not contain {directory.name}")
        extracted.replace(directory)
    return directory


def ensure_sources(wpt, samples):
    ensure_source(
        wpt,
        wpt.with_name(wpt.name + ".tar.gz"),
        f"https://codeload.github.com/web-platform-tests/wpt/tar.gz/{WPT_REVISION}",
        WPT_ARCHIVE_SHA256,
    )
    ensure_source(
        samples,
        samples.with_name(samples.name + ".tar.gz"),
        f"https://codeload.github.com/webrtc/samples/tar.gz/{SAMPLES_REVISION}",
        SAMPLES_ARCHIVE_SHA256,
    )
    script, archive = adapter_paths(samples)
    ensure_adapter(script, archive, f"https://registry.npmjs.org/webrtc-adapter/-/webrtc-adapter-{ADAPTER_VERSION}.tgz")


def ensure_adapter(script, archive, url):
    if script.is_file() and sha256_of(script) == ADAPTER_SHA256:
        return script
    script.parent.mkdir(parents=True, exist_ok=True)
    if not archive.is_file():
        partial = archive.with_name(archive.name + ".partial")
        with urllib.request.urlopen(url, timeout=120) as response, open(partial, "wb") as target:
            shutil.copyfileobj(response, target)
        partial.replace(archive)
    actual = sha256_of(archive)
    if actual != ADAPTER_ARCHIVE_SHA256:
        raise RuntimeError(f"{archive} has SHA-256 {actual}, expected {ADAPTER_ARCHIVE_SHA256}")
    with tarfile.open(archive) as bundle:
        try:
            member = bundle.extractfile("package/out/adapter.js")
        except KeyError as error:
            raise RuntimeError(f"{archive} has no package/out/adapter.js") from error
        data = member.read()
    if hashlib.sha256(data).hexdigest() != ADAPTER_SHA256:
        raise RuntimeError(f"adapter.js in {archive} does not match the pinned SHA-256")
    partial = script.with_name(script.name + ".partial")
    partial.write_bytes(data)
    partial.replace(script)
    return script


def adapter_paths(samples):
    root = samples.parent
    return root / f"adapter-{ADAPTER_VERSION}.js", root / f"webrtc-adapter-{ADAPTER_VERSION}.tgz"


def requirement_files(wpt):
    return [
        wpt / "tools/manifest/requirements.txt",
        wpt / "tools/wptrunner/requirements.txt",
        wpt / "tools/wptrunner/requirements_chromium.txt",
    ]


def ensure_venv(wpt, venv, interpreter):
    requirements = requirement_files(wpt)
    digest = hashlib.sha256(b"".join(path.read_bytes() for path in requirements)).hexdigest()
    marker = venv / "eden-requirements.sha256"
    python = venv / "bin/python"
    if python.is_file() and marker.is_file() and marker.read_text().strip() == digest:
        return python
    if venv.exists():
        shutil.rmtree(venv)
    installer = shutil.which("uv")
    if installer:
        subprocess.run([installer, "venv", "--python", interpreter, str(venv)], check=True)
        subprocess.run(
            [installer, "pip", "install", "--python", str(python), *(item for path in requirements for item in ("-r", str(path)))],
            check=True,
        )
    else:
        subprocess.run([interpreter, "-m", "venv", str(venv)], check=True)
        subprocess.run(
            [str(python), "-m", "pip", "install", *(item for path in requirements for item in ("-r", str(path)))],
            check=True,
        )
    marker.write_text(digest + "\n")
    return python


def ensure_server_config(wpt):
    config = wpt / "config.json"
    wanted = json.dumps({"ports": {name: ["auto"] * count for name, count in SERVER_PORTS.items()}}, indent=2) + "\n"
    if not config.is_file() or config.read_text() != wanted:
        config.write_text(wanted)


def wpt_hosts(wpt, python, venv):
    completed = subprocess.run(
        [str(python), str(wpt / "wpt"), "--venv", str(venv), "--skip-venv-setup", "make-hosts-file"],
        check=True,
        capture_output=True,
        text=True,
        cwd=wpt,
    )
    entries = [line for line in completed.stdout.splitlines() if line.startswith("127.0.0.1\t")]
    if not any(line.endswith("\tweb-platform.test") for line in entries):
        raise RuntimeError("wpt make-hosts-file did not list web-platform.test")
    return entries


def sandbox_hosts(system_hosts, entries):
    return system_hosts.rstrip("\n") + "\n" + "\n".join(entries) + "\n"


def sandbox_nsswitch(system_nsswitch):
    lines = []
    for line in system_nsswitch.splitlines():
        match = re.match(r"^(\s*hosts:\s*)(.*)$", line)
        if not match:
            lines.append(line)
            continue
        tokens = match.group(2).split()
        kept = []
        skip_action = False
        for token in tokens:
            if skip_action and token.startswith("["):
                skip_action = False
                continue
            skip_action = False
            if token == "files":
                skip_action = True
                continue
            kept.append(token)
        lines.append("hosts: " + " ".join(["files", *kept]))
    if not any(re.match(r"^\s*hosts:", line) for line in lines):
        lines.append("hosts: files dns")
    return "\n".join(lines) + "\n"


def load_test_wpe():
    specification = importlib.util.spec_from_file_location("eden_test_wpe", ROOT / "scripts/test_wpe.py")
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


def webkit_prefix(build):
    cache = build / "CMakeCache.txt"
    if cache.is_file():
        match = re.search(r"^EDEN_WPE_PREFIX:[A-Z]+=(.+)$", cache.read_text(), re.M)
        if match and match.group(1).strip():
            return Path(match.group(1).strip())
    raise RuntimeError(f"{build} does not record EDEN_WPE_PREFIX")


def browser_version(command):
    try:
        completed = subprocess.run(command, capture_output=True, text=True, timeout=60, check=False)
    except OSError:
        return None
    output = (completed.stdout or completed.stderr).strip()
    return output.splitlines()[0] if output else None


def wptrunner_command(arguments, browser, update_manifest):
    output = arguments.output / "wpt" / browser
    command = [
        str(arguments.python),
        "-c",
        WPTRUNNER_LAUNCHER,
        str(arguments.wpt),
        "--tests",
        str(arguments.wpt),
        "--metadata",
        str(arguments.metadata),
        "--manifest-update" if update_manifest else "--no-manifest-update",
        "--no-manifest-download",
        "--ssl-type=pregenerated",
        "--ca-cert-path",
        str(arguments.wpt / "tools/certs/cacert.pem"),
        "--host-key-path",
        str(arguments.wpt / "tools/certs/web-platform.test.key"),
        "--host-cert-path",
        str(arguments.wpt / "tools/certs/web-platform.test.pem"),
        "--test-type=testharness",
        "--processes=1",
        "--no-pause-after-test",
        "--no-fail-on-unexpected",
        "--no-enable-webtransport-h3",
        f"--timeout-multiplier={arguments.timeout_multiplier}",
        "--log-wptreport",
        str(output / "wptreport.json"),
        "--log-raw",
        str(output / "raw.log"),
        "--log-mach",
        str(output / "mach.log"),
    ]
    if browser == "eden":
        command += [
            "--product=wpewebkit_minibrowser",
            "--no-headless",
            "--binary-arg=--browser-name=MiniBrowser",
            "--binary",
            str(arguments.build / "eden-wpt-host"),
            "--webdriver-binary",
            str(arguments.webdriver),
        ]
        if arguments.eden_version:
            command += ["--browser-version", arguments.eden_version]
    else:
        command += [
            "--product=chromium",
            "--headless",
            "--no-enable-experimental",
            "--binary",
            str(arguments.chromium),
            "--webdriver-binary",
            str(arguments.chromedriver),
        ]
        if arguments.chromium_version:
            command += ["--browser-version", arguments.chromium_version]
    command += [f"--include=/{path.strip('/')}/" if "." not in Path(path).name else f"--include=/{path.strip('/')}" for path in arguments.include]
    return command


def samples_command(arguments, browser):
    output = arguments.output / "samples" / browser
    command = [
        arguments.node,
        str(ROOT / "tests/wpt/samples/run.mjs"),
        "--browser",
        browser,
        "--samples",
        str(arguments.samples),
        "--tests",
        str(ROOT / "tests/wpt/samples"),
        "--output",
        str(output / "results.json"),
        "--adapter",
        str(adapter_paths(arguments.samples)[0]),
        "--test-timeout",
        str(int(30000 * arguments.timeout_multiplier)),
    ]
    if browser == "eden":
        command += ["--webdriver", str(arguments.webdriver), "--binary", str(arguments.build / "eden-wpt-host")]
    else:
        command += ["--webdriver", str(arguments.chromedriver), "--binary", str(arguments.chromium)]
    for pattern in arguments.sample:
        command += ["--filter", pattern]
    return command


def run_inside(arguments):
    test_wpe = load_test_wpe()
    environment = os.environ.copy()
    failures = []
    with test_wpe.private_compositor(environment):
        update_manifest = not arguments.skip_manifest_update
        for suite in arguments.suite:
            for browser in arguments.browser:
                output = arguments.output / suite / browser
                output.mkdir(parents=True, exist_ok=True)
                if suite == "wpt":
                    command = wptrunner_command(arguments, browser, update_manifest)
                    update_manifest = False
                else:
                    command = samples_command(arguments, browser)
                (output / "command.json").write_text(json.dumps(command, indent=2) + "\n")
                with open(output / "console.log", "wb") as console:
                    completed = subprocess.run(command, env=environment, stdout=console, stderr=subprocess.STDOUT, cwd=ROOT, check=False)
                if completed.returncode:
                    failures.append(f"{suite} in {browser} exited with {completed.returncode}")
    for failure in failures:
        print(failure, file=sys.stderr)
    return 1 if failures else 0


def sandbox_environment(runtime):
    environment = os.environ.copy()
    for name in ("LD_LIBRARY_PATH", "PKG_CONFIG_PATH", "WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS", "WAYLAND_DISPLAY", "DISPLAY"):
        environment.pop(name, None)
    environment["XDG_RUNTIME_DIR"] = str(runtime)
    environment["XDG_DATA_HOME"] = str(runtime / "data")
    environment["XDG_CACHE_HOME"] = str(runtime / "cache")
    environment["XDG_CONFIG_HOME"] = str(runtime / "config")
    environment["QT_SCALE_FACTOR"] = "1"
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    return environment


def sandbox_command(arguments, hosts, nsswitch, forwarded):
    command = [
        "bwrap",
        "--die-with-parent",
        "--ro-bind",
        "/",
        "/",
        "--dev-bind",
        "/dev",
        "/dev",
        *(["--tmpfs", "/dev/snd"] if Path("/dev/snd").is_dir() else []),
        "--proc",
        "/proc",
        "--bind",
        "/tmp",
        "/tmp",
        "--ro-bind",
        str(hosts),
        "/etc/hosts",
        "--ro-bind",
        str(nsswitch),
        "/etc/nsswitch.conf",
        "--bind",
        str(arguments.output),
        str(arguments.output),
        "--bind",
        str(arguments.metadata),
        str(arguments.metadata),
    ]
    nss = Path.home() / ".pki"
    if nss.is_dir():
        command += ["--tmpfs", str(nss)]
    return [*command, "--", sys.executable, str(Path(__file__).resolve()), "--inside", *forwarded]


def read_results(path):
    text = Path(path).read_text()
    if not text.strip():
        raise RuntimeError(f"{path} is empty, so that run produced no results")
    try:
        return json.loads(text)
    except json.JSONDecodeError as error:
        raise RuntimeError(f"{path} is not valid JSON: {error}") from error


def load_wptreport(path):
    report = read_results(path)
    tests = {}
    for result in report.get("results", []):
        tests[result["test"]] = {
            "status": result.get("status"),
            "message": result.get("message"),
            "subtests": {
                subtest["name"]: {"status": subtest.get("status"), "message": subtest.get("message")}
                for subtest in result.get("subtests", [])
            },
        }
    return {"tests": tests, "run_info": report.get("run_info", {})}


def load_samples(path):
    report = read_results(path)
    tests = {}
    for entry in report.get("files", []):
        test = tests.setdefault(entry["file"], {"status": "OK", "message": None, "subtests": {}})
        if entry.get("status") != "OK":
            test["status"] = entry["status"]
            test["message"] = entry.get("message")
    for result in report.get("results", []):
        test = tests.setdefault(result["file"], {"status": "OK", "message": None, "subtests": {}})
        test["subtests"][result["name"]] = {"status": result["status"], "message": result.get("message")}
    return {"tests": tests, "run_info": report.get("run_info", {})}


def hint(message, status):
    if status in {"TIMEOUT", "EXTERNAL-TIMEOUT"}:
        return "timeout"
    if status == "CRASH":
        return "crash"
    for name, pattern in HINTS:
        if message and pattern.search(message):
            return name
    return "behavior"


def compare(eden, chromium):
    differences = []
    summary = {}
    for test in sorted(set(eden["tests"]) | set(chromium["tests"])):
        area = test.strip("/").split("/", 1)[0]
        bucket = summary.setdefault(
            area,
            {"tests": 0, "subtests": 0, "eden_passes": 0, "chromium_passes": 0, "eden_only_failures": 0, "chromium_only_failures": 0, "missing": 0},
        )
        bucket["tests"] += 1
        ours = eden["tests"].get(test)
        theirs = chromium["tests"].get(test)
        if ours is None or theirs is None:
            bucket["missing"] += 1
            differences.append({
                "test": test,
                "subtest": None,
                "kind": "missing in eden" if ours is None else "missing in chromium",
                "eden": None if ours is None else {"status": ours["status"], "message": ours["message"]},
                "chromium": None if theirs is None else {"status": theirs["status"], "message": theirs["message"]},
                "hint": "harness",
            })
            continue
        if theirs["status"] in PASSING and ours["status"] not in PASSING:
            differences.append({
                "test": test,
                "subtest": None,
                "kind": "eden fails",
                "eden": {"status": ours["status"], "message": ours["message"]},
                "chromium": {"status": theirs["status"], "message": theirs["message"]},
                "hint": hint(ours["message"], ours["status"]),
            })
        elif ours["status"] in PASSING and theirs["status"] not in PASSING:
            differences.append({
                "test": test,
                "subtest": None,
                "kind": "chromium fails",
                "eden": {"status": ours["status"], "message": ours["message"]},
                "chromium": {"status": theirs["status"], "message": theirs["message"]},
                "hint": hint(theirs["message"], theirs["status"]),
            })
        for name in sorted(set(ours["subtests"]) | set(theirs["subtests"])):
            bucket["subtests"] += 1
            mine = ours["subtests"].get(name, {"status": "MISSING", "message": None})
            other = theirs["subtests"].get(name, {"status": "MISSING", "message": None})
            eden_passes = mine["status"] in PASSING
            chromium_passes = other["status"] in PASSING
            bucket["eden_passes"] += eden_passes
            bucket["chromium_passes"] += chromium_passes
            if chromium_passes and not eden_passes:
                bucket["eden_only_failures"] += 1
                kind = "eden fails"
                reason = hint(mine["message"], mine["status"])
            elif eden_passes and not chromium_passes:
                bucket["chromium_only_failures"] += 1
                kind = "chromium fails"
                reason = hint(other["message"], other["status"])
            else:
                continue
            differences.append({"test": test, "subtest": name, "kind": kind, "eden": mine, "chromium": other, "hint": reason})
    return {"summary": summary, "differences": differences}


def shorten(message, limit=220):
    if not message:
        return ""
    text = " ".join(str(message).split())
    return text if len(text) <= limit else text[: limit - 3] + "..."


def load_diff(directory):
    path = Path(directory) / "diff.json"
    if not path.is_file():
        raise RuntimeError(f"{directory} has no diff.json, so it is not a finished sweep")
    return json.loads(path.read_text())


def differing_tests(diff):
    tests = {}
    for suite, comparison in diff.items():
        names = set()
        for item in comparison["differences"]:
            names.add(item["test"].split("?", 1)[0] if suite == "wpt" else item["test"])
        tests[suite] = sorted(names)
    return tests


def difference_key(suite, item):
    return (suite, item["test"], item["subtest"], item["kind"])


def mark_reproduced(comparisons, confirmation):
    confirmed = {difference_key(suite, item) for suite, comparison in confirmation.items() for item in comparison["differences"]}
    for suite, comparison in comparisons.items():
        if suite not in confirmation:
            continue
        for item in comparison["differences"]:
            item["reproduced"] = difference_key(suite, item) in confirmed
    return comparisons


def render_report(comparisons, meta):
    lines = ["# Eden WebRTC conformance sweep", ""]
    lines.append(f"- Eden: {meta.get('eden_version') or 'unknown'} ({meta.get('eden_runtime')})")
    lines.append(f"- Chromium: {meta.get('chromium_version') or 'unknown'}")
    lines.append(f"- web-platform-tests {WPT_REVISION}, webrtc/samples {SAMPLES_REVISION}")
    lines.append(f"- Generated {meta.get('generated')}")
    lines.append("")
    for suite, comparison in comparisons.items():
        lines.append(f"## {suite}")
        lines.append("")
        lines.append("| Area | Tests | Subtests | Eden passes | Chromium passes | Eden fails, Chromium passes | Chromium fails, Eden passes | Missing |")
        lines.append("|---|---:|---:|---:|---:|---:|---:|---:|")
        for area, bucket in sorted(comparison["summary"].items()):
            lines.append(
                f"| {area} | {bucket['tests']} | {bucket['subtests']} | {bucket['eden_passes']} | {bucket['chromium_passes']} "
                f"| {bucket['eden_only_failures']} | {bucket['chromium_only_failures']} | {bucket['missing']} |"
            )
        lines.append("")
        for kind in ("eden fails", "missing in eden", "missing in chromium", "chromium fails"):
            items = [item for item in comparison["differences"] if item["kind"] == kind]
            if not items:
                continue
            lines.append(f"### {kind.capitalize()} ({len(items)})")
            lines.append("")
            if any("reproduced" in item for item in items):
                reproduced = sum(1 for item in items if item.get("reproduced"))
                lines.append(f"{reproduced} reproduced in the confirmation run, {len(items) - reproduced} did not and are marked flaky.")
                lines.append("")
                items = sorted(items, key=lambda item: not item.get("reproduced"))
            for item in items:
                name = item["test"] + (f" | {item['subtest']}" if item["subtest"] else "")
                side = item["eden"] if kind in {"eden fails", "missing in chromium"} else item["chromium"]
                status = side["status"] if side else "MISSING"
                message = shorten(side["message"]) if side else ""
                flaky = " (flaky)" if item.get("reproduced") is False else ""
                lines.append(f"- [{item['hint']}]{flaky} `{name}`: {status}{(' ' + message) if message else ''}")
            lines.append("")
    return "\n".join(lines) + "\n"


def report(arguments):
    comparisons = {}
    for suite, loader, name in (("wpt", load_wptreport, "wptreport.json"), ("samples", load_samples, "results.json")):
        eden = arguments.output / suite / "eden" / name
        chromium = arguments.output / suite / "chromium" / name
        if eden.is_file() and chromium.is_file():
            comparisons[suite] = compare(loader(eden), loader(chromium))
    if not comparisons:
        raise RuntimeError(f"{arguments.output} has no results from both browsers")
    if arguments.confirmation:
        mark_reproduced(comparisons, load_diff(arguments.confirmation))
    meta_path = arguments.output / "meta.json"
    meta = json.loads(meta_path.read_text()) if meta_path.is_file() else {}
    meta["generated"] = datetime.datetime.now().astimezone().isoformat(timespec="seconds")
    if arguments.confirmation:
        meta["confirmation"] = str(arguments.confirmation)
    (arguments.output / "diff.json").write_text(json.dumps(comparisons, indent=2) + "\n")
    (arguments.output / "report.md").write_text(render_report(comparisons, meta))
    return comparisons


def record_run(path, versions, run):
    meta = json.loads(path.read_text()) if path.is_file() else {}
    runs = [entry for entry in meta.get("runs", []) if entry.get("started") != run["started"]]
    meta.update(versions)
    meta["runs"] = [*runs, run]
    path.write_text(json.dumps(meta, indent=2) + "\n")
    return meta


def parse_arguments(argv=None):
    parser = argparse.ArgumentParser(description="Run web-platform-tests and the WebRTC samples in Eden and Chromium and compare them")
    parser.add_argument("--build", type=Path, default=ROOT / "cmake-build-release")
    parser.add_argument("--wpt", type=Path, default=ROOT / "third_party/wpt" / f"wpt-{WPT_REVISION}")
    parser.add_argument("--samples", type=Path, default=ROOT / "third_party/webrtc-samples" / f"samples-{SAMPLES_REVISION}")
    parser.add_argument("--venv", type=Path, default=ROOT / "third_party/wpt/venv")
    parser.add_argument("--metadata", type=Path, default=ROOT / "third_party/wpt/metadata")
    parser.add_argument("--python-for-venv", default=sys.executable)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--browser", action="append", choices=BROWSERS)
    parser.add_argument("--suite", action="append", choices=SUITES)
    parser.add_argument("--include", action="append", help="WPT directory or test path, default is every WebRTC and media capture directory")
    parser.add_argument("--sample", action="append", default=[], help="Only run samples whose path contains this text")
    parser.add_argument("--chromium", type=Path, default=Path(shutil.which("chromium") or "/usr/bin/chromium"))
    parser.add_argument("--chromedriver", type=Path, default=Path(shutil.which("chromedriver") or "/usr/bin/chromedriver"))
    parser.add_argument("--node", default=shutil.which("node") or "node")
    parser.add_argument("--timeout-multiplier", type=float, default=1.0)
    parser.add_argument("--skip-manifest-update", action="store_true")
    parser.add_argument("--report-only", action="store_true")
    parser.add_argument("--differences-from", type=Path, help="Rerun only the tests that differed in this finished sweep")
    parser.add_argument("--confirmation", type=Path, help="Mark each difference by whether this rerun reproduced it")
    parser.add_argument("--inside", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--python", type=Path, help=argparse.SUPPRESS)
    parser.add_argument("--webdriver", type=Path, help=argparse.SUPPRESS)
    parser.add_argument("--eden-version", help=argparse.SUPPRESS)
    parser.add_argument("--chromium-version", help=argparse.SUPPRESS)
    arguments = parser.parse_args(argv)
    arguments.browser = arguments.browser or list(BROWSERS)
    arguments.suite = arguments.suite or list(SUITES)
    if arguments.differences_from and not arguments.inside:
        if arguments.include or arguments.sample:
            parser.error("--differences-from chooses the tests itself, so it can't be combined with --include or --sample")
        try:
            differing = differing_tests(load_diff(arguments.differences_from))
        except (OSError, ValueError, RuntimeError) as error:
            parser.error(str(error))
        arguments.suite = [suite for suite in arguments.suite if differing.get(suite)]
        if not arguments.suite:
            parser.error(f"{arguments.differences_from} has no differences to rerun")
        arguments.include = differing.get("wpt") or None
        arguments.sample = differing.get("samples", []) if "samples" in arguments.suite else []
    arguments.include = arguments.include or list(WPT_DIRECTORIES)
    for name in ("build", "wpt", "samples", "venv", "metadata", "output", "chromium", "chromedriver"):
        setattr(arguments, name, getattr(arguments, name).resolve())
    for name in ("differences_from", "confirmation"):
        if getattr(arguments, name):
            setattr(arguments, name, getattr(arguments, name).resolve())
    if not 0.5 <= arguments.timeout_multiplier <= 10:
        parser.error("The timeout multiplier must be between 0.5 and 10")
    for path in arguments.include:
        if path.startswith("/") and not path.strip("/"):
            parser.error("An include path must name a directory or test")
    return parser, arguments


def forwarded_arguments(arguments, python, webdriver, eden_version, chromium_version):
    forwarded = [
        "--build", str(arguments.build),
        "--wpt", str(arguments.wpt),
        "--samples", str(arguments.samples),
        "--venv", str(arguments.venv),
        "--metadata", str(arguments.metadata),
        "--output", str(arguments.output),
        "--chromium", str(arguments.chromium),
        "--chromedriver", str(arguments.chromedriver),
        "--node", arguments.node,
        "--timeout-multiplier", str(arguments.timeout_multiplier),
        "--python", str(python),
        "--webdriver", str(webdriver),
    ]
    if eden_version:
        forwarded += ["--eden-version", eden_version]
    if chromium_version:
        forwarded += ["--chromium-version", chromium_version]
    if arguments.skip_manifest_update:
        forwarded.append("--skip-manifest-update")
    for browser in arguments.browser:
        forwarded += ["--browser", browser]
    for suite in arguments.suite:
        forwarded += ["--suite", suite]
    for path in arguments.include:
        forwarded += ["--include", path]
    for pattern in arguments.sample:
        forwarded += ["--sample", pattern]
    return forwarded


def main(argv=None):
    parser, arguments = parse_arguments(argv)
    if arguments.inside:
        return run_inside(arguments)
    if arguments.report_only:
        report(arguments)
        print(arguments.output / "report.md")
        return 0
    if not shutil.which("bwrap"):
        parser.error("Install bubblewrap to isolate the sweep")
    if "eden" in arguments.browser and not (arguments.build / "eden-wpt-host").is_file():
        parser.error(f"Build the eden-wpt-host target in {arguments.build} first")
    if "chromium" in arguments.browser and not (arguments.chromium.is_file() and arguments.chromedriver.is_file()):
        parser.error("Chromium and a matching chromedriver are required")
    if "samples" in arguments.suite and not shutil.which(arguments.node):
        parser.error("Node.js is required for the samples")
    ensure_sources(arguments.wpt, arguments.samples)
    python = ensure_venv(arguments.wpt, arguments.venv, arguments.python_for_venv)
    ensure_server_config(arguments.wpt)
    prefix = webkit_prefix(arguments.build)
    webdriver = prefix / "bin/WPEWebDriver"
    if "eden" in arguments.browser and not webdriver.is_file():
        parser.error(f"{prefix} has no WPEWebDriver")
    arguments.output.mkdir(parents=True, exist_ok=True)
    arguments.metadata.mkdir(parents=True, exist_ok=True)
    eden_version = prefix.name
    chromium_version = browser_version([str(arguments.chromium), "--version"]) if "chromium" in arguments.browser else None
    run = {
        "browsers": arguments.browser,
        "suites": arguments.suite,
        "include": arguments.include,
        "samples": arguments.sample,
        "timeout_multiplier": arguments.timeout_multiplier,
        "differences_from": str(arguments.differences_from) if arguments.differences_from else None,
        "started": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
    }
    meta = record_run(arguments.output / "meta.json", {
        "eden_runtime": str(prefix),
        "eden_version": eden_version,
        "eden_build": str(arguments.build),
        "chromium_version": chromium_version,
        "chromedriver_version": browser_version([str(arguments.chromedriver), "--version"]) if "chromium" in arguments.browser else None,
        "wpt_revision": WPT_REVISION,
        "samples_revision": SAMPLES_REVISION,
        "adapter_version": ADAPTER_VERSION,
    }, run)
    test_wpe = load_test_wpe()
    with tempfile.TemporaryDirectory(prefix="eden-wpt-sweep-") as temporary:
        runtime = Path(temporary)
        hosts = runtime / "hosts"
        nsswitch = runtime / "nsswitch.conf"
        hosts.write_text(sandbox_hosts(Path("/etc/hosts").read_text(), wpt_hosts(arguments.wpt, python, arguments.venv)))
        nsswitch.write_text(sandbox_nsswitch(Path("/etc/nsswitch.conf").read_text()))
        (runtime / "run").mkdir()
        environment = sandbox_environment(runtime / "run")
        test_wpe.seed_gstreamer_registry(environment)
        test_wpe.silence_audio_output(environment)
        command = sandbox_command(arguments, hosts, nsswitch, forwarded_arguments(arguments, python, webdriver, eden_version, chromium_version))
        result = subprocess.run(command, env=environment, check=False).returncode
    run["finished"] = datetime.datetime.now().astimezone().isoformat(timespec="seconds")
    run["exit_code"] = result
    record_run(arguments.output / "meta.json", meta, run)
    if all(browser in arguments.browser for browser in BROWSERS):
        with contextlib.suppress(RuntimeError):
            report(arguments)
            print(arguments.output / "report.md")
            if arguments.differences_from:
                report(argparse.Namespace(output=arguments.differences_from, confirmation=arguments.output))
                print(arguments.differences_from / "report.md")
    return result


if __name__ == "__main__":
    sys.exit(main())
