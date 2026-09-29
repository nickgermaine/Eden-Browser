import argparse
import base64
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


EXPECTED_GEOMETRY_CASES = 99
COUNTER_PREFIX = "Eden intrinsic width probe:"
COUNTER_PATTERN = re.compile(
    r"event=(\d+) path=([a-z-]+) candidate=([01]) eligible=([01]) lines=(\d+) "
    r"start_ns=(\d+) duration_ns=(\d+) eligibility_ns=(\d+)"
)


def compare_exact(left, right, location="geometry"):
    if isinstance(left, bool) or isinstance(right, bool):
        if type(left) is not type(right) or left != right:
            raise AssertionError(f"{location}: {left!r} != {right!r}")
        return
    if isinstance(left, (int, float)) and isinstance(right, (int, float)):
        if not math.isfinite(left) or not math.isfinite(right) or left != right:
            raise AssertionError(f"{location}: {left!r} != {right!r}")
        return
    if isinstance(left, dict) and isinstance(right, dict):
        if left.keys() != right.keys():
            raise AssertionError(f"{location}: object keys changed")
        for key in sorted(left):
            compare_exact(left[key], right[key], f"{location}.{key}")
        return
    if isinstance(left, list) and isinstance(right, list):
        if len(left) != len(right):
            raise AssertionError(f"{location}: changed line or text range count")
        for index, (left_value, right_value) in enumerate(zip(left, right, strict=True)):
            compare_exact(left_value, right_value, f"{location}[{index}]")
        return
    if type(left) is not type(right) or left != right:
        raise AssertionError(f"{location}: {left!r} != {right!r}")


def validate_result(value, label):
    if not isinstance(value, dict) or value.get("probe") != "intrinsic-width":
        raise ValueError(f"{label}: result is not an intrinsic-width probe")
    if value.get("visibility") != "visible":
        raise ValueError(f"{label}: page visibility was not visible")
    viewport = value.get("viewport")
    if (
        not isinstance(viewport, list)
        or len(viewport) != 3
        or any(
            not isinstance(item, (int, float))
            or isinstance(item, bool)
            or not math.isfinite(item)
            for item in viewport
        )
        or any(item <= 0 for item in viewport)
    ):
        raise ValueError(f"{label}: viewport is missing or malformed")
    measurements = value.get("measurements")
    geometry = measurements.get("geometry") if isinstance(measurements, dict) else None
    if not isinstance(geometry, list) or len(geometry) != EXPECTED_GEOMETRY_CASES:
        raise ValueError(f"{label}: expected {EXPECTED_GEOMETRY_CASES} geometry cases")
    names = set()

    def coordinates(item, length):
        return (
            isinstance(item, list)
            and len(item) == length
            and all(
                isinstance(number, (int, float))
                and not isinstance(number, bool)
                and math.isfinite(number)
                for number in item
            )
        )

    for index, record in enumerate(geometry):
        if not isinstance(record, dict):
            raise ValueError(f"{label}: geometry case {index} is malformed")
        name = record.get("name")
        if not isinstance(name, str) or not name or name in names:
            raise ValueError(f"{label}: geometry case {index} has a missing or duplicate name")
        names.add(name)
        if record.get("layout") not in {"direct", "flex", "grid"}:
            raise ValueError(f"{label}: geometry case {name} has an invalid layout")
        if not isinstance(record.get("whiteSpace"), str) or not record["whiteSpace"]:
            raise ValueError(f"{label}: geometry case {name} has no computed white-space value")
        if not isinstance(record.get("font"), str) or not record["font"]:
            raise ValueError(f"{label}: geometry case {name} has no computed font")
        if not coordinates(record.get("host"), 4) or not coordinates(record.get("target"), 4):
            raise ValueError(f"{label}: geometry case {name} has malformed element geometry")
        if record.get("peer") is not None and not coordinates(record["peer"], 4):
            raise ValueError(f"{label}: geometry case {name} has malformed peer geometry")
        if not coordinates(record.get("scroll"), 2):
            raise ValueError(f"{label}: geometry case {name} has malformed scroll geometry")
        if not isinstance(record.get("hardBreaks"), list) or any(
            not coordinates(rect, 4) for rect in record["hardBreaks"]
        ):
            raise ValueError(f"{label}: geometry case {name} has malformed hard break geometry")
        if not isinstance(record.get("textRanges"), list) or not record["textRanges"]:
            raise ValueError(f"{label}: geometry case {name} has no text ranges")
        for range_index, text_range in enumerate(record["textRanges"]):
            if (
                not isinstance(text_range, dict)
                or not isinstance(text_range.get("text"), str)
                or not isinstance(text_range.get("rects"), list)
                or any(not coordinates(rect, 4) for rect in text_range["rects"])
            ):
                raise ValueError(f"{label}: geometry case {name} has malformed text range {range_index}")
    if "simple-min-content-hit" not in names:
        raise ValueError(f"{label}: simple min-content trigger case is missing")
    for white_space in ("normal", "pre-line", "pre-wrap", "break-spaces", "pre", "nowrap"):
        if not any(record["whiteSpace"] == white_space for record in geometry):
            raise ValueError(f"{label}: fixture did not render white-space:{white_space}")
    for feature in (
        "spacing-positive-letter",
        "spacing-negative-letter",
        "spacing-positive-word",
        "spacing-negative-word",
        "indent-right-align",
        "indent-center-align",
        "overflow-wrap-anywhere",
        "word-break-all",
        "word-break-keep-all",
        "hyphens-auto",
        "nested-mark",
        "single-full-mark",
        "multiple-text-nodes",
        "first-line-style",
        "first-line-preserved-spaces",
        "soft-hyphen-at-end",
        "zwsp-breakpoint",
        "nbsp-run",
        "mixed-script-bidi",
        "overflow-wrap-break-word",
        "pre-line-leading-consecutive-breaks",
        "pre-wrap-leading-consecutive-breaks",
        "br-start",
        "br-end",
        "br-middle",
        "single-character-min-content",
        "only-whitespace-collapse",
        "only-whitespace-preserved",
    ):
        if not any(name.startswith(feature + "-") for name in names):
            raise ValueError(f"{label}: fixture case for {feature} is missing")
    if sum(name.startswith("mutation-") for name in names) != 4:
        raise ValueError(f"{label}: mutation geometry states are missing")
    if sum(name.startswith("random-") for name in names) != 12:
        raise ValueError(f"{label}: deterministic randomized geometry cases are missing")
    return value


def parse_counter_log(contents, label):
    records = []
    for line_number, line in enumerate(contents.splitlines(), 1):
        prefix_index = line.find(COUNTER_PREFIX)
        if prefix_index < 0:
            continue
        payload = line[prefix_index + len(COUNTER_PREFIX) :].strip()
        if payload.startswith("truncated=1"):
            raise ValueError(f"{label}: intrinsic width counter output was truncated")
        match = COUNTER_PATTERN.fullmatch(payload)
        if not match:
            raise ValueError(f"{label}: malformed intrinsic width counter on line {line_number}")
        records.append(
            {
                "event": int(match.group(1)),
                "path": match.group(2),
                "candidate": int(match.group(3)),
                "eligible": int(match.group(4)),
                "lines": int(match.group(5)),
                "startNs": int(match.group(6)),
                "durationNs": int(match.group(7)),
                "eligibilityNs": int(match.group(8)),
            }
        )
    return records


def verify_counter_hits(records, label):
    hits = [
        record
        for record in records
        if record["path"] == "cached-text" and record["candidate"] == 1 and record["eligible"] == 1
    ]
    if not hits:
        raise ValueError(f"{label}: no verified cached-text intrinsic width hits were recorded")
    return len(hits)


def verify_no_cached_hits(records, label):
    hits = [record for record in records if record["path"] == "cached-text"]
    if hits:
        raise ValueError(f"{label}: disabled control unexpectedly used the cached-text path")


def fixture_fonts(root, extra_paths):
    fonts = [
        {
            "family": "FixtureVariable",
            "data": base64.b64encode((root / "resources/fonts/RobotoFlex.ttf").read_bytes()).decode(),
            "descriptors": {"weight": "100 1000"},
        },
        {
            "family": "FixtureRegularOnly",
            "data": base64.b64encode(
                (
                    root
                    / (
                        "third_party/wpe/source/wpewebkit-2.53.92-engine-experiments/Source/ThirdParty/pdfjs/web/"
                        "standard_fonts/LiberationSans-Regular.ttf"
                    )
                ).read_bytes()
            ).decode(),
            "descriptors": {},
        },
    ]
    for index, path in enumerate(extra_paths):
        fonts.append(
            {
                "family": f"FixtureExtra{index}",
                "data": base64.b64encode(Path(path).read_bytes()).decode(),
                "descriptors": {},
            }
        )
    return fonts


def environment_for(mode):
    environment = os.environ.copy()
    for name in list(environment):
        if name.startswith(("JSC_", "WEBKIT_", "EDEN_WPE_", "SYSPROF_", "LLVM_PROFILE")):
            environment.pop(name)
    environment["WEBKIT_EDEN_INTRINSIC_WIDTH"] = "0" if mode == "off" else "1"
    if mode == "on":
        environment["WEBKIT_EDEN_VERIFY_INTRINSIC_WIDTH"] = "1"
    environment["WEBKIT_EDEN_PATCH_COUNTERS"] = "1"
    environment["WEBKIT_SKIA_CANVAS_REPLAY_THREADS"] = "2"
    return environment


def read_result(path, label):
    try:
        value = json.loads(path.read_text())
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValueError(f"{label}: result JSON could not be read: {error}") from error
    return validate_result(value, label)


def compare_runs(baseline, enabled, baseline_label, enabled_label):
    compare_exact(
        baseline["measurements"]["geometry"],
        enabled["measurements"]["geometry"],
        f"{baseline_label} vs {enabled_label}.geometry",
    )
    compare_exact(baseline["viewport"], enabled["viewport"], f"{baseline_label} vs {enabled_label}.viewport")


def run_validation(build, output, font_files=(), runner=subprocess.run, timeout_seconds=1830):
    root = Path(__file__).resolve().parents[3]
    build = Path(build).resolve()
    output = Path(output)
    output.mkdir(parents=True, exist_ok=False)
    summary = {"passed": False, "scales": [], "runs": [], "comparison": "exact geometry and viewport equality"}
    try:
        if not (build / "eden-wpe-bench").is_file():
            raise FileNotFoundError(f"Missing benchmark binary in {build}")
        fonts = fixture_fonts(root, font_files)
        fixture = (
            "globalThis.edenFixtureFonts = "
            + json.dumps(fonts)
            + ";\n"
            + (root / "tests/wpe-intrinsic-width.js").read_text()
        )
        with tempfile.TemporaryDirectory(prefix="eden-intrinsic-width-validation-") as temporary:
            stage = Path(temporary)
            probe = stage / "probe.js"
            probe.write_text(fixture)
            identity = None
            for scale in (1, 2):
                paired = {}
                for mode in ("off", "on"):
                    label = f"intrinsic-{mode}-scale-{scale}"
                    staged_result = stage / f"{label}.json"
                    saved_result = output / f"{label}.json"
                    saved_log = output / f"{label}.log"
                    environment = environment_for(mode)
                    environment["EDEN_WPE_PREWARM"] = "0"
                    profile_directory = stage / label
                    profile_directory.mkdir()
                    environment["LLVM_PROFILE_FILE"] = str(profile_directory / "%p-%m.profraw")
                    command = [
                        sys.executable,
                        str(root / "scripts/test_wpe.py"),
                        "--build",
                        str(build),
                        "--scale",
                        str(scale),
                        "--benchmark",
                        "--",
                        "--output",
                        str(staged_result),
                        "--label",
                        label,
                        "--probe-script",
                        str(probe),
                        "--training-directory",
                        str(profile_directory),
                    ]
                    invocation_error = None
                    return_code = None
                    with saved_log.open("w") as log:
                        try:
                            completed = runner(
                                command,
                                env=environment,
                                stdout=log,
                                stderr=subprocess.STDOUT,
                                check=False,
                                timeout=timeout_seconds,
                            )
                            return_code = completed.returncode
                        except Exception as error:
                            invocation_error = error
                    if staged_result.exists():
                        shutil.copyfile(staged_result, saved_result)
                    run_record = {
                        "label": label,
                        "scale": scale,
                        "mode": mode,
                        "returnCode": return_code,
                        "log": saved_log.name,
                        "result": saved_result.name if saved_result.exists() else None,
                    }
                    summary["runs"].append(run_record)
                    if invocation_error is not None:
                        run_record["error"] = f"{type(invocation_error).__name__}: {invocation_error}"
                        raise RuntimeError(
                            f"{label}: benchmark invocation failed: {invocation_error}"
                        ) from invocation_error
                    if return_code != 0:
                        raise RuntimeError(f"{label}: benchmark exited with status {return_code}; see {saved_log}")
                    if not saved_result.is_file() or not saved_result.stat().st_size:
                        raise ValueError(f"{label}: benchmark produced no result JSON; see {saved_log}")
                    value = read_result(saved_result, label)
                    runtime = value.get("runtime")
                    if not isinstance(runtime, dict) or not runtime.get("library"):
                        raise ValueError(f"{label}: runtime library identity missing")
                    library = Path(runtime["library"]).resolve(strict=True)
                    with library.open("rb") as stream:
                        current_identity = {"library": str(library), "sha256": hashlib.file_digest(stream, "sha256").hexdigest()}
                    if identity is None:
                        identity = current_identity
                        summary["runtime"] = identity
                    else:
                        compare_exact(identity, current_identity, "runtime identity")
                    counter_text = saved_log.read_text(errors="replace")
                    counter_records = parse_counter_log(counter_text, label)
                    if mode == "on":
                        hit_count = verify_counter_hits(counter_records, label)
                    else:
                        verify_no_cached_hits(counter_records, label)
                        hit_count = 0
                    run_record["counterRecords"] = len(counter_records)
                    run_record["verifiedHits"] = hit_count
                    paired[mode] = value
                compare_runs(
                    paired["off"],
                    paired["on"],
                    f"intrinsic-off-scale-{scale}",
                    f"intrinsic-on-scale-{scale}",
                )
                summary["scales"].append(
                    {
                        "scale": scale,
                        "cases": EXPECTED_GEOMETRY_CASES,
                        "exactGeometryMatch": True,
                        "enabledVerifiedHits": next(
                            record["verifiedHits"]
                            for record in summary["runs"]
                            if record["scale"] == scale and record["mode"] == "on"
                        ),
                    }
                )
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
    parser.add_argument("--font-file", type=Path, action="append", default=[])
    parser.add_argument("--timeout-seconds", type=int, default=1830)
    args = parser.parse_args()
    if not 1 <= args.timeout_seconds <= 14400:
        parser.error("Timeout must be between 1 and 14400 seconds")
    try:
        summary = run_validation(args.build, args.output, args.font_file, timeout_seconds=args.timeout_seconds)
    except Exception as error:
        raise SystemExit(str(error)) from error
    print(json.dumps(summary, indent=2), flush=True)


if __name__ == "__main__":
    main()
