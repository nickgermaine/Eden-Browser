import argparse
import hashlib
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import sys
import tempfile
import time

project = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(project / "scripts/bench/wpe"))
from sweep import conditions


def fingerprints(paths):
    result = {}
    for path in paths:
        with path.open("rb") as source:
            result[str(path.resolve())] = hashlib.file_digest(source, "sha256").hexdigest()
    return result


parser = argparse.ArgumentParser()
parser.add_argument("--build", type=Path, default=project / "cmake-build-release")
parser.add_argument("--perf-tool", type=Path)
parser.add_argument("--mode", choices=["shell", "private", "adapter"], required=True)
parser.add_argument("--output", type=Path, required=True)
parser.add_argument("--inside", action="store_true")
parser.add_argument("--ack", choices=["default", "import", "swap"], default="default")
parser.add_argument("--profile", action="store_true")
parser.add_argument("--diagnostic", action="store_true")
args = parser.parse_args()
output = args.output.resolve()
build = args.build.resolve()
if args.profile and not args.perf_tool:
    parser.error("--profile requires --perf-tool")
if args.perf_tool:
    args.perf_tool = args.perf_tool.resolve()
if not args.inside:
    output.mkdir(parents=True, exist_ok=False)
    runtime = Path(tempfile.mkdtemp(prefix="eden-shell-measure-"))
    environment = os.environ.copy()
    for name in list(environment):
        if name.startswith(("JSC_", "WEBKIT_", "EDEN_WPE_", "SYSPROF_", "LLVM_PROFILE", "EDEN_AUTOMATION")):
            environment.pop(name)
    for name in ["LD_LIBRARY_PATH", "EDEN_PERF", "EDEN_BENCH_FRAMES"]:
        environment.pop(name, None)
    environment.update(
        {
            name: str(runtime / value)
            for name, value in {
                "XDG_RUNTIME_DIR": ".",
                "XDG_DATA_HOME": "data",
                "XDG_CONFIG_HOME": "config",
                "XDG_CACHE_HOME": "cache",
            }.items()
        }
    )
    (output / "runtime-path.txt").write_text(str(runtime))
    command = [
        "dbus-run-session",
        "--",
        sys.executable,
        __file__,
        "--inside",
        "--build",
        str(build),
        "--mode",
        args.mode,
        "--output",
        str(output),
        "--ack",
        args.ack,
    ]
    if args.profile:
        command += ["--profile", "--perf-tool", str(args.perf_tool)]
    if args.diagnostic:
        command.append("--diagnostic")
    raise SystemExit(subprocess.run(command, env=environment).returncode)

runtime = Path(os.environ["XDG_RUNTIME_DIR"])
tools = project / "third_party/wpe/tools/usr"
modules = [tools / "lib/weston/kiosk-shell.so"]
for name in ["headless-backend.so", "gl-renderer.so"]:
    modules.append(sorted((tools / "lib").glob("libweston-*/" + name))[-1])
environment = os.environ.copy()
environment["LD_LIBRARY_PATH"] = str(tools / "lib") + ":" + str(tools / "lib/weston")
environment["WESTON_MODULE_MAP"] = "".join(path.name + "=" + str(path) + ";" for path in modules)
processes = []
try:
    compositor = subprocess.Popen(
        [
            str(tools / "bin/weston"),
            "--backend=headless",
            "--renderer=gl",
            "--fake-seat",
            "--socket=eden-shell-measure",
            "--idle-time=0",
            "--no-config",
            "--shell=kiosk",
            "--width=1600",
            "--height=1200",
            "--log=" + str(output / "compositor.log"),
        ],
        env=environment,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    processes.append(compositor)
    deadline = time.monotonic() + 15
    while not (runtime / "eden-shell-measure").exists():
        if compositor.poll() is not None or time.monotonic() > deadline:
            raise RuntimeError("Private compositor failed")
        time.sleep(0.02)
    with (output / "keyring.log").open("w") as log:
        keyring = subprocess.Popen(
            [
                "gnome-keyring-daemon",
                "--foreground",
                "--unlock",
                "--components=secrets",
                "--control-directory=" + str(runtime / "keyring"),
            ],
            stdin=subprocess.PIPE,
            stdout=log,
            stderr=log,
        )
        processes.append(keyring)
        keyring.stdin.write((secrets.token_urlsafe(32) + "\n").encode())
        keyring.stdin.close()
    time.sleep(0.5)
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        port = listener.getsockname()[1]
    environment = os.environ.copy()
    environment.update(
        {
            "WAYLAND_DISPLAY": "eden-shell-measure",
            "QT_QPA_PLATFORM": "wayland",
            "QT_FORCE_STDERR_LOGGING": "1",
            "QT_SCALE_FACTOR": "1",
            "WEBKIT_INSPECTOR_HTTP_SERVER": f"127.0.0.1:{port}",
            "EDEN_WPE_REQUIRE_GPU": "1",
            "WEBKIT_SKIA_CANVAS_REPLAY_THREADS": "2",
        }
    )
    if args.ack != "default":
        environment["EDEN_WPE_FRAME_ACK"] = args.ack
    url = "https://browserbench.org/Speedometer3.1/"
    if args.mode == "adapter":
        command = [
            str(build / "eden-wpe-bench"),
            "--external-driver",
            "--viewport-width",
            "1592",
            "--viewport-height",
            "1096",
            "--url",
            url,
        ]
    else:
        command = [str(build / "eden-browser"), "--engine=wpe", *(["--private"] if args.mode == "private" else []), url]
    cache = (build / "CMakeCache.txt").read_text()
    prefix_line = next(line for line in cache.splitlines() if line.startswith("EDEN_WPE_SELECTED_PREFIX:"))
    prefix = Path(prefix_line.split("=", 1)[1])
    binaries = [
        build / "eden-browser",
        build / "eden-wpe-bench",
        build / "eden-engine-wpe.so",
        build / "libeden-core.so",
        build / "libeden-ui.so",
        prefix / "lib/libWPEWebKit-2.0.so",
    ]
    hashes = fingerprints(binaries)
    preflight = conditions(0.03)
    deadline = time.monotonic() + 90
    while not args.diagnostic and not preflight["idle"] and time.monotonic() < deadline:
        time.sleep(2)
        preflight = conditions(0.03)
    (output / "preflight.json").write_text(json.dumps(preflight, indent=2))
    if not args.diagnostic and not preflight["idle"]:
        raise RuntimeError("Machine remained busy before run")
    metadata = {
        "mode": args.mode,
        "ack": args.ack,
        "profiled": args.profile,
        "diagnostic": args.diagnostic,
        "command": command,
        "inspectorEndpoint": f"http://127.0.0.1:{port}",
        "binaries": hashes,
        "conditionsBefore": preflight,
        "inspectorDetachedDuringScoring": True,
    }
    (output / "provenance.json").write_text(json.dumps(metadata, indent=2))
    with (output / "browser.log").open("w") as log:
        app = subprocess.Popen(command, env=environment, stdout=log, stderr=log, start_new_session=True)
    processes.append(app)
    monitor_script = project / "scripts/bench/wpe/monitor-load.py"
    monitor = subprocess.Popen(
        [sys.executable, str(monitor_script), "--group", str(app.pid), "--output", str(output / "load.jsonl")]
    )
    processes.append(monitor)
    if args.profile:
        perf = args.perf_tool
        profile_env = os.environ.copy()
        profile_env["LD_LIBRARY_PATH"] = str(perf.parents[1] / "lib")
        with (output / "profile.log").open("w") as log:
            profiler = subprocess.Popen(
                [
                    str(perf),
                    "record",
                    "-p",
                    str(app.pid),
                    "-e",
                    "cycles:u",
                    "-F",
                    "199",
                    "--call-graph",
                    "dwarf,16384",
                    "-o",
                    str(output / "shell.perf"),
                ],
                env=profile_env,
                stdout=log,
                stderr=log,
            )
        processes.append(profiler)
    with (output / "driver.log").open("w") as log:
        driver = subprocess.Popen(
            [
                "node",
                str(project / "scripts/bench/wpe/remote-score.mjs"),
                f"http://127.0.0.1:{port}",
                str(output / "result.json"),
            ],
            stdout=log,
            stderr=log,
        )
    processes.append(driver)
    deadline = time.monotonic() + 1850
    while driver.poll() is None:
        if app.poll() is not None:
            raise RuntimeError("Browser exited before the result was collected")
        if time.monotonic() > deadline:
            raise RuntimeError("Benchmark driver timed out")
        time.sleep(0.2)
    if driver.returncode:
        raise RuntimeError("Benchmark driver failed: " + (output / "driver.log").read_text())
    metadata["conditionsAfter"] = conditions(1)
    metadata["browserPid"] = app.pid
    metadata["binariesAfter"] = fingerprints(binaries)
    metadata["binariesStable"] = hashes == metadata["binariesAfter"]
    (output / "provenance.json").write_text(json.dumps(metadata, indent=2))
    result = json.loads((output / "result.json").read_text())
    if not metadata["binariesStable"]:
        raise RuntimeError("Browser or engine binaries changed during the benchmark")
    if result["viewport"] != [1592, 1096, 1]:
        raise RuntimeError("Page viewport does not match the shell comparison")
    print(
        json.dumps({"mode": args.mode, "ack": args.ack, "score": result["score"], "viewport": result["viewport"]}),
        flush=True,
    )
finally:
    for process in reversed(processes):
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
    failed_mounts = []
    for relative in ["cache/Eden/Eden/profiles", "data/Eden/Eden/engine-data"]:
        mount = runtime / relative
        if os.path.ismount(mount):
            try:
                subprocess.run(["fusermount3", "-u", str(mount)], check=True, timeout=10)
            except (OSError, subprocess.SubprocessError):
                failed_mounts.append(str(mount))
    if failed_mounts:
        raise RuntimeError("Temporary profile mounts remain active: " + ", ".join(failed_mounts))
