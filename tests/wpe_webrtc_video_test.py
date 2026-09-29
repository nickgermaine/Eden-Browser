import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests/wpe_webrtc_video.js"
RUNNER = ROOT / "scripts/test_wpe.py"
MAXIMUM_CAPPED_KBPS = 405
MINIMUM_CAPPED_KBPS = 30
MAXIMUM_CAPPED_KEY_FRAMES = 2
MAXIMUM_UNMUTED_KEY_FRAMES = 4
SIMULCAST_BUDGET_FACTOR = 1.35
MINIMUM_DECODED_FRAMES = 30
MINIMUM_TRANSFORMED_FRAMES = 60
MINIMUM_COLOR_SAMPLES = 20
MINIMUM_COLOR_AGREEMENT = 0.9
CAPPED_CODECS = ("cappedVP8", "cappedVP9", "cappedAV1", "cappedH264")
SCALABILITY_CASES = tuple(f"scalability{codec}{mode}" for mode in ("L1T2", "L1T3") for codec in ("VP8", "VP9", "AV1"))


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


def find_probe(value):
    if isinstance(value, dict):
        if value.get("probe") == "webrtc-video":
            return value
        for child in value.values():
            found = find_probe(child)
            if found:
                return found
    if isinstance(value, list):
        for child in value:
            found = find_probe(child)
            if found:
                return found
    return None


def validate_capabilities(capabilities):
    send = capabilities.get("send") or []
    receive = capabilities.get("receive") or []
    require("video/VP8" in send, "VP8 is not offered for sending")
    require("video/VP9 profile-id=0" in send, "VP9 profile 0 is not offered for sending")
    require("video/VP9 profile-id=2" in send, "VP9 profile 2 is not offered for sending")
    require(any(item.startswith("video/AV1") for item in send), "AV1 is not offered for sending")
    h264 = [item for item in send if item.startswith("video/H264")]
    require(any("profile-level-id=42e01f" in item and "packetization-mode=1" in item for item in h264), "Constrained baseline H.264 is not offered for sending")
    require(not any("packetization-mode=0" in item for item in h264), "H.264 packetization mode 0 is offered but the encoders cannot bound NAL sizes")
    require(not any("profile-level-id=f4" in item for item in h264), "H.264 High 4:4:4 is offered for sending")
    for codec in ("video/VP8", "video/VP9 profile-id=0", "video/AV1"):
        require(any(item.startswith(codec) for item in receive), f"{codec} is not offered for receiving")
    require(any(item.startswith("video/H264") for item in receive), "H.264 is not offered for receiving")


def validate_simulcast(simulcast):
    require(simulcast.get("connection") == "connected", "The simulcast call did not connect")
    layers = {layer.get("rid"): layer for layer in simulcast.get("layers") or []}
    require(set(layers) == {"q", "h", "f"}, f"Simulcast layers are missing: {sorted(layers)}")
    require(len({layer.get("ssrc") for layer in layers.values()}) == 3, "Simulcast layers share an SSRC")
    for rid, layer in layers.items():
        require(layer.get("framesEncoded", 0) > 0, f"Simulcast layer {rid} encoded no frames")
    require(layers["f"]["frameWidth"] > layers["h"]["frameWidth"] > layers["q"]["frameWidth"] > 0, "Simulcast layer resolutions are not ordered")
    require(layers["q"]["frameWidth"] <= 320, f"The lowest simulcast layer is {layers['q']['frameWidth']} wide")
    targets = sum(layer.get("targetBitrate", 0) for layer in layers.values()) / 1000
    require(targets > 0, "Simulcast layers report no target bitrate")
    require(simulcast.get("kbps", 0) <= targets * SIMULCAST_BUDGET_FACTOR, f"Simulcast sent {simulcast.get('kbps')} kbps against {targets:.0f} kbps of targets")


def validate_capped(name, result):
    require(not result.get("unsupported"), f"{name}: the codec is unavailable")
    require(result.get("connection") == "connected", f"{name}: the call did not connect, it ended {result.get('connection')}")
    capped = result.get("cappedKbps", 0)
    require(MINIMUM_CAPPED_KBPS <= capped <= MAXIMUM_CAPPED_KBPS, f"{name}: sent {capped} kbps under a 300 kbps cap")
    for label, row in (("sender", result.get("resizedOutbound") or {}), ("receiver", result.get("resized") or {})):
        width, height = row.get("frameWidth", 0), row.get("frameHeight", 0)
        require(0 < width <= 640 and 0 < height <= 360 and abs(width / height - 16 / 9) < 0.05, f"{name}: the {label} did not follow the 640x360 source: {row}")
    require(result.get("framesAfterResize", 0) >= 20, f"{name}: only {result.get('framesAfterResize')} frames were decoded after the resize")
    require((result.get("mutedFrames") or 0) >= 2, f"{name}: a disabled track stopped sending frames")
    require((result.get("unmutedFrames") or 0) >= 10, f"{name}: video did not resume after the track was enabled again")
    capped_keyframes = result.get("cappedKeyFrames")
    require(capped_keyframes is not None and capped_keyframes <= MAXIMUM_CAPPED_KEY_FRAMES, f"{name}: {capped_keyframes} keyframes in 4 seconds at a steady 300 kbps cap")
    unmuted_keyframes = result.get("unmutedKeyFrames")
    require(unmuted_keyframes is not None and unmuted_keyframes <= MAXIMUM_UNMUTED_KEY_FRAMES, f"{name}: {unmuted_keyframes} keyframes in the 2 seconds after the track was enabled again")


def validate_decoded(name, result, minimum):
    require(not result.get("unsupported"), f"{name}: the codec is unavailable")
    require(result.get("connection") == "connected", f"{name}: the call did not connect, it ended {result.get('connection')}")
    inbound = result.get("inbound") or []
    require(inbound and all(row.get("framesDecoded", 0) >= minimum for row in inbound), f"{name}: decoded too few frames: {inbound}")


def validate_scalability(name, result, mode):
    validate_decoded(name, result, MINIMUM_DECODED_FRAMES)
    require(result.get("parameters") == [mode], f"{name}: the sender reports {result.get('parameters')} instead of {mode}")
    for row in result.get("outbound") or []:
        require(row.get("scalabilityMode") in (None, mode), f"{name}: stats report {row.get('scalabilityMode')}")


def validate_transform(name, result):
    validate_decoded(name, result, MINIMUM_TRANSFORMED_FRAMES)
    transformed = result.get("transformed") or {}
    require(transformed.get("sender", 0) > 0 and transformed.get("receiver", 0) > 0, f"{name}: the script transform saw no frames: {transformed}")


def validate_concurrent(result):
    require(result.get("connection") == "connected", f"concurrentSenders: the call did not connect, it ended {result.get('connection')}")
    for color, summary in (result.get("summary") or {}).items():
        samples = summary.get("samples", 0)
        require(samples >= MINIMUM_COLOR_SAMPLES, f"concurrentSenders: only {samples} {color} samples")
        require(summary.get("matching", 0) >= samples * MINIMUM_COLOR_AGREEMENT, f"concurrentSenders: {color} frames arrived with the wrong content: {summary}")
    require(set(result.get("summary") or {}) == {"red", "blue"}, "concurrentSenders: a sender is missing")


def validate_result(result):
    require(result.get("visibility") == "visible", "Probe page was not visible")
    measurements = result.get("measurements")
    require(isinstance(measurements, dict), "Probe measurements are missing")
    require(measurements.get("failures") == [], f"Probe reported a failure: {measurements.get('failures')}")
    validate_capabilities(measurements.get("capabilities") or {})
    validate_simulcast(measurements.get("simulcast") or {})
    for name in CAPPED_CODECS:
        validate_capped(name, measurements.get(name) or {})
    for name in ("decodeVP9Profile2", "decodeAV1"):
        validate_decoded(name, measurements.get(name) or {}, MINIMUM_DECODED_FRAMES)
    for name in SCALABILITY_CASES:
        validate_scalability(name, measurements.get(name) or {}, name[-4:])
    for name in ("transformH264", "transformVP8"):
        validate_transform(name, measurements.get(name) or {})
    validate_concurrent(measurements.get("concurrentSenders") or {})
    return {
        "passed": True,
        "simulcast": measurements["simulcast"],
        "capped": {name: {"cappedKbps": measurements[name]["cappedKbps"], "uncappedKbps": measurements[name]["uncappedKbps"]} for name in CAPPED_CODECS},
        "concurrentSenders": measurements["concurrentSenders"]["summary"],
    }


def run_in_directory(build, timeout_seconds, stage):
    build = build.resolve()
    require((build / "eden-wpe-bench").is_file(), f"Missing eden-wpe-bench in {build}")
    require(FIXTURE.is_file(), "WebRTC video fixture is missing")
    probe = stage / "probe.js"
    result_path = stage / "result.json"
    summary_path = stage / "summary.json"
    probe.write_text(FIXTURE.read_text())
    command = [
        sys.executable, str(RUNNER), "--build", str(build), "--timeout-seconds", str(timeout_seconds),
        "--writable-path", str(stage), "--benchmark", "--",
        "--output", str(result_path), "--label", "webrtc-video", "--probe-script", str(probe),
    ]
    summary = {"passed": False}
    try:
        completed = subprocess.run(command, env=clean_environment(), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=timeout_seconds + 45, check=False)
        (stage / "runner.log").write_text(completed.stdout)
        criticals = sorted({re.sub(r"\d{2}:\d{2}:\d{2}\.\d+", "", line).strip() for line in completed.stdout.splitlines() if "-CRITICAL **" in line})
        require(not criticals, "WebKit logged criticals: " + " | ".join(criticals[:5]))
        require(result_path.is_file(), f"WPE probe exited with status {completed.returncode} without a result")
        raw_result = json.loads(result_path.read_text())
        require_configured_runtime(build, raw_result)
        probe_result = find_probe(raw_result)
        require(probe_result is not None, "The result does not contain the WebRTC video probe")
        summary.update(validate_result(probe_result))
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
    with tempfile.TemporaryDirectory(prefix="eden-webrtc-video-") as temporary:
        return run_in_directory(build, timeout_seconds, Path(temporary))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path, default=ROOT / "cmake-build-release")
    parser.add_argument("--timeout-seconds", type=int, default=360)
    parser.add_argument("--output-directory", type=Path)
    arguments = parser.parse_args()
    require(1 <= arguments.timeout_seconds <= 900, "Timeout must be between 1 and 900 seconds")
    try:
        print(json.dumps(run(arguments.build, arguments.timeout_seconds, arguments.output_directory), indent=2))
    except Exception as error:
        raise SystemExit(f"WebRTC video regression failed: {type(error).__name__}: {error}") from error


if __name__ == "__main__":
    main()
