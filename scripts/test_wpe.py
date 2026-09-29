import argparse
import contextlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time


@contextlib.contextmanager
def private_compositor(environment):
    runtime = Path(environment["XDG_RUNTIME_DIR"])
    socket = environment.get("EDEN_TEST_WAYLAND_SOCKET", "eden-webkit-test")
    log = Path(environment.get("XDG_CACHE_HOME", str(runtime))) / "compositor.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    tools = Path(__file__).resolve().parents[1] / "third_party/wpe/tools/usr"
    compositor_environment = environment.copy()
    executable = shutil.which("weston")
    if (tools / "bin/weston").is_file():
        executable = str(tools / "bin/weston")
        compositor_environment["LD_LIBRARY_PATH"] = os.pathsep.join(
            str(path) for path in [tools / "lib", tools / "lib/weston"]
        )
        modules = [tools / "lib/weston/kiosk-shell.so"]
        for name in ["headless-backend.so", "gl-renderer.so"]:
            matches = sorted((tools / "lib").glob(f"libweston-*/{name}"))
            if not matches:
                raise RuntimeError(f"The local compositor is missing {name}")
            modules.append(matches[-1])
        compositor_environment["WESTON_MODULE_MAP"] = "".join(f"{path.name}={path};" for path in modules)
    if not executable:
        raise RuntimeError("Install Weston to run the private GPU tests")
    command = [
        executable,
        "--backend=headless",
        "--renderer=gl",
        "--fake-seat",
        f"--socket={socket}",
        "--idle-time=0",
        "--no-config",
        "--shell=kiosk",
        "--width=1600",
        "--height=1200",
        f"--log={log}",
    ]
    compositor = subprocess.Popen(
        command, env=compositor_environment, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE
    )
    try:
        deadline = time.monotonic() + 15
        while not (runtime / socket).exists():
            if compositor.poll() is not None or time.monotonic() >= deadline:
                if log.exists():
                    print(log.read_text(), file=sys.stderr)
                raise RuntimeError("The private GPU compositor did not start")
            time.sleep(0.02)
        environment["WAYLAND_DISPLAY"] = socket
        environment["QT_QPA_PLATFORM"] = "wayland"
        environment["EDEN_WPE_REQUIRE_GPU"] = "1"
        yield environment
    finally:
        compositor.terminate()
        try:
            compositor.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            compositor.kill()
            compositor.communicate()


def inside(build, names, environment, benchmark, test_program, timeout_seconds=None):
    with private_compositor(environment):
        program = "eden-wpe-bench" if benchmark else test_program
        arguments = names if benchmark else ["-nocrashhandler", *names]
        result = subprocess.run([str(build / program), *arguments], env=environment, timeout=timeout_seconds or (1830 if benchmark else 600))
        return result.returncode


SILENT_AUDIO_SINKS = ("pulsesink", "pipewiresink", "alsasink", "jackaudiosink", "osssink", "oss4sink", "openalsink")


def silence_audio_output(environment):
    ranks = [entry for entry in environment.get("GST_PLUGIN_FEATURE_RANK", "").split(",") if entry and entry.split(":", 1)[0] not in SILENT_AUDIO_SINKS]
    environment["GST_PLUGIN_FEATURE_RANK"] = ",".join([*ranks, *(f"{sink}:NONE" for sink in SILENT_AUDIO_SINKS)])


def seed_gstreamer_registry(environment):
    inspector = shutil.which("gst-inspect-1.0")
    if not inspector:
        print("gst-inspect-1.0 is missing, so web processes will scan GStreamer plugins on first media use", file=sys.stderr)
        return
    subprocess.run([inspector, "--exists", "capsfilter"], env=environment, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=300, check=False)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path, default=Path("build-webkit"))
    parser.add_argument("--scale", type=float, default=1)
    parser.add_argument("--benchmark", action="store_true")
    parser.add_argument("--test-program", choices=["eden-wpe-tests", "eden-cef-handlers-tests"], default="eden-wpe-tests")
    parser.add_argument("--timeout-seconds", type=int)
    parser.add_argument("--writable-path", type=Path, action="append", default=[])
    parser.add_argument("--real-session", action="store_true", help="Keep the desktop session's runtime directory so its audio server and D-Bus services are reachable")
    parser.add_argument("--allow-webkit-sandbox-disabled", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--inside", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("tests", nargs="*")
    arguments = parser.parse_args()
    build = arguments.build.resolve()
    program = "eden-wpe-bench" if arguments.benchmark else arguments.test_program
    if not (build / program).is_file():
        parser.error(f"Build the {program} target first")
    if arguments.scale <= 0 or arguments.scale > 4:
        parser.error("Scale must be between zero and four")
    if arguments.timeout_seconds is not None and not 1 <= arguments.timeout_seconds <= 14400:
        parser.error("Timeout must be between 1 and 14400 seconds")
    if (arguments.allow_webkit_sandbox_disabled
            and (not arguments.benchmark or arguments.test_program != "eden-wpe-tests"
                 or os.environ.get("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS") != "1")):
        parser.error("Sandbox opt-out is available only for an isolated WPE training benchmark")
    for path in arguments.writable_path:
        if not path.is_dir() or path.resolve() in (Path("/"), Path.home()):
            parser.error("Writable paths must be existing task directories")
    environment = os.environ.copy()
    for name in ["LD_LIBRARY_PATH", "PKG_CONFIG_PATH"]:
        environment.pop(name, None)
    if not arguments.allow_webkit_sandbox_disabled:
        environment.pop("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS", None)
    if arguments.inside:
        return inside(build, arguments.tests, environment, arguments.benchmark, arguments.test_program, arguments.timeout_seconds)
    if arguments.test_program != "eden-cef-handlers-tests" and not shutil.which("bwrap"):
        parser.error("Install bubblewrap to isolate the native test display")
    with tempfile.TemporaryDirectory(prefix="eden-webkit-native-") as temporary:
        runtime = Path(temporary)
        session_runtime = os.environ.get("XDG_RUNTIME_DIR") if arguments.real_session else None
        if arguments.real_session and not (session_runtime and Path(session_runtime).is_dir()):
            parser.error("--real-session needs the desktop session's XDG_RUNTIME_DIR")
        if session_runtime:
            environment["EDEN_TEST_WAYLAND_SOCKET"] = f"eden-webkit-test-{os.getpid()}"
        else:
            environment["XDG_RUNTIME_DIR"] = str(runtime)
        environment["XDG_DATA_HOME"] = str(runtime / "data")
        environment["XDG_CACHE_HOME"] = str(runtime / "cache")
        environment["XDG_CONFIG_HOME"] = str(runtime / "config")
        environment["QT_SCALE_FACTOR"] = str(arguments.scale)
        environment["QTEST_DISABLE_STACK_DUMP"] = "1"
        if arguments.test_program == "eden-cef-handlers-tests":
            return inside(build, arguments.tests, environment, arguments.benchmark, arguments.test_program, arguments.timeout_seconds)
        seed_gstreamer_registry(environment)
        silence_audio_output(environment)
        command = [
            "bwrap",
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
        ]
        if session_runtime:
            command += ["--bind", session_runtime, session_runtime]
        for path in arguments.writable_path:
            command += ["--bind", str(path.resolve()), str(path.resolve())]
        nss = Path.home() / ".pki"
        if nss.is_dir():
            command += ["--tmpfs", str(nss)]
        command += ["--", sys.executable, str(Path(__file__).resolve()), "--inside", "--build", str(build)]
        if arguments.allow_webkit_sandbox_disabled:
            command.append("--allow-webkit-sandbox-disabled")
        if arguments.timeout_seconds:
            command += ["--timeout-seconds", str(arguments.timeout_seconds)]
        if arguments.benchmark:
            command.append("--benchmark")
        command += ["--test-program", arguments.test_program, "--", *arguments.tests]
        return subprocess.run(command, env=environment, timeout=(arguments.timeout_seconds + 30) if arguments.timeout_seconds else (1860 if arguments.benchmark else 630)).returncode


if __name__ == "__main__":
    sys.exit(main())
