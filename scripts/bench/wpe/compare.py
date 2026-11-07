import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location(
    "sweep", ROOT / "scripts/bench/wpe/sweep.py"
)
sweep = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sweep)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--group", choices=["jit", "rendering"], required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    accepted = "build-webkit-canvas-replay"
    candidate = "build-webkit-rendering-experiments"
    configurations = (
        {
            "accepted": (accepted, {}, None),
            "jit-1.5": (accepted, {"JSC_jitPolicyScale": "1.5"}, None),
            "jit-2": (accepted, {"JSC_jitPolicyScale": "2"}, None),
            "physical-cores": (accepted, {}, "0-15"),
        }
        if args.group == "jit"
        else {
            "accepted": (accepted, {}, None),
            "candidate-off": (candidate, {}, None),
            "simple-text": (candidate, {"WEBKIT_EDEN_SIMPLE_TEXT": "1"}, None),
            "canvas-damage": (candidate, {"WEBKIT_EDEN_CANVAS_DAMAGE": "1"}, None),
            "canvas-worker-cpu": (
                candidate,
                {"WEBKIT_EDEN_CANVAS_CPU_PIXELS": "1048576"},
                None,
            ),
        }
    )
    environment = os.environ.copy()
    for name in list(environment):
        if name.startswith(
            ("JSC_", "WEBKIT_", "EDEN_WPE_", "SYSPROF_", "LLVM_PROFILE")
        ):
            environment.pop(name)
    environment["WEBKIT_SKIA_CANVAS_REPLAY_THREADS"] = "2"
    initial = sweep.conditions(0.03)
    manifest = {}
    hashes = {}
    file_states = {}
    for build, _, _ in configurations.values():
        directory = ROOT / build
        if build in manifest:
            continue
        import re

        prefix = Path(
            re.search(
                r"^EDEN_WPE_PREFIX:PATH=(.+)$",
                (directory / "CMakeCache.txt").read_text(),
                re.M,
            )[1]
        )
        manifest[build] = json.loads((prefix / "share/eden-wpe-build.json").read_text())
        if manifest[build]["mode"] != "performance" or manifest[build].get(
            "framePointers"
        ):
            raise RuntimeError(
                "Scoring requires the ordinary optimized runtime, without profiling instrumentation"
            )
        if build == candidate and not manifest[build].get("renderingExperiments"):
            raise RuntimeError("The candidate runtime lacks the rendering patches")
        for path in [
            directory / "eden-wpe-bench",
            directory / "eden-engine-wpe.so",
            prefix / "lib/libWPEWebKit-2.0.so",
        ]:
            with path.open("rb") as stream:
                hashes[str(path)] = hashlib.file_digest(stream, "sha256").hexdigest()
            file_states[path] = (path.stat().st_size, path.stat().st_mtime_ns)
    order = (
        [[0, 1, 3, 2], [1, 2, 0, 3], [2, 3, 1, 0], [3, 0, 2, 1]]
        if args.group == "jit"
        else [
            [(index + repeat) % 5 for index in [0, 1, 4, 2, 3]] for repeat in range(5)
        ]
    )
    provenance = {
        "control": next(iter(configurations)),
        "repeats": len(order),
        "order": order,
        "configurations": configurations,
        "runtimeBuilds": manifest,
        "binaries": hashes,
        "initialConditions": initial,
        "backgroundCpuLimitPercent": 3,
    }
    records = []
    names = list(configurations)
    for repeat, order in enumerate(provenance["order"], 1):
        for index in order:
            name = names[index]
            deadline = time.monotonic() + 600
            while True:
                before = sweep.conditions(0.03)
                if (
                    before["governors"] != initial["governors"]
                    or before["energyPreferences"] != initial["energyPreferences"]
                ):
                    raise RuntimeError("Power settings changed")
                if before["idle"]:
                    break
                if time.monotonic() >= deadline:
                    raise RuntimeError("Background CPU stayed above 3 percent")
                time.sleep(2)
            build, settings, affinity = configurations[name]
            label = f"{name}-{repeat}"
            print("Running", label, flush=True)
            with tempfile.TemporaryDirectory(
                prefix="eden-rendering-score-"
            ) as temporary:
                result = Path(temporary) / "result.json"
                command = [
                    sys.executable,
                    str(ROOT / "scripts/test_wpe.py"),
                    "--build",
                    str(ROOT / build),
                    "--benchmark",
                    "--",
                    "--output",
                    str(result),
                    "--label",
                    label,
                ]
                if affinity:
                    command = ["taskset", "--cpu-list", affinity] + command
                with (args.output / f"{label}.log").open("w") as log:
                    subprocess.run(
                        command,
                        cwd=ROOT,
                        env=environment | settings,
                        stdout=log,
                        stderr=subprocess.STDOUT,
                        check=True,
                        timeout=1830,
                    )
                contents = result.read_bytes()
                (args.output / f"{label}.json").write_bytes(contents)
            after = sweep.conditions(0.03)
            if (
                after["governors"] != initial["governors"]
                or after["energyPreferences"] != initial["energyPreferences"]
            ):
                raise RuntimeError("Power settings changed during the run")
            summary = sweep.summarize_run(json.loads(contents))
            for path, state in file_states.items():
                if (path.stat().st_size, path.stat().st_mtime_ns) != state:
                    raise RuntimeError(f"Binary changed during scoring: {path}")
            records.append(
                {
                    "configuration": name,
                    "repeat": repeat,
                    "file": f"{label}.json",
                    "conditions": before,
                    "endConditions": after,
                    "summary": summary,
                }
            )
            sweep.write_summary(args.output, records, provenance)
            print(label, round(summary["score"], 3), flush=True)


if __name__ == "__main__":
    main()
