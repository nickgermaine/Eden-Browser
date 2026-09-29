import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time


def main():
    root = Path(__file__).resolve().parents[3]
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    profiler = parser.add_mutually_exclusive_group(required=True)
    profiler.add_argument("--perf", type=Path)
    profiler.add_argument("--sysprof", type=Path)
    parser.add_argument("--probe-script", type=Path)
    parser.add_argument("--patch-counters", action="store_true")
    parser.add_argument("--call-graph", choices=("fp", "dwarf", "none"), default="fp")
    parser.add_argument("--frequency", type=int, default=499)
    args = parser.parse_args()
    if not 1 <= args.frequency <= 999:
        parser.error("--frequency must be between 1 and 999 Hz")
    args.output.mkdir(parents=True, exist_ok=False)
    prefix = Path(
        re.search(
            r"^EDEN_WPE_PREFIX:PATH=(.+)$",
            (args.build / "CMakeCache.txt").read_text(),
            re.M,
        )[1]
    )
    manifest = json.loads((prefix / "share/eden-wpe-build.json").read_text())
    if args.perf and args.call_graph == "fp" and not manifest.get("framePointers"):
        raise RuntimeError(
            "Frame-pointer capture requires a frame-pointer runtime; "
            "use --call-graph dwarf or none for the optimized runtime"
        )
    hashes = {}
    for path in [
        args.build / "eden-wpe-bench",
        args.build / "eden-engine-wpe.so",
        prefix / "lib/libWPEWebKit-2.0.so",
    ]:
        with path.open("rb") as stream:
            hashes[str(path.resolve())] = hashlib.file_digest(
                stream, "sha256"
            ).hexdigest()
    tools = root / "third_party/wpe/tools/usr"
    with tempfile.TemporaryDirectory(prefix="eden-frame-profile-") as temporary:
        runtime = Path(temporary)
        environment = os.environ.copy()
        for name in list(environment):
            if name.startswith(
                ("JSC_", "WEBKIT_", "EDEN_WPE_", "SYSPROF_", "LLVM_PROFILE")
            ):
                environment.pop(name)
        environment.pop("LD_LIBRARY_PATH", None)
        environment.update(
            XDG_RUNTIME_DIR=str(runtime),
            XDG_DATA_HOME=str(runtime / "data"),
            XDG_CONFIG_HOME=str(runtime / "config"),
            XDG_CACHE_HOME=str(runtime / "cache"),
            QT_SCALE_FACTOR="1",
            QT_QPA_PLATFORM="wayland",
            WAYLAND_DISPLAY="eden-profile",
            EDEN_WPE_REQUIRE_GPU="1",
            EDEN_WPE_PREWARM="0",
            WEBKIT_SKIA_CANVAS_REPLAY_THREADS="2",
            QT_FORCE_STDERR_LOGGING="1",
        )
        if args.perf:
            environment.update(
                JSC_useJITDump="true",
                JSC_jitDumpDirectory=str(args.output.resolve()),
            )
        else:
            environment["WEBKIT_ENABLE_SOURCE_DISPATCH_SIGNPOSTS"] = "1"
        if args.patch_counters:
            environment["WEBKIT_EDEN_PATCH_COUNTERS"] = "1"
        compositor_environment = environment.copy()
        compositor_environment["LD_LIBRARY_PATH"] = (
            str(tools / "lib") + ":" + str(tools / "lib/weston")
        )
        modules = [tools / "lib/weston/kiosk-shell.so"]
        for name in ["headless-backend.so", "gl-renderer.so"]:
            modules.append(sorted((tools / "lib").glob("libweston-*/" + name))[-1])
        compositor_environment["WESTON_MODULE_MAP"] = "".join(
            str(p.name) + "=" + str(p) + ";" for p in modules
        )
        compositor = subprocess.Popen(
            [
                str(tools / "bin/weston"),
                "--backend=headless",
                "--renderer=gl",
                "--fake-seat",
                "--socket=eden-profile",
                "--idle-time=0",
                "--no-config",
                "--shell=kiosk",
                "--width=1600",
                "--height=1200",
                "--log=" + str(args.output.resolve() / "weston.log"),
            ],
            env=compositor_environment,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        try:
            deadline = time.monotonic() + 15
            while not (runtime / "eden-profile").exists():
                if compositor.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError("Private compositor failed")
                time.sleep(0.02)
            if args.perf:
                command = [
                    str(args.perf.resolve()),
                    "record",
                    "-e",
                    "cycles:u",
                    "-F",
                    str(args.frequency),
                    "--clockid",
                    "mono",
                    "-o",
                    str(args.output.resolve() / "samples.perf"),
                ]
                if args.call_graph != "none":
                    command += [
                        "--call-graph",
                        "fp" if args.call_graph == "fp" else "dwarf,16384",
                    ]
                label = args.call_graph + "-diagnostic"
            else:
                command = [
                    str(args.sysprof.resolve()),
                    "--no-sysprofd",
                    "--no-perf",
                    "--no-cpu",
                    "--no-memory",
                    "--no-battery",
                    "--no-disk",
                    "--no-network",
                    "--no-decode",
                    "--no-debuginfod",
                    str(args.output.resolve() / "events.syscap"),
                ]
                label = "sysprof-diagnostic"
            command += [
                "--",
                str(args.build.resolve() / "eden-wpe-bench"),
                "--output",
                str(args.output.resolve() / "run.json"),
                "--label",
                label,
                "--viewport-width",
                "1600",
                "--viewport-height",
                "1200",
                "--timings",
                "--training-directory",
                str(args.output.resolve()),
            ]
            if args.probe_script:
                command += ["--probe-script", str(args.probe_script.resolve())]
            environment["LD_LIBRARY_PATH"] = str((args.perf or args.sysprof).resolve().parents[1] / "lib")
            (args.output / "command.json").write_text(
                json.dumps(
                    {
                        "command": command,
                        "runtimeBuild": manifest,
                        "binaries": hashes,
                        "environment": {
                            key: value
                            for key, value in environment.items()
                            if key.startswith(("JSC_", "WEBKIT_", "EDEN_WPE_"))
                        },
                    },
                    indent=2,
                )
                + "\n"
            )
            with (args.output / "record.log").open("w") as log:
                subprocess.run(
                    command,
                    env=environment,
                    stdout=log,
                    stderr=subprocess.STDOUT,
                    check=True,
                    timeout=1830,
                )
        finally:
            compositor.terminate()
            try:
                compositor.wait(timeout=5)
            except subprocess.TimeoutExpired:
                compositor.kill()
                compositor.wait()


if __name__ == "__main__":
    main()
