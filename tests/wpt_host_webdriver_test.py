import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests/wpt/fixtures/automation"
RUNNER = ROOT / "tests/wpt/samples/run.mjs"
EXPECTED = {
    "automation host > clicks are trusted and grant user activation",
    "automation host > display capture is granted with a fake screen",
    "automation host > camera and microphone come from fake devices",
    "automation host > typing reaches focused inputs",
    "automation host > double clicks reach elements",
    "automation host > tabs and popups are separate windows",
    "automation host > closed windows leave the handle list at once",
    "automation host > scripts open windows without a gesture",
    "automation host > alerts reach WebDriver",
    "automation host > a loopback call connects and carries data",
}


def load(name, path):
    specification = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(module)
    return module


sweep = load("eden_wpt_sweep", ROOT / "tests/wpt/wpt_sweep.py")
test_wpe = load("eden_test_wpe", ROOT / "scripts/test_wpe.py")


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def check(results_path, browser):
    report = json.loads(results_path.read_text())
    files = report["files"]
    require(len(files) == 1 and files[0]["file"] == "automation/host" and files[0]["status"] == "OK", f"{browser}: the fixture did not load: {files}")
    statuses = {result["name"]: result for result in report["results"]}
    require(set(statuses) == EXPECTED, f"{browser}: unexpected checks {sorted(set(statuses) ^ EXPECTED)}")
    failures = [f"{name}: {result['status']} {result.get('message')}" for name, result in sorted(statuses.items()) if result["status"] != "PASS"]
    require(not failures, f"{browser}: " + "; ".join(failures))
    return {name: result["duration"] for name, result in statuses.items()}


def run_inside(arguments):
    environment = os.environ.copy()
    summary = {}
    with test_wpe.private_compositor(environment):
        for browser in ("eden", "chromium"):
            output = arguments.output / f"{browser}.json"
            webdriver = arguments.webdriver if browser == "eden" else arguments.chromedriver
            binary = arguments.build / "eden-wpt-host" if browser == "eden" else arguments.chromium
            with tempfile.TemporaryDirectory(prefix="eden-wpt-no-extra-") as extra:
                command = [
                    arguments.node,
                    str(RUNNER),
                    "--browser", browser,
                    "--webdriver", str(webdriver),
                    "--binary", str(binary),
                    "--samples", str(FIXTURE),
                    "--tests", extra,
                    "--output", str(output),
                    "--test-timeout", "60000",
                ]
                completed = subprocess.run(command, env=environment, capture_output=True, text=True, timeout=600, check=False)
            require(completed.returncode == 0, f"{browser}: the runner failed: {completed.stderr[-2000:]}")
            summary[browser] = check(output, browser)
    print(json.dumps(summary, indent=2))
    return 0


def main():
    parser = argparse.ArgumentParser(description="Drive the Eden WPT host and Chromium through WebDriver with the samples runner")
    parser.add_argument("--build", type=Path, default=ROOT / "cmake-build-release")
    parser.add_argument("--chromium", type=Path, default=Path(shutil.which("chromium") or "/usr/bin/chromium"))
    parser.add_argument("--chromedriver", type=Path, default=Path(shutil.which("chromedriver") or "/usr/bin/chromedriver"))
    parser.add_argument("--node", default=shutil.which("node") or "node")
    parser.add_argument("--inside", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--output", type=Path, help=argparse.SUPPRESS)
    parser.add_argument("--webdriver", type=Path, help=argparse.SUPPRESS)
    arguments = parser.parse_args()
    arguments.build = arguments.build.resolve()
    if arguments.inside:
        try:
            return run_inside(arguments)
        except (AssertionError, subprocess.TimeoutExpired) as error:
            print(error, file=sys.stderr)
            return 1
    try:
        require(shutil.which("bwrap"), "Install bubblewrap to isolate the check")
        require((arguments.build / "eden-wpt-host").is_file(), f"Build the eden-wpt-host target in {arguments.build} first")
        require(arguments.chromium.is_file() and arguments.chromedriver.is_file(), "Chromium and a matching chromedriver are required")
        require(shutil.which(arguments.node), "Node.js is required")
        webdriver = sweep.webkit_prefix(arguments.build) / "bin/WPEWebDriver"
        require(webdriver.is_file(), f"{webdriver} is missing")
        with tempfile.TemporaryDirectory(prefix="eden-wpt-host-check-") as temporary:
            runtime = Path(temporary)
            (runtime / "run").mkdir()
            (runtime / "output").mkdir()
            environment = sweep.sandbox_environment(runtime / "run")
            test_wpe.seed_gstreamer_registry(environment)
            test_wpe.silence_audio_output(environment)
            command = [
                "bwrap",
                "--die-with-parent",
                "--ro-bind", "/", "/",
                "--dev-bind", "/dev", "/dev",
                *(["--tmpfs", "/dev/snd"] if Path("/dev/snd").is_dir() else []),
                "--proc", "/proc",
                "--bind", "/tmp", "/tmp",
                *(["--tmpfs", str(Path.home() / ".pki")] if (Path.home() / ".pki").is_dir() else []),
                "--",
                sys.executable, str(Path(__file__).resolve()), "--inside",
                "--build", str(arguments.build),
                "--chromium", str(arguments.chromium.resolve()),
                "--chromedriver", str(arguments.chromedriver.resolve()),
                "--node", arguments.node,
                "--output", str(runtime / "output"),
                "--webdriver", str(webdriver),
            ]
            completed = subprocess.run(command, env=environment, timeout=1500, check=False)
            require(completed.returncode == 0, f"The WebDriver host check exited with {completed.returncode}")
    except (AssertionError, RuntimeError, subprocess.TimeoutExpired) as error:
        raise SystemExit(f"WPT host WebDriver check failed: {error}") from error
    return 0


if __name__ == "__main__":
    sys.exit(main())
