import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
from urllib.parse import urlsplit, urlunsplit


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

from wpe.bolt import write_json


def identity(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def read_page_set(path):
    page_set = json.loads(path.read_text())
    if not isinstance(page_set, list) or len(page_set) < 5:
        raise RuntimeError("The browsing page set must contain at least five pages")
    seen = set()
    for index, page in enumerate(page_set):
        if not isinstance(page, dict):
            raise RuntimeError(f"Browsing page {index + 1} must be an object")
        url = page.get("url")
        selector = page.get("selector")
        expected_text = page.get("expectedText")
        if (not isinstance(url, str) or not url or not isinstance(selector, str) or not selector
                or not isinstance(expected_text, str) or not expected_text):
            raise RuntimeError(f"Browsing page {index + 1} requires a URL, selector and expected text")
        try:
            parts = urlsplit(url)
            port = parts.port
        except ValueError as error:
            raise RuntimeError(f"Browsing page {index + 1} has an invalid URL") from error
        if (parts.scheme.lower() not in ("http", "https") or not parts.hostname
                or parts.username is not None or parts.password is not None
                or parts.netloc.endswith(":")):
            raise RuntimeError(f"Browsing page {index + 1} must use an HTTP(S) URL without user information")
        if port is not None and not 1 <= port <= 65535:
            raise RuntimeError(f"Browsing page {index + 1} has an invalid URL port")
        if not selector.strip() or not expected_text.strip():
            raise RuntimeError(f"Browsing page {index + 1} requires a nonempty selector and expected text")
        host = parts.hostname.lower()
        if ":" in host and not host.startswith("["):
            host = f"[{host}]"
        default_port = 80 if parts.scheme.lower() == "http" else 443
        authority = host if port in (None, default_port) else f"{host}:{port}"
        normalized = urlunsplit((parts.scheme.lower(), authority, parts.path or "/", parts.query, ""))
        if normalized in seen:
            raise RuntimeError("The browsing page set must contain distinct URLs")
        seen.add(normalized)
    return page_set


def create_plan(arguments):
    build = arguments.build.resolve(strict=True)
    checkout = arguments.webkit_checkout.resolve(strict=True)
    webkit_build = arguments.webkit_build.resolve(strict=True)
    tests = arguments.layout_test_list.resolve(strict=True)
    pages = arguments.pages.resolve(strict=True)
    for program in ("eden-wpe-bench", "eden-wpe-tests"):
        if not (build / program).is_file():
            raise RuntimeError("Build the native driver first: " + program)
    layout_root = checkout / "LayoutTests"
    if not (checkout / "Tools/Scripts/run-webkit-tests").is_file() or not layout_root.is_dir():
        raise RuntimeError("Provide the matching complete WebKit checkout with LayoutTests")
    layout_root = layout_root.resolve(strict=True)
    selected = [line.strip() for line in tests.read_text().splitlines() if line.strip()]
    if not selected or len(selected) != len(set(selected)):
        raise RuntimeError("Provide a nonempty layout test list with no duplicates")
    for test in selected:
        path = (layout_root / test).resolve(strict=True)
        if not path.is_file() or not path.is_relative_to(layout_root):
            raise RuntimeError("Each layout smoke entry must name one test file: " + test)
    read_page_set(pages)
    common = [sys.executable, str(Path(__file__).resolve()), "--build", str(build),
              "--prefix", "{prefix}", "--report", "{report}", "--output", "{output}"]
    plan = {}
    for role in ("training", "speedometer", "smoke", "browsing", "layout"):
        argv = common + ["--run", role]
        if role == "training":
            argv += ["--profiles", "{profiles}"]
        if role == "browsing":
            argv += ["--pages", str(pages), "--pages-sha256", identity(pages)]
        if role == "layout":
            argv += ["--webkit-checkout", str(checkout), "--webkit-build", str(webkit_build),
                     "--layout-test-list", str(tests), "--tests-sha256", identity(tests)]
        specification = {"argv": argv, "timeoutSeconds": 7500 if role == "layout" else 4500}
        if role == "training":
            specification["environment"] = {"WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS": "1"}
        plan[role] = specification
    arguments.plan.parent.mkdir(parents=True, exist_ok=True)
    if arguments.plan.exists():
        raise RuntimeError("Refusing to overwrite an existing workload plan")
    write_json(arguments.plan, plan)


def run(arguments):
    role = arguments.run
    environment = os.environ.copy()
    disable_sandbox_for_training = (
        role == "training" and environment.get("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS") == "1"
    )
    for name in tuple(environment):
        managed = name.startswith(("JSC_", "WEBKIT_", "EDEN_WPE_", "LLVM_PROFILE", "SYSPROF_"))
        sandbox_exception = name == "WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS" and disable_sandbox_for_training
        if (managed and not sandbox_exception) or name in ("LD_LIBRARY_PATH", "PKG_CONFIG_PATH"):
            environment.pop(name)
    environment["EDEN_WPE_REQUIRE_PHASE1"] = "1"
    environment["EDEN_WPE_BOLT_RUNTIME"] = str(arguments.prefix)
    environment["EDEN_WPE_BOLT_REPORT"] = str(arguments.report)
    native = [sys.executable, str(ROOT / "scripts/test_wpe.py"), "--build", str(arguments.build),
              "--writable-path", str(arguments.output), "--timeout-seconds", "4200"]
    if disable_sandbox_for_training:
        native.append("--allow-webkit-sandbox-disabled")
    if role in ("training", "speedometer"):
        command = native
        if role == "training":
            command += ["--writable-path", str(arguments.profiles)]
            environment["EDEN_WPE_PREWARM"] = "0"
        command += [
            "--benchmark",
            "--",
            "--output",
            str(arguments.report),
            "--label",
            "bolt-" + role,
            "--viewport-width",
            "1600",
            "--viewport-height",
            "1200",
        ]
        if role == "training":
            command += ["--training-directory", str(arguments.profiles)]
    elif role == "smoke":
        command = native + ["canvasThreadPixelsAndContextLifecycle", "canvasReplayPresentsAndSurvivesPendingDestruction",
                            "texturePoolReusesImportsAndPresentsLatestFrame", "rendererTerminationAndReload"]
    elif role == "browsing":
        if identity(arguments.pages) != arguments.pages_sha256:
            raise RuntimeError("The browsing page set changed since this plan was written")
        environment["EDEN_WPE_BROWSING_PAGES"] = str(arguments.pages)
        environment["EDEN_WPE_BROWSING_REPORT"] = str(arguments.report)
        command = native + ["browsingSoak"]
    else:
        if identity(arguments.layout_test_list) != arguments.tests_sha256:
            raise RuntimeError("The layout smoke list changed since this plan was written")
        results = arguments.output / "layout-results"
        command = [sys.executable, str(arguments.webkit_checkout / "Tools/Scripts/run-webkit-tests"),
                   "--wpe", "--release", "--no-build", "--root", str(arguments.webkit_build),
                   "--results-directory", str(results), "--test-list", str(arguments.layout_test_list),
                   "--child-processes=1", "--no-show-results", "--no-new-test-results", "--skipped=ignore",
                   "--additional-env-var=LD_LIBRARY_PATH=" + str(arguments.prefix / "lib")]
        completed = subprocess.run(command, cwd=arguments.webkit_checkout, env=environment, timeout=7200)
        report = results / "full_results.json"
        if not report.is_file() or not 0 <= completed.returncode < 254:
            raise RuntimeError("The layout smoke driver did not produce completed results")
        shutil.copyfile(report, arguments.report)
        return
    subprocess.run(command, cwd=ROOT, env=environment, check=True, timeout=4350)


def main():
    parser = argparse.ArgumentParser(description="Write or execute the bundled BOLT workload plan")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--plan", type=Path)
    mode.add_argument("--run", choices=("training", "speedometer", "smoke", "browsing", "layout"))
    parser.add_argument("--build", type=Path, required=True)
    for name in ("webkit-checkout", "webkit-build", "layout-test-list", "pages", "prefix", "report", "output", "profiles"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--pages-sha256")
    parser.add_argument("--tests-sha256")
    arguments = parser.parse_args()
    required = ("webkit_checkout", "webkit_build", "layout_test_list", "pages") if arguments.plan else ("prefix", "report", "output")
    if any(getattr(arguments, name) is None for name in required):
        parser.error("Missing paths: " + ", ".join(required))
    create_plan(arguments) if arguments.plan else run(arguments)


if __name__ == "__main__":
    main()
