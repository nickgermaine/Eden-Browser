import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import statistics
import subprocess
import sys
import tempfile

from validate_intrinsic_width import compare_exact


COLLECTION_CHECKS = (
    "indexed wrapper identity",
    "maximum array index remains a named property",
    "ordinary indexed assignment creates no expando",
    "ordinary string-key indexed assignment creates no expando",
    "strict ordinary indexed assignment still throws",
    "indexed defineOwnProperty still throws",
    "prototype indexed getter remains visible",
    "maximum array index remains inherited",
    "HTMLFormControlsCollection indexed wrapper identity",
    "HTMLFormControlsCollection named lookup",
    "HTMLOptionsCollection indexed wrapper identity",
    "site quirk indexed expando remains readable",
    "site quirk string-key indexed expando remains readable",
    "quirk expando survives reads from a non-quirks realm",
    "site quirk does not alter indexed defineOwnProperty",
    "site quirk indexed expando remains deletable",
    "site quirk string-key indexed expando remains deletable",
)
SVG_CHECKS = (
    "repeated equal attribute writes preserve mutation notifications",
    "changed attribute value reparses",
    "attribute removal resets the reflected length",
    "empty length uses its default value",
    "invalid length remains observable",
    "negative forbidden length remains observable",
    "pixel units survive repeated writes",
    "percentage units survive repeated writes",
    "calc length is rejected",
    "baseVal writes synchronize before an equal attribute write",
    "cloned element has independent animated length state",
    "circle repeated length attribute remains reflected",
    "line repeated length attribute remains reflected",
    "ellipse repeated length attribute remains reflected",
    "use repeated length attribute remains reflected",
    "image repeated length attribute remains reflected",
    "foreignObject repeated length attribute remains reflected",
    "active SMIL animation retains parser update semantics",
    "active SMIL animation continues after the equal attribute write",
)
TIMINGS = (
    "collectionMissMilliseconds",
    "collectionTraversalMilliseconds",
    "svgEqualWriteMilliseconds",
    "svgChangingWriteMilliseconds",
)


def validate_records(records, expected, label):
    if not isinstance(records, list) or any(not isinstance(record, dict) for record in records):
        raise ValueError(f"{label}: malformed check records")
    names = [record.get("name") for record in records]
    if tuple(names) != expected:
        raise ValueError(f"{label}: expected checks {expected}, observed {names}")
    failed = [record["name"] for record in records if record.get("passed") is not True]
    if failed:
        raise ValueError(f"{label}: failed checks {failed}")


def validate_result(value, label):
    if not isinstance(value, dict) or value.get("probe") != "dom-fast-paths":
        raise ValueError(f"{label}: missing DOM fast path result")
    if value.get("visibility") != "visible":
        raise ValueError(f"{label}: page was not visible")
    viewport = value.get("viewport")
    if not isinstance(viewport, list) or len(viewport) != 3 or any(
        isinstance(number, bool) or not isinstance(number, (int, float))
        or not math.isfinite(number) or number <= 0 for number in viewport
    ):
        raise ValueError(f"{label}: malformed viewport")
    measurements = value.get("measurements")
    if not isinstance(measurements, dict) or measurements.get("failures") != []:
        raise ValueError(f"{label}: failures {measurements.get('failures') if isinstance(measurements, dict) else measurements}")
    validate_records(measurements.get("collection"), COLLECTION_CHECKS, label + ".collection")
    validate_records(measurements.get("svg"), SVG_CHECKS, label + ".svg")
    timings = measurements.get("timings")
    if not isinstance(timings, dict) or set(timings) != set(TIMINGS):
        raise ValueError(f"{label}: timing samples are missing")
    for name in TIMINGS:
        samples = timings[name]
        if not isinstance(samples, list) or len(samples) != 5 or any(
            isinstance(sample, bool) or not isinstance(sample, (int, float))
            or not math.isfinite(sample) or sample <= 0 for sample in samples
        ):
            raise ValueError(f"{label}: malformed {name} samples")
    runtime = value.get("runtime")
    if not isinstance(runtime, dict) or not runtime.get("library"):
        raise ValueError(f"{label}: runtime library identity missing")
    return value


def environment_for(svg_skip):
    environment = os.environ.copy()
    for name in list(environment):
        if name.startswith(("JSC_", "WEBKIT_", "EDEN_WPE_", "SYSPROF_", "LLVM_PROFILE")):
            environment.pop(name)
    environment["EDEN_WPE_PREWARM"] = "0"
    environment["WEBKIT_SKIA_CANVAS_REPLAY_THREADS"] = "2"
    if not svg_skip:
        environment["WEBKIT_EDEN_DISABLE_SVG_LENGTH_PARSE_SKIP"] = "1"
    return environment


def library_digest(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def compose_probe(root):
    return "\n".join((
        (root / "scripts/wpe/adopted/collection_index_test.js").read_text(),
        (root / "tests/wpe/svg-length-parse.js").read_text(),
        (root / "tests/wpe/dom-fast-paths.js").read_text(),
    ))


def run_validation(build, output, scales=(1, 2), runner=subprocess.run, timeout_seconds=240):
    root = Path(__file__).resolve().parents[3]
    build = Path(build).resolve()
    output = Path(output)
    output.mkdir(parents=True, exist_ok=False)
    summary = {"passed": False, "runs": [], "scales": []}
    try:
        if not (build / "eden-wpe-bench").is_file():
            raise FileNotFoundError(f"Missing benchmark binary in {build}")
        fixture = compose_probe(root)
        (output / "probe.js").write_text(fixture)
        with tempfile.TemporaryDirectory(prefix="eden-dom-fast-paths-") as temporary:
            stage = Path(temporary)
            probe = stage / "probe.js"
            probe.write_text(fixture)
            identity = None
            for scale in scales:
                paired = {}
                for mode in ("skip-disabled", "skip-enabled"):
                    label = f"dom-fast-paths-{mode}-scale-{scale}"
                    staged_result = stage / f"{label}.json"
                    saved_result = output / staged_result.name
                    saved_log = output / f"{label}.log"
                    environment = environment_for(mode == "skip-enabled")
                    profile_directory = stage / label
                    profile_directory.mkdir()
                    environment["LLVM_PROFILE_FILE"] = str(profile_directory / "%p-%m.profraw")
                    command = [sys.executable, str(root / "scripts/test_wpe.py"), "--build", str(build),
                               "--scale", str(scale), "--timeout-seconds", str(timeout_seconds), "--benchmark", "--",
                               "--output", str(staged_result), "--label", label, "--probe-script", str(probe),
                               "--training-directory", str(profile_directory)]
                    record = {"label": label, "mode": mode, "scale": scale, "log": saved_log.name}
                    summary["runs"].append(record)
                    try:
                        with saved_log.open("w") as log:
                            completed = runner(command, env=environment, stdout=log, stderr=subprocess.STDOUT,
                                               check=False, timeout=timeout_seconds + 45)
                        record["returnCode"] = completed.returncode
                    finally:
                        if staged_result.exists():
                            shutil.copyfile(staged_result, saved_result)
                            record["result"] = saved_result.name
                    if completed.returncode:
                        raise RuntimeError(f"{label}: process exited {completed.returncode}; see {saved_log}")
                    value = validate_result(json.loads(saved_result.read_text()), label)
                    record["medianMilliseconds"] = {
                        name: statistics.median(value["measurements"]["timings"][name]) for name in TIMINGS
                    }
                    current = {"library": value["runtime"]["library"], "sha256": library_digest(value["runtime"]["library"])}
                    if identity is None:
                        identity = current
                        summary["runtime"] = identity
                    else:
                        compare_exact(identity, current, "runtime identity")
                    paired[mode] = value
                for part in ("collection", "svg"):
                    compare_exact(paired["skip-disabled"]["measurements"][part], paired["skip-enabled"]["measurements"][part], f"scale {scale} {part}")
                summary["scales"].append({"scale": scale, "collectionChecks": len(COLLECTION_CHECKS), "svgChecks": len(SVG_CHECKS), "exactMatch": True})
        summary["passed"] = True
        return summary
    except Exception as error:
        summary["error"] = f"{type(error).__name__}: {error}"
        raise
    finally:
        (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--scale", type=float, action="append")
    arguments = parser.parse_args()
    try:
        result = run_validation(arguments.build, arguments.output, tuple(arguments.scale or (1, 2)))
    except Exception as error:
        raise SystemExit(str(error)) from error
    print(json.dumps(result, indent=2), flush=True)


if __name__ == "__main__":
    main()
