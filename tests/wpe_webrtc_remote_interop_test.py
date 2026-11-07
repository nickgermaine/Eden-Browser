import argparse
import base64
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import re
import secrets
import subprocess
import sys
import tempfile
import threading

import gi

gi.require_version("Nice", "0.1")
from gi.repository import GLib, Nice


ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests/wpe_webrtc_remote_interop.js"
RUNNER = ROOT / "scripts/test_wpe.py"
CONNECTED_STATES = (Nice.ComponentState.CONNECTED, Nice.ComponentState.READY)


MDNS_CANDIDATE = re.compile(r"^(?:a=)?candidate:\S* \d+ udp \d+ [0-9a-f-]{36}\.local \d+ typ host", re.IGNORECASE)


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def clean_environment():
    environment = os.environ.copy()
    for name in list(environment):
        if name.startswith(("JSC_", "WEBKIT_", "EDEN_WPE_", "SYSPROF_", "LLVM_PROFILE")):
            environment.pop(name)
    environment["EDEN_WPE_PREWARM"] = "0"
    environment["GST_DEBUG"] = "0"
    return environment


def google_style_credentials():
    ufrag = base64.b64encode(secrets.token_bytes(24)).decode()
    pwd = base64.b64encode(secrets.token_bytes(20)).decode()
    require(pwd.endswith("=") and "=" not in ufrag, "Google-style credentials changed shape")
    return ufrag, pwd


class LiteAgent:
    def __init__(self, context):
        self.context = context
        self.agent = Nice.Agent.new_full(context, Nice.Compatibility.RFC5245, Nice.AgentOption.LITE_MODE)
        self.agent.props.controlling_mode = False
        self.agent.props.ice_tcp = False
        self.agent.props.upnp = False
        self.states = []
        self.gathered = threading.Event()
        self.connected = threading.Event()
        self.agent.connect("candidate-gathering-done", lambda agent, stream: self.gathered.set())
        self.agent.connect("component-state-changed", self._state_changed)
        self.stream = self.agent.add_stream(1)
        self.ufrag, self.pwd = google_style_credentials()
        require(self.agent.set_local_credentials(self.stream, self.ufrag, self.pwd), "libnice rejected local credentials")
        self.agent.attach_recv_ex(self.stream, 1, context, lambda *arguments: None, None)
        require(self.agent.gather_candidates(self.stream), "libnice could not gather candidates")

    def _state_changed(self, agent, stream, component, state):
        self.states.append(int(state))
        if state in CONNECTED_STATES:
            self.connected.set()

    def description(self):
        candidates = [
            self.agent.generate_local_candidate_sdp(candidate)
            for candidate in self.agent.get_local_candidates(self.stream, 1)
            if candidate.transport == Nice.CandidateTransport.UDP
        ]
        require(candidates, "The test ICE agent has no UDP candidates")
        return {"ufrag": self.ufrag, "pwd": self.pwd, "candidates": candidates}

    def apply_remote(self, details):
        ufrag = details.get("ufrag")
        pwd = details.get("pwd")
        require(isinstance(ufrag, str) and isinstance(pwd, str), "The page did not send its ICE parameters")
        require(self.agent.set_remote_credentials(self.stream, ufrag, pwd), "libnice rejected the page credentials")
        lines = details.get("candidates", [])
        parsed = [candidate for candidate in (self.agent.parse_remote_candidate_sdp(self.stream, line) for line in lines) if candidate]
        obfuscated = [line for line in lines if MDNS_CANDIDATE.search(line)]
        require(parsed or obfuscated, "The page sent no usable UDP candidates")
        self.remote_candidates = {"addresses": len(parsed), "mdns": len(obfuscated)}
        if parsed:
            self.agent.set_remote_candidates(self.stream, 1, parsed)

    def state(self):
        return {
            "remoteCandidates": getattr(self, "remote_candidates", None),
            "connected": self.connected.is_set(),
            "peerControlling": int(Nice.ComponentState.READY) in self.states,
            "states": list(self.states),
        }


class Signaling:
    def __init__(self):
        self.context = GLib.MainContext.new()
        self.loop = GLib.MainLoop.new(self.context, False)
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()
        self.answerer = self._call(lambda: LiteAgent(self.context))
        self.offerer = self._call(lambda: LiteAgent(self.context))
        signaling = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *arguments):
                return

            def _reply(self, payload, status=200):
                body = json.dumps(payload).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Access-Control-Allow-Origin", "*")
                self.end_headers()
                self.wfile.write(body)

            def do_GET(self):
                routes = {
                    "/agent": lambda: signaling._describe(signaling.answerer),
                    "/state": lambda: signaling._call(signaling.answerer.state),
                    "/offer": lambda: signaling._describe(signaling.offerer),
                    "/offer-state": lambda: signaling._call(signaling.offerer.state),
                }
                self._dispatch(routes)

            def do_POST(self):
                length = int(self.headers.get("Content-Length", "0"))
                require(0 < length <= 262144, "Signaling request body is invalid")
                details = json.loads(self.rfile.read(length))
                routes = {
                    "/remote": lambda: signaling._call(lambda: signaling.answerer.apply_remote(details)) or {"ok": True},
                    "/offer-answer": lambda: signaling._call(lambda: signaling.offerer.apply_remote(details)) or {"ok": True},
                }
                self._dispatch(routes)

            def _dispatch(self, routes):
                route = routes.get(self.path)
                if not route:
                    self._reply({"error": "unknown route"}, 404)
                    return
                try:
                    self._reply(route())
                except Exception as error:
                    signaling.errors.append(f"{self.path}: {type(error).__name__}: {error}")
                    self._reply({"error": type(error).__name__}, 500)

        self.errors = []
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server_thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.server_thread.start()
        self.url = f"http://127.0.0.1:{self.server.server_address[1]}"

    def _run(self):
        self.context.push_thread_default()
        self.loop.run()

    def _call(self, function):
        done = threading.Event()
        outcome = {}

        def invoke():
            try:
                outcome["value"] = function()
            except Exception as error:
                outcome["error"] = error
            done.set()
            return GLib.SOURCE_REMOVE

        source = GLib.idle_source_new()
        source.set_callback(invoke)
        source.attach(self.context)
        require(done.wait(20), "The ICE agent thread did not respond")
        if "error" in outcome:
            raise outcome["error"]
        return outcome.get("value")

    def _describe(self, agent):
        require(agent.gathered.wait(15), "The test ICE agent did not finish gathering")
        return self._call(agent.description)

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.loop.quit()
        self.thread.join(5)


def validate_safe_payload(value, secrets_to_hide):
    text = json.dumps(value)
    for secret in secrets_to_hide:
        require(secret not in text, "The probe result leaked an ICE secret")
    require(not re.search(r"(?:^|\\n|\")(?:v=0|a=candidate:|a=ice-pwd:|a=ice-ufrag:)", text), "The probe result leaked SDP text")


def validate_result(result):
    measurements = result.get("measurements")
    require(result.get("probe") == "webrtc-remote-interop" and isinstance(measurements, dict), "Probe result is malformed")
    failures = measurements.get("failures")
    require(failures == [], f"Probe failures: {failures}")
    sfu = measurements.get("sfuLoopback") or {}
    require(sfu.get("passed") is True, "The SFU-shaped loopback did not pass")
    require(sfu.get("answerRewrites", {}).get("legacyDataChannel") is True, "The SFU answer did not use the legacy data channel form")
    require(sfu.get("answerRewrites", {}).get("rewrittenAddresses", 0) > 0, "The SFU answer carried no empty-foundation addresses")
    require(len(sfu.get("clientTransceivers", [])) == 5, "The client transceiver set changed")
    require(sfu.get("remoteDataSection") in ("normalized", "legacy"), "The legacy data channel section was neither kept nor normalized")
    require(sfu.get("echo") == "echo:hello", "The data channel did not echo")
    media = sfu.get("media") or {}
    require(media.get("clientVideo") == {"width": 320, "height": 180}, "The client did not render remote video")
    require(media.get("clientAudioPackets") and media.get("sfuVideoFrames") and media.get("sfuAudioPackets"), "Media did not flow in both directions")
    answerer = measurements.get("answererSending") or {}
    require(answerer.get("passed") is True, "The answerer did not send media")
    require(answerer.get("addTrack") == {"width": 320, "streams": 1}, "The answerer track added after the offer was not received with its stream")
    require(answerer.get("addTrackAnswerAnnouncesStream") is True, "The answer did not announce the added stream")
    require(answerer.get("lateReplaceTrack") == {"width": 320}, "A track replaced after negotiation was not sent")
    tolerated = measurements.get("toleratedIceCharacters") or {}
    require(tolerated.get("passed") is True, "ICE with a padded password did not pass")
    require(tolerated.get("remoteDescriptionKeepsOriginal") is True, "The reported remote description changed the ICE parameters")
    require(tolerated.get("stayedConnected") is True, "Renegotiation restarted ICE")
    require(tolerated.get("agentConnected") is True, "The libnice agent did not connect")
    lite = measurements.get("liteOffer") or {}
    require(lite.get("passed") is True and lite.get("agentConnected") is True, "Answering a lite offerer did not connect")
    rejections = measurements.get("rejections") or {}
    require(rejections.get("passed") is True, "An invalid ICE password was accepted")
    meet = measurements.get("meetRenegotiation") or {}
    require(meet.get("ssltcpCandidate") == "accepted", "A trickled ssltcp candidate was rejected")
    require(meet.get("malformedCandidate") not in (None, "accepted"), "A malformed ICE candidate was accepted")
    require(meet.get("renegotiationOffer") == "created", "createOffer failed after setCodecPreferences")
    require(meet.get("opusPayloadKept") is True and meet.get("videoCodecKept") is True, "setCodecPreferences changed the negotiated codecs")
    require(meet.get("mediaContinued") is True and meet.get("connection") == "connected", "Media stopped after renegotiation")
    require(meet.get("offerAfterReplaceTrack") == "created" and meet.get("replacedTrackFlowing") is True, "Renegotiation after replaceTrack failed or stopped the audio")
    require(meet.get("balancedOffersBundle") is True and meet.get("balancedConnected") is True, "The default bundle policy did not bundle and connect")
    require(meet.get("passed") is True, "The Meet-shaped renegotiation did not pass")
    for name in ("invalidCharacter", "tooShort", "space"):
        require(rejections.get(name) in ("InvalidAccessError", "OperationError"), f"Rejection case {name} returned {rejections.get(name)}")
    return {
        "passed": True,
        "sfuLoopback": {key: sfu[key] for key in ("answerRewrites", "remoteDataSection", "echo", "media", "connection")},
        "answererSending": {key: answerer[key] for key in ("addTrack", "addTrackAnswerAnnouncesStream", "lateReplaceTrack")},
        "toleratedIceCharacters": {key: tolerated[key] for key in ("remoteDescriptionKeepsOriginal", "statesBeforeRenegotiation", "statesAfterRenegotiation", "stayedConnected", "agentConnected")},
        "liteOffer": {key: lite[key] for key in ("states", "agentConnected", "peerControlling")},
        "rejections": rejections,
    }


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


def run_in_directory(build, timeout_seconds, stage, runner_arguments):
    build = build.resolve()
    require((build / "eden-wpe-bench").is_file(), f"Missing eden-wpe-bench in {build}")
    require(FIXTURE.is_file(), "WebRTC remote interoperability fixture is missing")
    signaling = Signaling()
    summary = {"passed": False}
    probe = stage / "probe.js"
    result_path = stage / "result.json"
    summary_path = stage / "summary.json"
    try:
        probe.write_text("globalThis.edenInteropSignaling = " + json.dumps({"url": signaling.url}) + ";\n" + FIXTURE.read_text())
        command = [
            sys.executable,
            str(RUNNER),
            "--build",
            str(build),
            "--timeout-seconds",
            str(timeout_seconds),
            "--writable-path",
            str(stage),
            *runner_arguments,
            "--benchmark",
            "--",
            "--output",
            str(result_path),
            "--label",
            "webrtc-remote-interop",
            "--probe-script",
            str(probe),
        ]
        completed = subprocess.run(
            command,
            env=clean_environment(),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=timeout_seconds + 45,
            check=False,
        )
        (stage / "runner.log").write_text(completed.stdout)
        require(result_path.is_file(), f"WPE probe exited with status {completed.returncode} without a result")
        raw_result = json.loads(result_path.read_text())
        hidden = [signaling.answerer.pwd, signaling.answerer.ufrag, signaling.offerer.pwd, signaling.offerer.ufrag]
        validate_safe_payload(raw_result, hidden)
        result = {key: raw_result[key] for key in ("probe", "measurements", "viewport", "visibility", "runtime", "webkitVersion") if key in raw_result}
        require_configured_runtime(build, result)
        result_path.write_text(json.dumps(result, indent=2) + "\n")
        require(not signaling.errors, f"Signaling errors: {signaling.errors}")
        summary.update(validate_result(result))
        require(completed.returncode == 0, f"WPE probe exited with status {completed.returncode}")
        summary["passed"] = True
        return summary
    except Exception as error:
        summary["error"] = f"{type(error).__name__}: {error}"
        raise
    finally:
        signaling.close()
        summary_path.write_text(json.dumps(summary, indent=2) + "\n")


def run(build, timeout_seconds, output_directory=None, runner_arguments=()):
    if output_directory:
        stage = output_directory.resolve()
        stage.mkdir(parents=True, exist_ok=False)
        return run_in_directory(build, timeout_seconds, stage, runner_arguments)
    with tempfile.TemporaryDirectory(prefix="eden-webrtc-remote-interop-") as temporary:
        return run_in_directory(build, timeout_seconds, Path(temporary), runner_arguments)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=Path, default=ROOT / "cmake-build-release")
    parser.add_argument("--timeout-seconds", type=int, default=240)
    parser.add_argument("--output-directory", type=Path)
    arguments = parser.parse_args()
    require(1 <= arguments.timeout_seconds <= 600, "Timeout must be between 1 and 600 seconds")
    try:
        print(json.dumps(run(arguments.build, arguments.timeout_seconds, arguments.output_directory), indent=2))
    except Exception as error:
        raise SystemExit(f"WebRTC remote interoperability regression failed: {type(error).__name__}: {error}") from error


if __name__ == "__main__":
    main()
