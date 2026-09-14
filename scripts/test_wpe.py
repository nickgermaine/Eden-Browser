import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time


def inside(build, names, environment, benchmark):
    runtime = Path(environment["XDG_RUNTIME_DIR"])
    log = runtime / "compositor.log"
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
        "--socket=eden-webkit-test",
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
        while not (runtime / "eden-webkit-test").exists():
            if compositor.poll() is not None or time.monotonic() >= deadline:
                if log.exists():
                    print(log.read_text(), file=sys.stderr)
                raise RuntimeError("The private GPU compositor did not start")
            time.sleep(0.02)
        environment["WAYLAND_DISPLAY"] = "eden-webkit-test"
        environment["QT_QPA_PLATFORM"] = "wayland"
        environment["EDEN_WPE_REQUIRE_GPU"] = "1"
        program = "eden-wpe-bench" if benchmark else "eden-wpe-tests"
        arguments = names if benchmark else ["-nocrashhandler", *names]
        result = subprocess.run([str(build / program), *arguments], env=environment, timeout=1830 if benchmark else 300)
        return result.returncode
    finally:
        compositor.terminate()
        try:
            compositor.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            compositor.kill()
            compositor.communicate()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path, default=Path("build-webkit"))
    parser.add_argument("--scale", type=float, default=1)
    parser.add_argument("--benchmark", action="store_true")
    parser.add_argument("--inside", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("tests", nargs="*")
    arguments = parser.parse_args()
    build = arguments.build.resolve()
    program = "eden-wpe-bench" if arguments.benchmark else "eden-wpe-tests"
    if not (build / program).is_file():
        parser.error(f"Build the {program} target first")
    if arguments.scale <= 0 or arguments.scale > 4:
        parser.error("Scale must be between zero and four")
    environment = os.environ.copy()
    for name in ["LD_LIBRARY_PATH", "PKG_CONFIG_PATH", "WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS"]:
        environment.pop(name, None)
    if arguments.inside:
        return inside(build, arguments.tests, environment, arguments.benchmark)
    if not shutil.which("bwrap"):
        parser.error("Install bubblewrap to isolate the native test display")
    with tempfile.TemporaryDirectory(prefix="eden-webkit-native-") as temporary:
        runtime = Path(temporary)
        environment["XDG_RUNTIME_DIR"] = str(runtime)
        environment["XDG_DATA_HOME"] = str(runtime / "data")
        environment["XDG_CACHE_HOME"] = str(runtime / "cache")
        environment["XDG_CONFIG_HOME"] = str(runtime / "config")
        environment["QT_SCALE_FACTOR"] = str(arguments.scale)
        environment["QTEST_DISABLE_STACK_DUMP"] = "1"
        command = [
            "bwrap",
            "--ro-bind",
            "/",
            "/",
            "--dev-bind",
            "/dev",
            "/dev",
            "--proc",
            "/proc",
            "--bind",
            "/tmp",
            "/tmp",
        ]
        nss = Path.home() / ".pki"
        if nss.is_dir():
            command += ["--tmpfs", str(nss)]
        command += ["--", sys.executable, str(Path(__file__).resolve()), "--inside", "--build", str(build)]
        if arguments.benchmark:
            command.append("--benchmark")
        command += ["--", *arguments.tests]
        return subprocess.run(command, env=environment, timeout=1860 if arguments.benchmark else 330).returncode


if __name__ == "__main__":
    sys.exit(main())
