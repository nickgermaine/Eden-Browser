import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import wave

sys.path.insert(0, str(Path(__file__).resolve().parent))

from wpe_voice_processing_test import FAR_END_SPEECH, MICROPHONE, PIPEWIRE_CONFIG, WIREPLUMBER_CONFIG, node_names, require, start, stop, wait_for


ROOT = Path(__file__).resolve().parents[1]
COMPOSITOR_SOCKET = "eden-microphone"
SPEECH_RATE = 48000
SPEECH_GAIN_DB = -26.0
SPEECH_SECONDS = 150
PAUSE_SECONDS = 0.6
RAW_SECONDS = 6
PROCESSED_SECONDS = 20
SETTLED_SECONDS = 10
MAXIMUM_RAW_DIFFERENCE_DB = 3.0
MAXIMUM_PROCESSED_SHORTFALL_DB = 6.0
CHROME_TIMEOUT_SECONDS = 120

PAGE = """<!doctype html><meta charset="utf-8"><title>microphone-level</title><body><script>
window.microphoneLevel = null;
(async () => {
    const report = async result => {
        window.microphoneLevel = result;
        await fetch('/result', {method: 'POST', body: JSON.stringify(result)});
    };
    try {
        const delay = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));
        (await navigator.mediaDevices.getUserMedia({audio: true})).getTracks().forEach(track => track.stop());
        const device = (await navigator.mediaDevices.enumerateDevices()).find(item => item.kind === 'audioinput' && item.label.includes(%(microphone)s));
        if (!device)
            throw new Error('microphone missing');
        const measure = async (constraints, seconds, settledSeconds) => {
            const stream = await navigator.mediaDevices.getUserMedia({audio: {deviceId: {exact: device.deviceId}, ...constraints}});
            const track = stream.getAudioTracks()[0];
            const context = new AudioContext();
            await context.resume();
            const analyser = context.createAnalyser();
            analyser.fftSize = 2048;
            context.createMediaStreamSource(stream).connect(analyser);
            const samples = new Float32Array(analyser.fftSize);
            const levels = [];
            const started = performance.now();
            while (performance.now() - started < seconds * 1000) {
                await delay(40);
                analyser.getFloatTimeDomainData(samples);
                let power = 0;
                for (const sample of samples)
                    power += sample * sample;
                levels.push([performance.now() - started, 10 * Math.log10(Math.max(power / samples.length, 1e-12))]);
            }
            const settings = track.getSettings();
            track.stop();
            await context.close();
            const settled = levels.filter(([time]) => time >= (seconds - settledSeconds) * 1000).map(([, level]) => level).sort((a, b) => a - b);
            const quantile = fraction => Math.round(settled[Math.floor(fraction * (settled.length - 1))] * 10) / 10;
            const firstSecond = levels.filter(([time]) => time < 1000).map(([, level]) => level).sort((a, b) => a - b);
            return {
                p50: quantile(0.5),
                p90: quantile(0.9),
                p99: quantile(0.99),
                firstSecondP90: firstSecond.length ? Math.round(firstSecond[Math.floor(0.9 * (firstSecond.length - 1))] * 10) / 10 : null,
                windows: levels.length,
                settings: {echoCancellation: settings.echoCancellation, noiseSuppression: settings.noiseSuppression, autoGainControl: settings.autoGainControl},
            };
        };
        const raw = await measure({echoCancellation: false, noiseSuppression: false, autoGainControl: false}, %(raw)d, %(raw)d - 1);
        const processed = await measure({echoCancellation: true, noiseSuppression: true, autoGainControl: true}, %(processed)d, %(settled)d);
        await report({raw, processed, userAgent: navigator.userAgent});
    } catch (error) {
        await report({error: String(error && error.message || error)});
    }
})();
</script>
"""


def write_quiet_speech(stage):
    phrase = stage / "phrase.wav"
    subprocess.run(["espeak-ng", "-v", "en-us", "-s", "160", "-w", str(phrase), FAR_END_SPEECH], check=True)
    with wave.open(str(phrase), "rb") as source:
        rate = source.getframerate()
        frames = source.readframes(source.getnframes())
        width = source.getsampwidth()
        channels = source.getnchannels()
    require(width == 2 and channels == 1, "espeak-ng produced an unexpected format")
    values = [int.from_bytes(frames[index:index + 2], "little", signed=True) for index in range(0, len(frames), 2)]
    peak = max(1, max(abs(value) for value in values))
    gain = 10 ** (SPEECH_GAIN_DB / 20) * 32767 / peak
    pause = [0] * int(rate * PAUSE_SECONDS)
    loop = [int(value * gain) for value in values] + pause
    repeats = math.ceil(SPEECH_SECONDS * rate / len(loop))
    output = stage / "quiet-speech.wav"
    with wave.open(str(output), "wb") as target:
        target.setnchannels(1)
        target.setsampwidth(2)
        target.setframerate(rate)
        target.writeframes(b"".join(value.to_bytes(2, "little", signed=True) for value in loop) * repeats)
    return output


def source_volume():
    completed = subprocess.run(["pactl", "get-source-volume", "eden-test-microphone"], capture_output=True, text=True)
    match = re.search(r"(\d+)%", completed.stdout)
    return int(match[1]) if match else None


def reset_source_volume():
    subprocess.run(["pactl", "set-source-volume", "eden-test-microphone", "100%"], check=True)


class Results:
    def __init__(self):
        self.lock = threading.Condition()
        self.value = None

    def set(self, value):
        with self.lock:
            self.value = value
            self.lock.notify_all()

    def take(self, timeout):
        with self.lock:
            self.lock.wait_for(lambda: self.value is not None, timeout)
            value, self.value = self.value, None
            return value


def page_server(results):
    page = (PAGE % {"microphone": json.dumps(MICROPHONE), "raw": RAW_SECONDS, "processed": PROCESSED_SECONDS, "settled": SETTLED_SECONDS}).encode()

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *arguments):
            return

        def do_GET(self):
            if self.path == "/microphone-level.html":
                self.reply(200, "text/html", page)
            else:
                self.reply(404, "text/plain", b"")

        def do_POST(self):
            body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
            if self.path == "/result":
                results.set(json.loads(body))
            self.reply(200, "text/plain", b"")

        def reply(self, status, content_type, body):
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def run_chrome(stage, url, results):
    browser = shutil.which("google-chrome-stable") or shutil.which("chromium")
    if not browser:
        return {"skipped": "no Chrome or Chromium installed"}
    profile = Path(tempfile.mkdtemp(prefix="chrome-", dir=stage))
    process = start(stage, "chrome", [
        browser, "--headless=new", "--no-first-run", "--no-default-browser-check", f"--user-data-dir={profile}",
        "--password-store=basic", "--use-fake-ui-for-media-stream", "--autoplay-policy=no-user-gesture-required", url,
    ])
    try:
        value = results.take(CHROME_TIMEOUT_SECONDS)
        return value if value is not None else {"error": "Chrome produced no result"}
    finally:
        stop([process])
        shutil.rmtree(profile, ignore_errors=True)


def inside(arguments):
    runtime = Path(os.environ["XDG_RUNTIME_DIR"])
    stage = arguments.stage
    processes = []
    talker = None
    results = Results()
    server = page_server(results)
    url = f"http://127.0.0.1:{server.server_address[1]}/microphone-level.html"
    measurements = {}
    try:
        processes.append(start(stage, "mutter", [
            "mutter", "--headless", "--wayland", "--no-x11", "--virtual-monitor", "1280x800", "--wayland-display", COMPOSITOR_SOCKET,
        ]))
        wait_for(lambda: (runtime / COMPOSITOR_SOCKET).exists(), "Headless mutter did not start")
        processes.append(start(stage, "pipewire", ["pipewire"]))
        wait_for(lambda: (runtime / "pipewire-0").exists(), "PipeWire did not start")
        processes.append(start(stage, "wireplumber", ["wireplumber"]))
        processes.append(start(stage, "pipewire-pulse", ["pipewire-pulse"]))
        wait_for(lambda: (runtime / "pulse/native").exists(), "The PulseAudio server did not start")
        wait_for(lambda: {"eden-test-speaker", "eden-test-room", "eden-test-microphone"} <= node_names(), "The virtual audio devices were not created")
        subprocess.run(["pactl", "set-default-sink", "eden-test-speaker"], check=True)
        subprocess.run(["pactl", "set-default-source", "eden-test-microphone"], check=True)
        speech = write_quiet_speech(stage)
        talker = start(stage, "talker", [
            "gst-launch-1.0", "filesrc", f"location={speech}", "!", "wavparse", "!", "audioconvert", "!",
            "audioresample", "!", "pulsesink", "device=eden-test-room",
        ])
        time.sleep(1)
        reset_source_volume()
        environment = dict(os.environ)
        environment["EDEN_TEST_MICROPHONE_LEVEL_PAGE"] = url
        command = [str(arguments.build / "eden-wpe-tests"), "-nocrashhandler", "microphoneLevelFollowsGainControl"]
        if arguments.webkit_library:
            command = ["bwrap", "--dev-bind", "/", "/", "--ro-bind", str(arguments.webkit_library), str(arguments.webkit_target), "--", *command]
        with (stage / "test.log").open("w") as log:
            exit_code = subprocess.run(command, env=environment, stdout=log, stderr=subprocess.STDOUT, timeout=arguments.timeout_seconds).returncode
        measurements["eden"] = results.take(5) or {"error": f"Eden produced no result (exit {exit_code})"}
        measurements["eden"]["exit"] = exit_code
        measurements["eden"]["sourceVolumeAfter"] = source_volume()
        reset_source_volume()
        if not arguments.skip_chrome:
            measurements["chrome"] = run_chrome(stage, url, results)
            measurements["chrome"]["sourceVolumeAfter"] = source_volume()
        (stage / "measurements.json").write_text(json.dumps(measurements, indent=2) + "\n")
        return 0
    finally:
        if talker:
            stop([talker])
        server.shutdown()
        stop(processes)


def analyze(measurements, require_chrome):
    eden = measurements.get("eden") or {}
    require(not eden.get("error"), f"The Eden run failed: {eden.get('error')}")
    require(eden.get("exit") == 0, f"The Eden test program exited with {eden.get('exit')}")
    for name in ("raw", "processed"):
        settings = eden[name]["settings"]
        wanted = name == "processed"
        require(settings.get("echoCancellation") is wanted, f"Eden reported echoCancellation {settings.get('echoCancellation')} for the {name} capture")
    summary = {"edenRawP90": eden["raw"]["p90"], "edenProcessedP90": eden["processed"]["p90"], "edenGainDb": round(eden["processed"]["p90"] - eden["raw"]["p90"], 1)}
    chrome = measurements.get("chrome")
    if not chrome or chrome.get("skipped"):
        require(not require_chrome, "The Chrome reference did not run")
        return summary
    require(not chrome.get("error"), f"The Chrome reference failed: {chrome.get('error')}")
    summary.update({"chromeRawP90": chrome["raw"]["p90"], "chromeProcessedP90": chrome["processed"]["p90"], "chromeGainDb": round(chrome["processed"]["p90"] - chrome["raw"]["p90"], 1)})
    require(abs(eden["raw"]["p90"] - chrome["raw"]["p90"]) <= MAXIMUM_RAW_DIFFERENCE_DB, f"The unprocessed microphone differs between engines: Eden {eden['raw']['p90']} dBFS, Chrome {chrome['raw']['p90']} dBFS")
    summary["processedShortfallDb"] = round(chrome["processed"]["p90"] - eden["processed"]["p90"], 1)
    require(summary["processedShortfallDb"] <= MAXIMUM_PROCESSED_SHORTFALL_DB, f"Processed speech reaches {eden['processed']['p90']} dBFS in Eden and {chrome['processed']['p90']} dBFS in Chrome")
    return summary


def run(build, timeout_seconds, stage, webkit_library=None, webkit_target=None, skip_chrome=False, require_chrome=True, gst_debug=None):
    require((build / "eden-wpe-tests").is_file(), f"Missing eden-wpe-tests in {build}")
    for tool in ("dbus-run-session", "mutter", "pipewire", "wireplumber", "pipewire-pulse", "pactl", "pw-dump", "gst-launch-1.0", "bwrap", "espeak-ng"):
        require(shutil.which(tool), f"The microphone level check requires {tool}")
    runtime = Path(tempfile.mkdtemp(prefix="eden-microphone-", dir="/tmp"))
    try:
        runtime.chmod(0o700)
        configuration = runtime / "config"
        (configuration / "pipewire/pipewire.conf.d").mkdir(parents=True)
        (configuration / "pipewire/pipewire.conf.d/eden-test.conf").write_text(PIPEWIRE_CONFIG)
        (configuration / "wireplumber/wireplumber.conf.d").mkdir(parents=True)
        (configuration / "wireplumber/wireplumber.conf.d/eden-test.conf").write_text(WIREPLUMBER_CONFIG)
        environment = {
            key: value for key, value in os.environ.items()
            if not key.startswith(("DBUS_", "WAYLAND_", "JSC_", "WEBKIT_", "EDEN_WPE_", "GST_", "LLVM_PROFILE", "PULSE_", "PIPEWIRE_"))
            and key not in ("DISPLAY", "XAUTHORITY", "XDG_RUNTIME_DIR", "XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME", "XDG_STATE_HOME")
        }
        environment.update({
            "XDG_RUNTIME_DIR": str(runtime),
            "XDG_CONFIG_HOME": str(configuration),
            "XDG_DATA_HOME": str(runtime / "data"),
            "XDG_CACHE_HOME": str(runtime / "cache"),
            "XDG_STATE_HOME": str(runtime / "state"),
            "XDG_CURRENT_DESKTOP": "GNOME",
            "XDG_SESSION_TYPE": "wayland",
            "WAYLAND_DISPLAY": COMPOSITOR_SOCKET,
            "QT_QPA_PLATFORM": "wayland",
            "EDEN_WPE_PREWARM": "0",
        })
        if gst_debug:
            environment.update({"GST_DEBUG": gst_debug, "GST_DEBUG_NO_COLOR": "1"})
        command = [
            "dbus-run-session", "--", sys.executable, str(Path(__file__).resolve()), "--inside",
            "--build", str(build), "--stage", str(stage), "--timeout-seconds", str(timeout_seconds),
        ]
        if skip_chrome:
            command.append("--skip-chrome")
        if webkit_library:
            command += ["--webkit-library", str(webkit_library), "--webkit-target", str(webkit_target)]
        completed = subprocess.run(command, env=environment, timeout=timeout_seconds + CHROME_TIMEOUT_SECONDS + 90, check=False)
        summary = {"harnessExit": completed.returncode, "passed": False}
        try:
            measurements_path = stage / "measurements.json"
            require(measurements_path.exists(), f"The microphone level harness produced no measurements (exit {completed.returncode})")
            measurements = json.loads(measurements_path.read_text())
            summary["measurements"] = measurements
            summary.update(analyze(measurements, require_chrome and not skip_chrome))
            summary["passed"] = True
            return summary
        finally:
            (stage / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    finally:
        shutil.rmtree(runtime, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path, default=ROOT / "cmake-build-release")
    parser.add_argument("--timeout-seconds", type=int, default=240)
    parser.add_argument("--output-directory", type=Path)
    parser.add_argument("--webkit-library", type=Path)
    parser.add_argument("--webkit-target", type=Path)
    parser.add_argument("--skip-chrome", action="store_true")
    parser.add_argument("--allow-missing-chrome", action="store_true")
    parser.add_argument("--gst-debug", help="GST_DEBUG categories to record in test.log")
    parser.add_argument("--inside", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--stage", type=Path, help=argparse.SUPPRESS)
    arguments = parser.parse_args()
    if arguments.inside:
        sys.exit(inside(arguments))
    require(1 <= arguments.timeout_seconds <= 600, "Timeout must be between 1 and 600 seconds")
    require(bool(arguments.webkit_library) == bool(arguments.webkit_target), "A replacement WebKit library needs its target path")
    try:
        with tempfile.TemporaryDirectory(prefix="eden-microphone-check-") as temporary:
            stage = arguments.output_directory.resolve() if arguments.output_directory else Path(temporary)
            stage.mkdir(parents=True, exist_ok=True)
            summary = run(arguments.build.resolve(), arguments.timeout_seconds, stage, arguments.webkit_library, arguments.webkit_target,
                          arguments.skip_chrome, not arguments.allow_missing_chrome, arguments.gst_debug)
            print(json.dumps(summary, indent=2))
    except Exception as error:
        raise SystemExit(f"Microphone level check failed: {type(error).__name__}: {error}") from error


if __name__ == "__main__":
    main()
