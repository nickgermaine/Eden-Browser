import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests/wpe_webrtc_source_identity.js"
RUNNER = ROOT / "scripts/test_wpe.py"
INITIAL_CASES = {
    "trackless-sendonly": ("sendonly", False),
    "trackless-sendrecv": ("sendrecv", False),
    "canvas-sendonly": ("sendonly", True),
    "canvas-sendrecv": ("sendrecv", True),
    "receiveonly": ("recvonly", False),
}
STABILITY_STAGES = (
    "initial",
    "repeated",
    "codec-preferences",
    "sender-parameters",
    "replace-track",
    "renegotiated",
)
NEGOTIATED_STAGES = ("initial", "replace-track")
SENDER_SSRC_STAGES = ("before-offer", "initial-negotiation", "before-parameter-update", "parameter-update", "replace-track", "final")


def clean_environment():
    environment = os.environ.copy()
    for name in list(environment):
        if name.startswith(("JSC_", "WEBKIT_", "EDEN_WPE_", "SYSPROF_", "LLVM_PROFILE")):
            environment.pop(name)
    environment["EDEN_WPE_PREWARM"] = "0"
    environment["GST_DEBUG"] = "0"
    return environment


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def validate_safe_payload(value):
    forbidden_keys = ("candidate", "credential", "ice-pwd", "ice-ufrag", "password", "sdp", "usernamefragment")
    forbidden_text = re.compile(r"(?:^|\n)(?:v=0|a=candidate:|a=ice-pwd:|a=ice-ufrag:)|candidate:[^\s]+", re.IGNORECASE)

    def walk(item, location):
        if isinstance(item, dict):
            for key, child in item.items():
                lowered = str(key).lower()
                require(not any(token in lowered for token in forbidden_keys), f"Unsafe result field at {location}.{key}")
                walk(child, f"{location}.{key}")
        elif isinstance(item, list):
            for index, child in enumerate(item):
                walk(child, f"{location}[{index}]")
        elif isinstance(item, str):
            require(not forbidden_text.search(item), f"Unsafe result text at {location}")

    walk(value, "result")


def configured_prefix(build):
    cache = build / "CMakeCache.txt"
    if not cache.is_file():
        return None
    match = re.search(r"^EDEN_WPE_PREFIX:PATH=(.*)$", cache.read_text(), re.MULTILINE)
    return Path(match[1]).resolve() if match and match[1] else None


def require_configured_runtime(build, result):
    prefix = configured_prefix(build)
    loaded = (result.get("runtime") or {}).get("library")
    require(
        prefix is None or (isinstance(loaded, str) and Path(loaded).resolve().is_relative_to(prefix / "lib")),
        f"eden-wpe-bench loaded {loaded} instead of the configured runtime {prefix}; rebuild the eden-wpe-bench target",
    )


def sanitized_result(result):
    return {
        key: result[key]
        for key in ("probe", "measurements", "viewport", "visibility", "runtime", "webkitVersion")
        if key in result
    }


def validate_sections(record, expected_directions, label, allow_retained_sources=False):
    sections = record.get("sections")
    require(isinstance(sections, list) and len(sections) == len(expected_directions), f"{label}: video section count changed")
    primary_by_mid = {}
    used_source_ids = set()
    for index, section in enumerate(sections):
        require(isinstance(section, dict), f"{label}: video section {index} is malformed")
        mid = section.get("mid")
        require(isinstance(mid, str) and mid, f"{label}: video section {index} has no MID")
        require(section.get("direction") == expected_directions[index], f"{label}: video direction changed at index {index}")
        source_ids = section.get("sourceIds")
        fid_groups = section.get("fidGroups")
        codecs = section.get("codecs")
        require(isinstance(source_ids, list) and all(isinstance(item, str) and item.isdigit() for item in source_ids), f"{label}: source IDs are malformed")
        require(isinstance(fid_groups, list), f"{label}: FID groups are malformed")
        require(isinstance(codecs, list) and all(isinstance(item, dict) for item in codecs), f"{label}: codec metadata is malformed")
        if expected_directions[index] not in ("sendonly", "sendrecv") and (not allow_retained_sources or not source_ids):
            require(source_ids == [], f"{label}: receive-only video advertises source IDs")
            require(fid_groups == [], f"{label}: receive-only video advertises an FID group")
            continue
        require(len(fid_groups) == 1, f"{label}: sending video is missing one FID group at MID {mid}")
        fid = fid_groups[0]
        primary = fid.get("primary")
        retransmission = fid.get("retransmission")
        require(isinstance(primary, str) and primary.isdigit(), f"{label}: FID primary ID is malformed at MID {mid}")
        require(isinstance(retransmission, str) and retransmission.isdigit(), f"{label}: FID retransmission ID is malformed at MID {mid}")
        require(primary != retransmission, f"{label}: FID source IDs are equal at MID {mid}")
        require(primary in source_ids and retransmission in source_ids, f"{label}: FID source IDs are not both advertised at MID {mid}")
        rtx_codecs = [item for item in codecs if item.get("name", "").lower() == "rtx"]
        require(rtx_codecs, f"{label}: RTX codec metadata is missing at MID {mid}")
        primary_payloads = {item.get("payload") for item in codecs if item.get("name", "").lower() != "rtx"}
        require(any(item.get("apt") in primary_payloads for item in rtx_codecs), f"{label}: RTX codec mapping is missing at MID {mid}")
        require(mid not in primary_by_mid, f"{label}: video MID is duplicated")
        primary_by_mid[mid] = primary
        for source_id in source_ids:
            require(source_id not in used_source_ids, f"{label}: source ID is reused across senders")
            used_source_ids.add(source_id)
    return primary_by_mid


def validate_initial_cases(measurements):
    records = measurements.get("initialCases")
    require(isinstance(records, list) and len(records) == len(INITIAL_CASES), "Initial offer matrix is incomplete")
    results = {}
    for record in records:
        require(isinstance(record, dict), "Initial offer case is malformed")
        name = record.get("name")
        require(name in INITIAL_CASES and name not in results, "Initial offer case name is missing or duplicated")
        direction, attached = INITIAL_CASES[name]
        require(record.get("direction") == direction and record.get("attached") is attached, f"{name}: case setup changed")
        offers = record.get("offers")
        require(isinstance(offers, list) and len(offers) == 2, f"{name}: first and repeated offers are required")
        require(offers[0].get("stage") == "initial" and offers[1].get("stage") == "repeated", f"{name}: offer order changed")
        require(offers[0].get("applied") is True, f"{name}: the initial offer was not negotiated before the repeated offer")
        require(offers[1].get("applied") is False, f"{name}: the repeated offer was applied")
        first = validate_sections(offers[0], [direction], name + ".initial")
        repeated = validate_sections(offers[1], [direction], name + ".repeated")
        require(first.keys() == repeated.keys(), f"{name}: MID changed between offers")
        require(first == repeated, f"{name}: primary source ID changed between offers")
        results[name] = {"direction": direction, "attached": attached, "primaryIds": first}
    require(results.keys() == INITIAL_CASES.keys(), "An initial offer case is missing")
    return results


def validate_result(result):
    require(isinstance(result, dict) and result.get("probe") == "webrtc-source-identity", "Result is not the WebRTC source identity probe")
    require(result.get("visibility") == "visible", "Probe page was not visible")
    viewport = result.get("viewport")
    require(
        isinstance(viewport, list)
        and len(viewport) == 3
        and all(isinstance(value, (int, float)) and not isinstance(value, bool) and value > 0 for value in viewport),
        "Probe viewport is malformed",
    )
    measurements = result.get("measurements")
    require(isinstance(measurements, dict), "Probe measurements are missing")
    require(measurements.get("failures") == [], f"Probe reported a failure: {measurements.get('failures')}")
    initial_cases = validate_initial_cases(measurements)
    direction_offers = measurements.get("directionOffers")
    directions = ["sendonly", "recvonly", "sendrecv", "inactive", "sendonly"]
    require(isinstance(direction_offers, list) and len(direction_offers) == len(directions), "Direction transition coverage is missing")
    direction_primary = None
    for record, direction in zip(direction_offers, directions, strict=True):
        require(record.get("direction") == direction, "Direction transition order changed")
        require(record.get("applied") is True, f"Direction transition to {direction} was not negotiated")
        current = validate_sections(record, [direction], "direction-" + direction, allow_retained_sources=True)
        if current:
            if direction_primary is None:
                direction_primary = current
            else:
                require(current == direction_primary, "Source identity changed across direction transitions")
    offers = measurements.get("stabilityOffers")
    require(isinstance(offers, list) and len(offers) == len(STABILITY_STAGES), "Offer stability checkpoints are missing")
    baseline = None
    checkpoint_summary = []
    for record, expected_stage in zip(offers, STABILITY_STAGES, strict=True):
        require(record.get("stage") == expected_stage, f"Offer checkpoint order changed at {expected_stage}")
        require(record.get("applied") is (expected_stage in NEGOTIATED_STAGES), f"{expected_stage}: offer application state changed")
        expected_directions = ["sendonly"]
        current = validate_sections(record, expected_directions, expected_stage)
        if baseline is None:
            baseline = current
        else:
            require(baseline.keys() <= current.keys(), f"{expected_stage}: a sender MID disappeared")
            for mid, source_id in baseline.items():
                require(current[mid] == source_id, f"{expected_stage}: primary source ID changed at MID {mid}")
        require(current.keys() == baseline.keys(), f"{expected_stage}: sender MID count changed")
        checkpoint_summary.append({"stage": expected_stage, "senders": current})
    require(measurements.get("negotiatedStages") == list(NEGOTIATED_STAGES), "Negotiation did not apply the initial and latest selected offers")
    require(measurements.get("selectedOfferStage") == "replace-track", "Negotiation did not use the latest selected offer")
    validate_sections(measurements.get("multipleSenderOffer", {}), ["sendonly", "sendrecv"], "multiple-sender-identities")
    exposure = measurements.get("senderParameterExposure")
    require(isinstance(exposure, dict) and exposure.get("stable") is True, "Sender SSRC parameter exposure changed")
    exposure_stages = exposure.get("stages")
    require(isinstance(exposure_stages, list) and [item.get("stage") for item in exposure_stages] == list(SENDER_SSRC_STAGES), "Sender SSRC parameter checkpoints are incomplete")
    require(all(item.get("present") is exposure.get("present") for item in exposure_stages), "Sender SSRC parameter exposure changed between checkpoints")
    require(not exposure_stages[0].get("value"), "A sender SSRC parameter was exposed before negotiation")
    transmission = measurements.get("transmission")
    require(isinstance(transmission, dict), "Paired peer transmission is missing")
    require(transmission.get("selectedOfferStage") == "replace-track", "Transmission used a different offer than the latest checkpoint")
    senders = transmission.get("senders")
    require(isinstance(senders, list) and len(senders) == 1, "Expected paired RTP statistics for the canvas sender")
    final_primary = checkpoint_summary[-1]["senders"]
    seen_mids = set()
    for sender in senders:
        require(isinstance(sender, dict), "Paired sender statistics are malformed")
        mid = sender.get("mid")
        require(mid in final_primary and mid not in seen_mids, "Paired sender MID is missing or duplicated")
        seen_mids.add(mid)
        primary = final_primary[mid]
        require(sender.get("advertisedPrimary") == primary, f"Advertised primary changed at MID {mid}")
        require(sender.get("outboundSsrc") == primary, f"Outbound RTP SSRC differs from the offer at MID {mid}")
        require(sender.get("inboundSsrc") == primary, f"Inbound RTP SSRC differs from the offer at MID {mid}")
        require(sender.get("framesDecoded", 0) > 0, f"No video frames were decoded at MID {mid}")
        require(sender.get("geometry") == {"width": 320, "height": 180}, f"Received video geometry changed at MID {mid}")
    require(seen_mids == final_primary.keys(), "Paired sender coverage is incomplete")
    exposed = {str(item["value"]) for item in exposure_stages if item.get("value")}
    require(exposed <= set(final_primary.values()), "A sender SSRC parameter differs from the advertised primary")
    detached = measurements.get("detachedPlayback")
    require(isinstance(detached, dict), "Detached muted remote playback is missing")
    require(detached.get("connected") is False, "Detached remote video was attached to the document")
    require(detached.get("muted") is True, "Detached remote video was not muted")
    require(detached.get("paused") is False, "Detached muted remote video is paused")
    require(isinstance(detached.get("readyState"), int) and detached["readyState"] >= 2, "Detached muted remote video has no current data")
    require(detached.get("geometry") == {"width": 320, "height": 180}, "Detached muted remote video geometry changed")
    return {
        "passed": True,
        "initialCases": initial_cases,
        "checkpoints": checkpoint_summary,
        "senderParameterExposure": exposure,
        "transmission": senders,
        "detachedPlayback": detached,
    }


def run_in_directory(build, timeout_seconds, stage):
    build = build.resolve()
    require((build / "eden-wpe-bench").is_file(), f"Missing eden-wpe-bench in {build}")
    require(FIXTURE.is_file(), "WebRTC source identity fixture is missing")
    probe = stage / "probe.js"
    result_path = stage / "result.json"
    summary_path = stage / "summary.json"
    probe.write_text(FIXTURE.read_text())
    command = [
        sys.executable,
        str(RUNNER),
        "--build",
        str(build),
        "--timeout-seconds",
        str(timeout_seconds),
        "--writable-path",
        str(stage),
        "--benchmark",
        "--",
        "--output",
        str(result_path),
        "--label",
        "webrtc-source-identity",
        "--probe-script",
        str(probe),
    ]
    summary = {"passed": False}
    try:
        completed = subprocess.run(
            command,
            env=clean_environment(),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=timeout_seconds + 45,
            check=False,
        )
        require(result_path.is_file(), f"WPE probe exited with status {completed.returncode} without a result")
        raw_result = json.loads(result_path.read_text())
        validate_safe_payload(raw_result)
        result = sanitized_result(raw_result)
        require_configured_runtime(build, result)
        result_path.write_text(json.dumps(result, indent=2) + "\n")
        summary.update(validate_result(result))
        require(completed.returncode == 0, f"WPE probe exited with status {completed.returncode}")
        summary["passed"] = True
        return summary
    except Exception as error:
        summary["error"] = f"{type(error).__name__}: {error}"
        raise
    finally:
        summary_path.write_text(json.dumps(summary, indent=2) + "\n")


def run(build, timeout_seconds, output_directory=None):
    if output_directory:
        stage = output_directory.resolve()
        stage.mkdir(parents=True, exist_ok=False)
        return run_in_directory(build, timeout_seconds, stage)
    with tempfile.TemporaryDirectory(prefix="eden-webrtc-source-identity-") as temporary:
        return run_in_directory(build, timeout_seconds, Path(temporary))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path, default=ROOT / "cmake-build-release")
    parser.add_argument("--timeout-seconds", type=int, default=180)
    parser.add_argument("--output-directory", type=Path)
    arguments = parser.parse_args()
    require(1 <= arguments.timeout_seconds <= 600, "Timeout must be between 1 and 600 seconds")
    try:
        print(json.dumps(run(arguments.build, arguments.timeout_seconds, arguments.output_directory), indent=2))
    except Exception as error:
        raise SystemExit(f"WebRTC source identity regression failed: {type(error).__name__}: {error}") from error


if __name__ == "__main__":
    main()
