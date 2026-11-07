import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

from validate_intrinsic_width import compare_exact, environment_for


COUNTER_PREFIX = "Eden author rule memo:"
COUNTER_PATTERN = re.compile(r"lookups (\d+) hits (\d+) misses (\d+) bypasses (\d+) reused-rules (\d+)")
EXPECTED_ASSERTIONS = 89


def validate_result(value, label):
    if not isinstance(value, dict) or value.get("probe") != "author-rule-memo":
        raise ValueError(f"{label}: missing author memo result")
    if value.get("visibility") != "visible":
        raise ValueError(f"{label}: page was not visible")
    viewport = value.get("viewport")
    if not isinstance(viewport, list) or len(viewport) != 3 or any(
        isinstance(number, bool) or not isinstance(number, (int, float))
        or not math.isfinite(number) or number <= 0 for number in viewport
    ):
        raise ValueError(f"{label}: malformed viewport")
    result = value.get("measurements")
    if not isinstance(result, dict) or result.get("failures") != []:
        raise ValueError(f"{label}: assertion failures: {result}")
    records = result.get("records")
    if not isinstance(records, list) or any(not isinstance(record, dict) for record in records):
        raise ValueError(f"{label}: malformed assertion records")
    assertions = [record for record in records if not record.get("skipped")]
    if result.get("assertions") != EXPECTED_ASSERTIONS or len(assertions) != EXPECTED_ASSERTIONS:
        raise ValueError(f"{label}: expected {EXPECTED_ASSERTIONS} assertions")
    names = {record.get("name") for record in assertions}
    if len(names) != EXPECTED_ASSERTIONS or any(not isinstance(name, str) or not name for name in names):
        raise ValueError(f"{label}: missing or duplicated assertion names")
    for record in assertions:
        if record.get("passed") is not True or "actual" not in record or "expected" not in record:
            raise ValueError(f"{label}: failed assertion {record}")
        compare_exact(record["actual"], record["expected"], f"{label}.{record['name']}")
    required = {"focus state selector", "quirks mode document", "CSSOM inserted rule invalidates rule cache"}
    required.update(f"native media viewport {stage}" for stage in ("initial", "narrow", "restored"))
    if not required <= names:
        raise ValueError(f"{label}: required focus, mutation or viewport coverage missing")
    skipped = [record.get("name") for record in records if record.get("skipped")]
    if skipped != ["dynamic media viewport toggle"]:
        raise ValueError(f"{label}: unexpected skips {skipped}")
    runtime = value.get("runtime")
    if not isinstance(runtime, dict) or not runtime.get("library"):
        raise ValueError(f"{label}: runtime library identity missing")
    return value


def validate_counters(contents, enabled, label):
    records = []
    for line in contents.splitlines():
        position = line.find(COUNTER_PREFIX)
        if position < 0:
            continue
        match = COUNTER_PATTERN.fullmatch(line[position + len(COUNTER_PREFIX):].strip())
        if not match:
            raise ValueError(f"{label}: malformed or failed memo counter: {line}")
        lookup, hit, miss, bypass, reused = map(int, match.groups())
        if lookup != hit + miss + bypass or reused < hit:
            raise ValueError(f"{label}: inconsistent memo counter")
        records.append({"lookups": lookup, "hits": hit, "misses": miss, "bypasses": bypass, "reusedRules": reused})
    hits = max((record["hits"] for record in records), default=0)
    if enabled and hits == 0:
        raise ValueError(f"{label}: enabled run recorded no memo hits")
    if not enabled and hits:
        raise ValueError(f"{label}: disabled run recorded memo hits")
    return records


def library_digest(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def run_validation(build, output, runner=subprocess.run, timeout_seconds=180):
    root = Path(__file__).resolve().parents[3]
    build = Path(build).resolve()
    output = Path(output)
    output.mkdir(parents=True, exist_ok=False)
    summary = {"passed": False, "runs": [], "scales": [], "viewportCoverage": "native iframe resizing replaces the WTR-only top-level resize check"}
    try:
        if not (build / "eden-wpe-bench").is_file():
            raise FileNotFoundError(f"Missing benchmark binary in {build}")
        fixture = "globalThis.edenAuthorMemoMarkup = " + json.dumps(
            (root / "scripts/wpe/adopted/author_rule_memo_test.html").read_text()
        ) + ";\n"
        fixture += (root / "scripts/wpe/adopted/author_rule_memo_test.js").read_text() + "\n"
        fixture += (root / "tests/wpe-author-rule-memo.js").read_text()
        (output / "probe.js").write_text(fixture)
        with tempfile.TemporaryDirectory(prefix="eden-author-memo-validation-") as temporary:
            stage = Path(temporary)
            probe = stage / "probe.js"
            probe.write_text(fixture)
            identity = None
            for scale in (1, 2):
                paired = {}
                for mode in ("off", "on"):
                    label = f"author-memo-{mode}-scale-{scale}"
                    staged_result = stage / f"{label}.json"
                    saved_result = output / staged_result.name
                    saved_log = output / f"{label}.log"
                    environment = environment_for("off")
                    environment["EDEN_WPE_PREWARM"] = "0"
                    environment.pop("WEBKIT_EDEN_INTRINSIC_WIDTH", None)
                    environment["WEBKIT_EDEN_AUTHOR_RULE_MEMO"] = "1" if mode == "on" else "0"
                    if mode == "on":
                        environment["WEBKIT_EDEN_VERIFY_AUTHOR_RULE_MEMO"] = "1"
                    profile_directory = stage / label
                    profile_directory.mkdir()
                    environment["LLVM_PROFILE_FILE"] = str(profile_directory / "%p-%m.profraw")
                    command = [sys.executable, str(root / "scripts/test_wpe.py"), "--build", str(build),
                               "--scale", str(scale), "--timeout-seconds", str(timeout_seconds), "--benchmark", "--",
                               "--output", str(staged_result), "--label", label, "--probe-script", str(probe),
                               "--training-directory", str(profile_directory)]
                    run_record = {"label": label, "mode": mode, "scale": scale, "log": saved_log.name}
                    summary["runs"].append(run_record)
                    try:
                        with saved_log.open("w") as log:
                            completed = runner(command, env=environment, stdout=log, stderr=subprocess.STDOUT,
                                               check=False, timeout=timeout_seconds + 45)
                        run_record["returnCode"] = completed.returncode
                    finally:
                        if staged_result.exists():
                            shutil.copyfile(staged_result, saved_result)
                            run_record["result"] = saved_result.name
                    if completed.returncode:
                        raise RuntimeError(f"{label}: process exited {completed.returncode}; see {saved_log}")
                    value = validate_result(json.loads(saved_result.read_text()), label)
                    run_record["counters"] = validate_counters(saved_log.read_text(errors="replace"), mode == "on", label)
                    current_identity = {"library": value["runtime"]["library"], "sha256": library_digest(value["runtime"]["library"])}
                    if identity is None:
                        identity = current_identity
                        summary["runtime"] = identity
                    else:
                        compare_exact(identity, current_identity, "runtime identity")
                    paired[mode] = value
                compare_exact(paired["off"]["measurements"], paired["on"]["measurements"], f"scale {scale} assertions")
                compare_exact(paired["off"]["viewport"], paired["on"]["viewport"], f"scale {scale} viewport")
                summary["scales"].append({"scale": scale, "assertions": EXPECTED_ASSERTIONS, "exactMatch": True})
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
    arguments = parser.parse_args()
    try:
        result = run_validation(arguments.build, arguments.output)
    except Exception as error:
        raise SystemExit(str(error)) from error
    print(json.dumps(result, indent=2), flush=True)


if __name__ == "__main__":
    main()
