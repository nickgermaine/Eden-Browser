import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time


ROOT = Path(__file__).resolve().parents[1]
SOCKET = "eden-portal"
SOURCE_TYPES = {"screen": 1, "window": 2}
MONITOR_RULES = [
    "interface='org.freedesktop.portal.ScreenCast'",
    "interface='org.freedesktop.impl.portal.ScreenCast'",
    "type='error'",
]


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def messages(log):
    blocks = []
    for line in log.splitlines():
        if line and not line[0].isspace():
            blocks.append([line])
        elif blocks:
            blocks[-1].append(line)
    return blocks


def selected_types(block):
    text = "\n".join(block)
    match = re.search(r'string "types"\s+variant\s+uint32 (\d+)', text)
    return int(match[1]) if match else None


def analyze(log, source):
    blocks = messages(log)
    frontend_sessions = [block for block in blocks if "interface=org.freedesktop.portal.ScreenCast; member=CreateSession" in block[0]]
    frontend_selections = [block for block in blocks if "interface=org.freedesktop.portal.ScreenCast; member=SelectSources" in block[0]]
    backend_selections = [block for block in blocks if "interface=org.freedesktop.impl.portal.ScreenCast; member=SelectSources" in block[0]]
    denials = [block for block in blocks if block[0].startswith("error") and ("AccessDenied" in block[0] or any("not allowed" in line for line in block))]
    return {
        "frontendSessions": len(frontend_sessions),
        "frontendSelections": [selected_types(block) for block in frontend_selections],
        "backendSelections": [selected_types(block) for block in backend_selections],
        "denials": len(denials),
        "expectedType": SOURCE_TYPES[source],
    }


def validate(summary):
    require(summary["denials"] == 0, "The desktop portal denied a request from the sandboxed web process")
    require(summary["frontendSessions"] >= 1, "WebKit did not create a screen cast session")
    require(summary["frontendSelections"] == [summary["expectedType"]], "WebKit did not request the chosen source type")
    require(summary["backendSelections"] == [summary["expectedType"]], "The portal did not forward the chosen source type to the desktop")


def inside(arguments):
    runtime = Path(os.environ["XDG_RUNTIME_DIR"])
    stage = arguments.stage
    compositor = subprocess.Popen(
        ["mutter", "--headless", "--wayland", "--no-x11", "--virtual-monitor", "1600x1000", "--wayland-display", SOCKET],
        stdout=(stage / "mutter.log").open("w"),
        stderr=subprocess.STDOUT,
    )
    monitor = None
    try:
        deadline = time.monotonic() + 20
        while not (runtime / SOCKET).exists():
            require(compositor.poll() is None and time.monotonic() < deadline, "Headless mutter did not start")
            time.sleep(0.05)
        monitor = subprocess.Popen(
            ["dbus-monitor", "--session", *MONITOR_RULES],
            stdout=(stage / "portal.log").open("w"),
            stderr=subprocess.STDOUT,
        )
        time.sleep(0.5)
        command = [str(arguments.build / "eden-wpe-tests"), "-nocrashhandler", "displayCaptureRequestsTheDesktopPortal"]
        if arguments.webkit_library:
            command = [
                "bwrap", "--dev-bind", "/", "/", "--ro-bind", str(arguments.webkit_library), str(arguments.webkit_target), "--", *command,
            ]
        with (stage / "test.log").open("w") as log:
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=arguments.timeout_seconds)
        return result.returncode
    finally:
        if monitor:
            monitor.terminate()
            monitor.wait(5)
        compositor.terminate()
        try:
            compositor.wait(10)
        except subprocess.TimeoutExpired:
            compositor.kill()


def run(build, source, timeout_seconds, stage, webkit_library=None, webkit_target=None):
    require((build / "eden-wpe-tests").is_file(), f"Missing eden-wpe-tests in {build}")
    for tool in ("dbus-run-session", "dbus-monitor", "mutter", "bwrap"):
        require(shutil.which(tool), f"The desktop portal check requires {tool}")
    runtime = Path(tempfile.mkdtemp(prefix="eden-portal-", dir="/tmp"))
    try:
        runtime.chmod(0o700)
        environment = {
            key: value for key, value in os.environ.items()
            if not key.startswith(("DBUS_", "WAYLAND_", "JSC_", "WEBKIT_", "EDEN_WPE_", "GST_", "LLVM_PROFILE"))
            and key not in ("DISPLAY", "XAUTHORITY", "XDG_RUNTIME_DIR", "XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME")
        }
        environment.update({
            "XDG_RUNTIME_DIR": str(runtime),
            "XDG_DATA_HOME": str(runtime / "data"),
            "XDG_CONFIG_HOME": str(runtime / "config"),
            "XDG_CACHE_HOME": str(runtime / "cache"),
            "XDG_CURRENT_DESKTOP": "GNOME",
            "XDG_SESSION_TYPE": "wayland",
            "WAYLAND_DISPLAY": SOCKET,
            "QT_QPA_PLATFORM": "wayland",
            "EDEN_WPE_PREWARM": "0",
            "EDEN_TEST_DESKTOP_PORTAL": source,
            "EDEN_TEST_DESKTOP_PORTAL_WAIT_MS": "6000",
        })
        command = [
            "dbus-run-session", "--", sys.executable, str(Path(__file__).resolve()), "--inside",
            "--build", str(build), "--source", source, "--stage", str(stage), "--timeout-seconds", str(timeout_seconds),
        ]
        if webkit_library:
            command += ["--webkit-library", str(webkit_library), "--webkit-target", str(webkit_target)]
        completed = subprocess.run(command, env=environment, timeout=timeout_seconds + 60, check=False)
        log = (stage / "portal.log").read_text() if (stage / "portal.log").exists() else ""
        summary = analyze(log, source)
        summary["testExit"] = completed.returncode
        test_log = (stage / "test.log").read_text() if (stage / "test.log").exists() else ""
        outcome = re.search(r"Desktop portal display capture outcome: (.*)", test_log)
        summary["pageOutcome"] = outcome[1] if outcome else None
        summary["passed"] = False
        try:
            require(completed.returncode == 0, f"The portal test program exited with {completed.returncode}")
            validate(summary)
            summary["passed"] = True
        finally:
            (stage / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        return summary
    finally:
        shutil.rmtree(runtime, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path, default=ROOT / "cmake-build-release")
    parser.add_argument("--source", choices=sorted(SOURCE_TYPES), action="append")
    parser.add_argument("--timeout-seconds", type=int, default=120)
    parser.add_argument("--output-directory", type=Path)
    parser.add_argument("--webkit-library", type=Path)
    parser.add_argument("--webkit-target", type=Path)
    parser.add_argument("--inside", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--stage", type=Path, help=argparse.SUPPRESS)
    arguments = parser.parse_args()
    if arguments.inside:
        arguments.source = arguments.source[0]
        sys.exit(inside(arguments))
    require(1 <= arguments.timeout_seconds <= 600, "Timeout must be between 1 and 600 seconds")
    require(bool(arguments.webkit_library) == bool(arguments.webkit_target), "A replacement WebKit library needs its target path")
    results = {}
    try:
        with tempfile.TemporaryDirectory(prefix="eden-portal-check-") as temporary:
            base = arguments.output_directory.resolve() if arguments.output_directory else Path(temporary)
            for source in arguments.source or sorted(SOURCE_TYPES):
                stage = base / source
                stage.mkdir(parents=True, exist_ok=False)
                results[source] = run(arguments.build.resolve(), source, arguments.timeout_seconds, stage, arguments.webkit_library, arguments.webkit_target)
        print(json.dumps(results, indent=2))
    except Exception as error:
        raise SystemExit(f"Desktop portal display capture check failed: {type(error).__name__}: {error}") from error


if __name__ == "__main__":
    main()
