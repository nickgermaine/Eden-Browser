import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import math
import os
from pathlib import Path
import random
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import wave


ROOT = Path(__file__).resolve().parents[1]
COMPOSITOR_SOCKET = "eden-voice"
MICROPHONE = "Eden test microphone"
MINIMUM_ECHO_REDUCTION_DB = 15.0
NEAR_END_FREQUENCY = 5000
NEAR_END_RATE = 48000
NEAR_END_AMPLITUDE = 0.2
NEAR_END_SECONDS = 60
NEAR_END_SYLLABLE_MS = (160, 360)
NEAR_END_PAUSE_MS = (60, 220)
NEAR_END_RAMP_MS = 10
NEAR_END_SEED = 5000
MAXIMUM_NEAR_END_LOSS_DB = 6.0
MINIMUM_SIGNAL_POWER = 1e-7
MAXIMUM_ROOM_DIFFERENCE_DB = 3.0
FAR_END_SPEECH = (
    "Thanks for joining the call. Can everyone hear me clearly? Let's go over the schedule for next week, "
    "then look at the open questions from the design review. Please share your screen when you're ready."
)

PIPEWIRE_CONFIG = """
context.objects = [
    { factory = adapter
        args = {
            factory.name = support.null-audio-sink
            node.name = "eden-test-speaker"
            node.description = "Eden test speaker"
            media.class = "Audio/Sink"
            audio.position = [ FL FR ]
            priority.session = 2000
            priority.driver = 2000
            object.linger = true
        }
    }
    { factory = adapter
        args = {
            factory.name = support.null-audio-sink
            node.name = "eden-test-room"
            node.description = "Eden test room"
            media.class = "Audio/Sink"
            audio.position = [ FL FR ]
            priority.session = 100
            priority.driver = 100
            object.linger = true
        }
    }
]
context.modules = [
    { name = libpipewire-module-loopback
        args = {
            node.description = "Eden test echo path"
            capture.props = {
                node.name = "eden-test-echo-capture"
                target.object = "eden-test-speaker"
                stream.capture.sink = true
                node.passive = true
                audio.position = [ FL FR ]
            }
            playback.props = {
                node.name = "eden-test-echo-playback"
                target.object = "eden-test-room"
                node.dont-reconnect = true
                audio.position = [ FL FR ]
            }
        }
    }
    { name = libpipewire-module-loopback
        args = {
            node.description = "Eden test microphone path"
            capture.props = {
                node.name = "eden-test-room-capture"
                target.object = "eden-test-room"
                stream.capture.sink = true
                node.passive = true
                audio.position = [ FL FR ]
            }
            playback.props = {
                node.name = "eden-test-microphone"
                node.description = "Eden test microphone"
                media.class = "Audio/Source"
                priority.session = 2000
                audio.position = [ FL FR ]
            }
        }
    }
]
"""

WIREPLUMBER_CONFIG = """
wireplumber.profiles = {
    main = {
        monitor.alsa = disabled
        monitor.alsa-midi = disabled
        monitor.bluez = disabled
        monitor.bluez-midi = disabled
        monitor.v4l2 = disabled
        monitor.libcamera = disabled
    }
}
"""


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def wait_for(condition, message, timeout=20):
    deadline = time.monotonic() + timeout
    while not condition():
        require(time.monotonic() < deadline, message)
        time.sleep(0.1)


def node_names():
    completed = subprocess.run(["pw-dump"], capture_output=True, text=True, check=False)
    if completed.returncode:
        return set()
    names = set()
    for item in json.loads(completed.stdout or "[]"):
        properties = (item.get("info") or {}).get("props") or {}
        if item.get("type") == "PipeWire:Interface:Node" and "node.name" in properties:
            names.add(properties["node.name"])
    return names


def start(stage, name, command):
    return subprocess.Popen(command, stdout=(stage / f"{name}.log").open("w"), stderr=subprocess.STDOUT)


def stop(processes):
    for process in reversed(processes):
        process.terminate()
    for process in reversed(processes):
        try:
            process.wait(10)
        except subprocess.TimeoutExpired:
            process.kill()


def write_near_end_speech(path):
    generator = random.Random(NEAR_END_SEED)
    total = NEAR_END_RATE * NEAR_END_SECONDS
    ramp = NEAR_END_RATE * NEAR_END_RAMP_MS // 1000
    envelope = [0.0] * total
    position = 0
    while position < total:
        length = min(total - position, NEAR_END_RATE * generator.randint(*NEAR_END_SYLLABLE_MS) // 1000)
        for offset in range(length):
            edge = min(offset, length - 1 - offset)
            envelope[position + offset] = 0.5 - 0.5 * math.cos(math.pi * edge / ramp) if edge < ramp else 1.0
        position += length + NEAR_END_RATE * generator.randint(*NEAR_END_PAUSE_MS) // 1000
    step = 2 * math.pi * NEAR_END_FREQUENCY / NEAR_END_RATE
    samples = bytearray()
    for index, gain in enumerate(envelope):
        samples += int(32767 * NEAR_END_AMPLITUDE * gain * math.sin(step * index)).to_bytes(2, "little", signed=True)
    with wave.open(str(path), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(NEAR_END_RATE)
        output.writeframes(bytes(samples))


class NearEndTalker:
    def __init__(self, stage):
        self.stage = stage
        self.speech = stage / "near-end.wav"
        self.process = None
        self.lock = threading.Lock()
        write_near_end_speech(self.speech)

    def start(self):
        with self.lock:
            if self.process:
                return
            self.process = start(self.stage, "near-end", [
                "gst-launch-1.0", "filesrc", f"location={self.speech}", "!", "wavparse", "!", "audioconvert", "!",
                "audioresample", "!", "pulsesink", "device=eden-test-room",
            ])

    def stop(self):
        with self.lock:
            if self.process:
                stop([self.process])
                self.process = None


def control_server(talker, speech):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *arguments):
            return

        def do_GET(self):
            routes = {"/near-end/on": talker.start, "/near-end/off": talker.stop}
            route = routes.get(self.path)
            if route:
                route()
                self.reply(200, "text/plain", str(NEAR_END_FREQUENCY).encode())
            elif self.path == "/far-end.wav":
                self.reply(200, "audio/wav", speech)
            else:
                self.reply(404, "text/plain", b"")

        def reply(self, status, content_type, body):
            self.send_response(status)
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def inside(arguments):
    runtime = Path(os.environ["XDG_RUNTIME_DIR"])
    stage = arguments.stage
    processes = []
    talker = NearEndTalker(stage)
    speech = stage / "far-end.wav"
    subprocess.run(["espeak-ng", "-v", "en-us", "-s", "165", "-w", str(speech), FAR_END_SPEECH], check=True)
    server = control_server(talker, speech.read_bytes())
    try:
        processes.append(start(stage, "mutter", [
            "mutter", "--headless", "--wayland", "--no-x11", "--virtual-monitor", "1600x1000", "--wayland-display", COMPOSITOR_SOCKET,
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
        environment = dict(os.environ)
        environment["EDEN_TEST_VOICE_PROCESSING_CONTROL"] = f"http://127.0.0.1:{server.server_address[1]}"
        command = [str(arguments.build / "eden-wpe-tests"), "-nocrashhandler", "voiceProcessingCancelsEcho"]
        if arguments.webkit_library:
            command = ["bwrap", "--dev-bind", "/", "/", "--ro-bind", str(arguments.webkit_library), str(arguments.webkit_target), "--", *command]
        with (stage / "test.log").open("w") as log:
            return subprocess.run(command, env=environment, stdout=log, stderr=subprocess.STDOUT, timeout=arguments.timeout_seconds).returncode
    finally:
        talker.stop()
        server.shutdown()
        stop(processes)


def decibels(numerator, denominator):
    return 10 * math.log10(max(numerator, 1e-20) / max(denominator, 1e-20))


def analyze(results):
    enabled = results["enabled"]
    disabled = results["disabled"]
    require(enabled.get("echoCancellation") is True and disabled.get("echoCancellation") is False, "The capture did not report the requested echo cancellation setting")
    require(enabled.get("playing") and disabled.get("playing"), "The far-end audio did not play")
    require(enabled.get("connected") == "connected" and disabled.get("connected") == "connected", "The call did not connect")
    require(disabled["echo"] > MINIMUM_SIGNAL_POWER, "The echo path did not reach the microphone")
    require(disabled["nearEnd"] > MINIMUM_SIGNAL_POWER, "The near-end talker did not reach the microphone")
    for run in (enabled, disabled):
        require(run["roomEcho"] > MINIMUM_SIGNAL_POWER, "The far end was not audible in the room")
        require(run["roomNearEnd"] > MINIMUM_SIGNAL_POWER, "The near-end talker was not audible in the room")
    require(abs(decibels(enabled["roomEcho"], disabled["roomEcho"])) <= MAXIMUM_ROOM_DIFFERENCE_DB, "The far end played at different levels in the two runs")
    require(abs(decibels(enabled["roomNearEnd"], disabled["roomNearEnd"])) <= MAXIMUM_ROOM_DIFFERENCE_DB, "The near-end talker played at different levels in the two runs")
    for key in ("roomDoubleTalkEcho", "roomDoubleTalkNearEnd"):
        require(enabled[key] > MINIMUM_SIGNAL_POWER and disabled[key] > MINIMUM_SIGNAL_POWER, "Both talkers were not audible during double talk")
        require(abs(decibels(enabled[key], disabled[key])) <= MAXIMUM_ROOM_DIFFERENCE_DB, "Double talk played at different levels in the two runs")
    return {
        "echoReductionDb": decibels(disabled["echo"], enabled["echo"]),
        "nearEndChangeDb": decibels(enabled["nearEnd"], disabled["nearEnd"]),
        "roomEchoDifferenceDb": decibels(enabled["roomEcho"], disabled["roomEcho"]),
        "doubleTalkEchoReductionDb": decibels(disabled["doubleTalkEcho"], enabled["doubleTalkEcho"]),
        "doubleTalkNearEndChangeDb": decibels(enabled["doubleTalkNearEnd"], disabled["doubleTalkNearEnd"]),
    }


def run(build, timeout_seconds, stage, webkit_library=None, webkit_target=None, gst_debug=None):
    require((build / "eden-wpe-tests").is_file(), f"Missing eden-wpe-tests in {build}")
    for tool in ("dbus-run-session", "mutter", "pipewire", "wireplumber", "pipewire-pulse", "pactl", "pw-dump", "gst-launch-1.0", "bwrap", "espeak-ng"):
        require(shutil.which(tool), f"The voice processing check requires {tool}")
    runtime = Path(tempfile.mkdtemp(prefix="eden-voice-", dir="/tmp"))
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
            "EDEN_TEST_VOICE_PROCESSING": MICROPHONE,
        })
        if gst_debug:
            environment.update({"GST_DEBUG": gst_debug, "GST_DEBUG_NO_COLOR": "1"})
        command = [
            "dbus-run-session", "--", sys.executable, str(Path(__file__).resolve()), "--inside",
            "--build", str(build), "--stage", str(stage), "--timeout-seconds", str(timeout_seconds),
        ]
        if webkit_library:
            command += ["--webkit-library", str(webkit_library), "--webkit-target", str(webkit_target)]
        completed = subprocess.run(command, env=environment, timeout=timeout_seconds + 90, check=False)
        test_log = (stage / "test.log").read_text() if (stage / "test.log").exists() else ""
        summary = {"testExit": completed.returncode, "passed": False}
        try:
            match = re.search(r"Voice processing results: (\{.*\})", test_log)
            require(match, f"The voice processing test produced no results (exit {completed.returncode})")
            results = json.loads(match[1])
            summary["measurements"] = results
            summary.update(analyze(results))
            require(completed.returncode == 0, f"The voice processing test exited with {completed.returncode}")
            require(summary["echoReductionDb"] >= MINIMUM_ECHO_REDUCTION_DB, f"Echo was reduced by only {summary['echoReductionDb']:.1f} dB")
            require(summary["nearEndChangeDb"] >= -MAXIMUM_NEAR_END_LOSS_DB, f"The near-end talker lost {-summary['nearEndChangeDb']:.1f} dB")
            require(summary["doubleTalkEchoReductionDb"] >= MINIMUM_ECHO_REDUCTION_DB, f"Echo was reduced by only {summary['doubleTalkEchoReductionDb']:.1f} dB during double talk")
            require(summary["doubleTalkNearEndChangeDb"] >= -MAXIMUM_NEAR_END_LOSS_DB, f"The near-end talker lost {-summary['doubleTalkNearEndChangeDb']:.1f} dB during double talk")
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
    parser.add_argument("--gst-debug", help="GST_DEBUG categories to record in test.log")
    parser.add_argument("--inside", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--stage", type=Path, help=argparse.SUPPRESS)
    arguments = parser.parse_args()
    if arguments.inside:
        sys.exit(inside(arguments))
    require(1 <= arguments.timeout_seconds <= 600, "Timeout must be between 1 and 600 seconds")
    require(bool(arguments.webkit_library) == bool(arguments.webkit_target), "A replacement WebKit library needs its target path")
    try:
        with tempfile.TemporaryDirectory(prefix="eden-voice-check-") as temporary:
            stage = arguments.output_directory.resolve() if arguments.output_directory else Path(temporary)
            stage.mkdir(parents=True, exist_ok=True)
            print(json.dumps(run(arguments.build.resolve(), arguments.timeout_seconds, stage, arguments.webkit_library, arguments.webkit_target, arguments.gst_debug), indent=2))
    except Exception as error:
        raise SystemExit(f"Voice processing check failed: {type(error).__name__}: {error}") from error


if __name__ == "__main__":
    main()
