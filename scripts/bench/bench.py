""":"
exec python3 "$0" "$@"
":"""

import argparse
import json
import math
import os
import pathlib
import re
import select
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True


ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT_ROOT = pathlib.Path(__file__).resolve().parent
BASELINE = json.loads((SCRIPT_ROOT / "baseline.json").read_text())
BOUNDARIES = {
    "startup.shell_first_frame_ms": 150.0,
    "startup.web_first_paint_ms": 900.0,
    "tabswitch.input_to_frame_ms": 8.3,
    "input.key_to_frame_ms": 8.3,
    "frame.interval_ms": 16.667,
    "memory.shell_private_mb": 150.0,
    "idle.shell_cpu_percent": 0.0,
}
PERF_PATTERN = re.compile(r"EDEN_PERF\s+([A-Za-z0-9_.]+)=([^\s]+)")
GPU_FAILURE_PATTERN = re.compile(r"GPU process exited|exit_code=139|SIGSEGV|segmentation fault|SharedImageBackingFactory")
PHASE_SAMPLE_COUNT = 20
TARGET_REFRESH_HZ = 120.0


def percentile(values, fraction):
    ordered = sorted(values)
    if not ordered:
        return math.nan
    index = min(len(ordered) - 1, max(0, math.ceil(len(ordered) * fraction) - 1))
    return ordered[index]


def summary(values):
    return {
        "samples": len(values),
        "p50": percentile(values, 0.50),
        "p95": percentile(values, 0.95),
        "p99": percentile(values, 0.99),
    }


def json_ready(value):
    if isinstance(value, float) and not math.isfinite(value):
        return None
    if isinstance(value, dict):
        return {key: json_ready(entry) for key, entry in value.items()}
    if isinstance(value, list):
        return [json_ready(entry) for entry in value]
    return value


def measured_driver_runs(driver_text, key, warmup_count=1):
    pattern = re.compile(rf"{re.escape(key)}=([0-9.]+)")
    return [[float(value) for value in pattern.findall(text)][warmup_count:] for text in driver_text]


def phase_delay_seconds(index):
    phase = (index * 7) % PHASE_SAMPLE_COUNT
    return phase / (TARGET_REFRESH_HZ * PHASE_SAMPLE_COUNT)


def parse_perf(text):
    values = {}
    for metric, raw_value in PERF_PATTERN.findall(text):
        try:
            value = float(raw_value)
        except ValueError:
            continue
        values.setdefault(metric, []).append(value)
    return values


def compile_native(temp_root):
    compiler = shutil.which("c++")
    if not compiler:
        raise RuntimeError("c++ compiler unavailable")
    driver = temp_root / "driver"
    subprocess.run(
        [compiler, "-std=c++20", "-O2", str(SCRIPT_ROOT / "native" / "driver.cpp"), "-o", str(driver), "-lX11", "-lXtst"],
        check=True,
    )
    return driver


def generate_video(page_root):
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        return False
    destination = page_root / "video.webm"
    subprocess.run(
        [
            ffmpeg,
            "-hide_banner",
            "-loglevel",
            "error",
            "-f",
            "lavfi",
            "-i",
            "testsrc2=size=3840x2160:rate=60:duration=2",
            "-c:v",
            "libvpx-vp9",
            "-deadline",
            "realtime",
            "-cpu-used",
            "8",
            "-y",
            str(destination),
        ],
        check=True,
    )
    return True


def session(page_uri, tabs=1):
    entries = [{"backend": "cef", "pinned": False, "title": "bench:ready", "url": page_uri} for _ in range(tabs)]
    return {"version": 3, "windows": [{"activeIndex": 0, "layout": "horizontal", "tabs": entries}]}


class AutomationClient:
    def __init__(self, path):
        self.path = path
        self.socket = None
        self.next_id = 1

    def connect(self, timeout=20):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            candidate = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                candidate.connect(str(self.path))
                candidate.settimeout(2)
                self.socket = candidate
                return
            except OSError:
                candidate.close()
                time.sleep(0.01)
        raise TimeoutError("automation socket did not accept a connection")

    def close(self):
        if self.socket:
            self.socket.close()
            self.socket = None

    def call(self, method, parameters=None):
        identifier = self.next_id
        self.next_id += 1
        request = {"jsonrpc": "2.0", "id": identifier, "method": method, "params": parameters or {}}
        self.socket.sendall(json.dumps(request, separators=(",", ":")).encode() + b"\n")
        response = b""
        while not response.endswith(b"\n"):
            chunk = self.socket.recv(65536)
            if not chunk:
                raise ConnectionError("automation socket closed")
            response += chunk
        parsed = json.loads(response)
        if parsed.get("id") != identifier:
            raise RuntimeError("automation response id mismatch")
        if "error" in parsed:
            raise RuntimeError(parsed["error"]["message"])
        return parsed.get("result")

    def sequence(self):
        return int(self.call("dumpMetrics").get("sequence", 0))

    def wait_metric(self, name, after, timeout=10):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            metrics = self.call("dumpMetrics", {"afterSequence": after})
            for sample in metrics.get("samples", []):
                if sample.get("name") == name:
                    return float(sample["value"]), int(sample["sequence"])
            time.sleep(0.002)
        raise TimeoutError(f"metric did not arrive: {name}")

    def wait_metric_count(self, name, count, timeout=20):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            metrics = self.call("dumpMetrics", {"afterSequence": 0})
            samples = [sample for sample in metrics.get("samples", []) if sample.get("name") == name]
            if len(samples) >= count:
                return max(int(sample["sequence"]) for sample in samples)
            time.sleep(0.002)
        raise TimeoutError(f"metric count did not arrive: {name} {count}")


def process_tree_rss(process):
    total = shell_rss(process)
    parent_map = {}
    for stat_path in pathlib.Path("/proc").glob("[0-9]*/stat"):
        try:
            fields = stat_path.read_text().split()
            parent_map.setdefault(int(fields[3]), []).append(int(fields[0]))
        except (OSError, ValueError, IndexError):
            continue
    pending = list(parent_map.get(process, []))
    while pending:
        child = pending.pop()
        total += shell_rss(child)
        pending.extend(parent_map.get(child, []))
    return total


def shell_rss(process):
    status_path = pathlib.Path(f"/proc/{process}/status")
    if not status_path.exists():
        return 0.0
    status = status_path.read_text()
    match = re.search(r"^VmRSS:\s+([0-9]+)\s+kB$", status, re.MULTILINE)
    return float(match.group(1)) / 1024.0 if match else math.nan


def shell_memory_details(process):
    details = {}
    rollup = pathlib.Path(f"/proc/{process}/smaps_rollup")
    if rollup.exists():
        for line in rollup.read_text().splitlines():
            match = re.match(r"^([A-Za-z_]+):\s+([0-9]+)\s+kB$", line)
            if match:
                details[match.group(1)] = float(match.group(2)) / 1024.0
    mapped = {}
    smaps = pathlib.Path(f"/proc/{process}/smaps")
    if smaps.exists():
        current_path = "anonymous"
        for line in smaps.read_text(errors="replace").splitlines():
            header = re.match(r"^[0-9a-f]+-[0-9a-f]+\s+\S+\s+\S+\s+\S+\s+\S+\s*(.*)$", line)
            if header:
                current_path = header.group(1) or "anonymous"
            elif line.startswith("Rss:"):
                value = float(line.split()[1]) / 1024.0
                category = "libcef" if "libcef.so" in current_path else "qtwebengine" if "Qt6WebEngine" in current_path else "other"
                mapped[category] = mapped.get(category, 0.0) + value
    return details, mapped


def cpu_sample(process, seconds):
    clock_ticks = os.sysconf(os.sysconf_names["SC_CLK_TCK"])
    before_threads = thread_ticks(process)
    latest_threads = dict(before_threads)
    before = pathlib.Path(f"/proc/{process}/stat").read_text().split()
    started = time.monotonic()
    deadline = started + seconds
    while time.monotonic() < deadline:
        time.sleep(min(0.05, max(0.0, deadline - time.monotonic())))
        latest_threads.update(thread_ticks(process))
    after = pathlib.Path(f"/proc/{process}/stat").read_text().split()
    elapsed = time.monotonic() - started
    cpu_seconds = ((int(after[13]) + int(after[14])) - (int(before[13]) + int(before[14]))) / clock_ticks
    ticks = (int(after[13]) + int(after[14])) - (int(before[13]) + int(before[14]))
    latest_threads.update(thread_ticks(process))
    deltas = []
    for thread, after_sample in latest_threads.items():
        before_sample = before_threads.get(thread, {"utime": 0, "stime": 0, "name": after_sample["name"]})
        user_ticks = after_sample["utime"] - before_sample["utime"]
        system_ticks = after_sample["stime"] - before_sample["stime"]
        if user_ticks or system_ticks:
            deltas.append({"tid": thread, "name": after_sample["name"], "utime_ticks": user_ticks, "stime_ticks": system_ticks})
    return 100.0 * cpu_seconds / elapsed, ticks, deltas


def thread_ticks(process):
    samples = {}
    for stat_path in pathlib.Path(f"/proc/{process}/task").glob("[0-9]*/stat"):
        try:
            text = stat_path.read_text()
            close = text.rfind(")")
            thread = int(stat_path.parent.name)
            name = text[text.find("(") + 1 : close]
            fields = text[close + 2 :].split()
            samples[thread] = {"name": name, "utime": int(fields[11]), "stime": int(fields[12])}
        except (OSError, ValueError, IndexError):
            continue
    return samples


def run_eden_case(binary, mode, scenario_name, page_uri, temp_root, tabs=1, seconds=25, engine_stage=None, idle_seconds=30):
    xdg_root = pathlib.Path(tempfile.mkdtemp(prefix=f"eden-bench-{mode}-{scenario_name}-", dir=temp_root))
    data_root = xdg_root / "data"
    session_root = data_root / "Eden" / "Eden"
    session_root.mkdir(parents=True)
    (xdg_root / "config").mkdir()
    (xdg_root / "cache").mkdir()
    (session_root / "session.json").write_text(json.dumps(session(page_uri, tabs)))
    runtime_root = pathlib.Path(os.environ.get("XDG_RUNTIME_DIR", ""))
    if not runtime_root.is_dir():
        raise RuntimeError("XDG_RUNTIME_DIR is unavailable")
    socket_root = pathlib.Path(tempfile.mkdtemp(prefix="eden-bench-", dir=runtime_root))
    socket_root.chmod(0o700)
    socket_path = socket_root / "automation.sock"
    environment = os.environ.copy()
    environment.update(
        {
            "XDG_DATA_HOME": str(data_root),
            "XDG_CONFIG_HOME": str(xdg_root / "config"),
            "XDG_CACHE_HOME": str(xdg_root / "cache"),
            "EDEN_PERF": "1",
            "EDEN_AUTOMATION": "1",
            "EDEN_AUTOMATION_SOCKET": str(socket_path),
        }
    )
    environment.pop("QT_QPA_PLATFORM", None)
    environment.pop("EDEN_CEF_SHARED_TEXTURE_PROBE", None)
    environment.pop("EDEN_AUTOMATION_ENGINE_STAGE", None)
    environment.pop("EDEN_IDLE_DIAGNOSTICS", None)
    if engine_stage:
        environment["EDEN_AUTOMATION_ENGINE_STAGE"] = engine_stage
    if scenario_name == "idle-stage":
        environment["EDEN_IDLE_DIAGNOSTICS"] = "1"
    if scenario_name == "frames":
        environment["EDEN_BENCH_FRAMES"] = "1"
    arguments = [
        str(binary),
        "--engine=cef",
        "--no-sandbox",
        "--disable-background-networking",
        "--disable-component-update",
        "--disable-sync",
        "--autoplay-policy=no-user-gesture-required",
    ]
    if mode == "windowed":
        arguments.append("--engine-compositing=windowed")
        environment.pop("WAYLAND_DISPLAY", None)
    elif mode == "accelerated-probe":
        arguments.extend(["--engine-compositing=osr", "--ozone-platform=x11", "--use-angle=gl-egl"])
        environment["EDEN_CEF_SHARED_TEXTURE_PROBE"] = "1"
    log_path = xdg_root / "browser.log"
    with log_path.open("w") as output:
        browser = subprocess.Popen(arguments, stdout=output, stderr=subprocess.STDOUT, env=environment, start_new_session=True)
        process = browser.pid
        driver_result = None
        video_elapsed = None
        client = AutomationClient(socket_path)
        startup_values = []
        rpc_samples = []
        try:
            client.connect(seconds)
            startup_sequence = 0
            value, startup_sequence = client.wait_metric("startup.shell_first_frame_ms", startup_sequence, seconds)
            startup_values.append(("startup.shell_first_frame_ms", value))
            if scenario_name == "startup" and mode == "osr":
                value, startup_sequence = client.wait_metric("startup.web_first_paint_ms", startup_sequence, seconds)
                startup_values.append(("startup.web_first_paint_ms", value))
            elif scenario_name == "idle-stage":
                if engine_stage == "cef":
                    unused, startup_sequence = client.wait_metric("startup.stage.cef_initialized_ms", startup_sequence, seconds)
                elif not engine_stage:
                    unused, startup_sequence = client.wait_metric("page.ready", startup_sequence, seconds)
            elif engine_stage == "cef":
                unused, startup_sequence = client.wait_metric("startup.stage.cef_initialized_ms", startup_sequence, seconds)
            elif engine_stage:
                pass
            elif scenario_name != "startup":
                unused, startup_sequence = client.wait_metric("page.ready", startup_sequence, seconds)
                if scenario_name in {"input", "scroll"}:
                    unused, startup_sequence = client.wait_metric("page.script_ready", startup_sequence, seconds)
            rpc_samples = client.call("dumpMetrics", {"afterSequence": 0}).get("samples", [])
            if scenario_name == "tabswitch":
                for index in range(1, tabs):
                    client.call("activateTab", {"index": index})
                    client.wait_metric_count("page.ready", index + 1, seconds)
                warm_sequence = client.sequence()
                client.call("activateTab", {"index": 0})
                unused, warm_sequence = client.wait_metric("tabswitch.input_to_frame_ms", warm_sequence)
                sequence = client.sequence()
                values = []
                for sample in range(21):
                    if sample > 0:
                        time.sleep(phase_delay_seconds(sample - 1))
                    client.call("activateTab", {"index": (sample + 1) % tabs})
                    value, sequence = client.wait_metric("tabswitch.input_to_frame_ms", sequence)
                    values.append(value)
                driver_result = subprocess.CompletedProcess([], 0, "\n".join(f"tabswitch={value}" for value in values), "")
            elif scenario_name == "input":
                sequence = client.sequence()
                values = []
                for sample in range(21):
                    if sample > 0:
                        time.sleep(phase_delay_seconds(sample - 1))
                    client.call("typeInOmnibox", {"text": chr(ord("a") + sample % 26)})
                    value, sequence = client.wait_metric("input.key_to_frame_ms", sequence)
                    values.append(value)
                driver_result = subprocess.CompletedProcess([], 0, "\n".join(f"input={value}" for value in values), "")
            elif scenario_name == "frames":
                sequence = client.sequence()
                values = []
                client.call("beginFrameCollection")
                for sample in range(120):
                    source = sample % 2
                    client.call("reorderTab", {"from": source, "to": 1 - source})
                    value, sequence = client.wait_metric("reorder.input_to_frame_ms", sequence)
                    values.append(value)
                time.sleep(0.3)
                client.call("endFrameCollection")
                driver_result = subprocess.CompletedProcess([], 0, "\n".join(f"reorder={value}" for value in values), "")
            elif scenario_name == "scroll":
                sequence = client.sequence()
                values = []
                frame_values = []
                for sample in range(20):
                    started_sequence = sequence
                    client.call("wheel", {"delta": -120})
                    value, page_sequence = client.wait_metric("scroll.input_to_page_event_ms", started_sequence)
                    values.append(value)
                    sequence = page_sequence
                    if mode == "osr":
                        frame_value, frame_sequence = client.wait_metric("scroll.input_to_frame_ms", started_sequence)
                        frame_values.append(frame_value)
                        sequence = max(sequence, frame_sequence)
                driver_result = subprocess.CompletedProcess(
                    [],
                    0,
                    "\n".join(
                        [*(f"scroll={value}" for value in values), *(f"scroll_frame={value}" for value in frame_values)]
                    ),
                    "",
                )
            elif scenario_name == "video":
                sequence = client.sequence()
                unused, sequence = client.wait_metric("video.frame", sequence, seconds)
                video_started = time.monotonic()
                time.sleep(10)
                video_elapsed = time.monotonic() - video_started
                driver_result = subprocess.CompletedProcess([], 0, "", "")
            elif scenario_name == "memory":
                time.sleep(4)
                rollup, mappings = shell_memory_details(process)
                shell_private = rollup.get("Private_Clean", 0.0) + rollup.get("Private_Dirty", 0.0)
                driver_result = subprocess.CompletedProcess(
                    [],
                    0,
                    f"shell_private_mb={shell_private:.3f}\nshell_rss_mb={shell_rss(process):.3f}\n"
                    f"process_tree_rss_mb={process_tree_rss(process):.3f}\n"
                    + "".join(f"smaps_{name.lower()}_mb={value:.3f}\n" for name, value in sorted(rollup.items()))
                    + "".join(f"mapping_{name}_rss_mb={value:.3f}\n" for name, value in sorted(mappings.items())),
                    "",
                )
            elif scenario_name in {"idle", "idle-stage"}:
                time.sleep(4)
                cpu_percent, ticks, threads = cpu_sample(process, idle_seconds)
                driver_result = subprocess.CompletedProcess(
                    [],
                    0,
                    f"shell_cpu_percent={cpu_percent:.6f}\nshell_cpu_ticks={ticks}\n"
                    + "".join(
                        f"thread tid={entry['tid']} name={entry['name']} utime_ticks={entry['utime_ticks']} stime_ticks={entry['stime_ticks']}\n"
                        for entry in threads
                    ),
                    "",
                )
            else:
                driver_result = subprocess.CompletedProcess([], 0, "\n".join(f"{name}={value}" for name, value in startup_values), "")
            rpc_samples = client.call("dumpMetrics", {"afterSequence": 0}).get("samples", [])
        except (subprocess.TimeoutExpired, OSError, TimeoutError, RuntimeError, ConnectionError) as error:
            driver_result = subprocess.CompletedProcess([], 124, "", str(error))
        finally:
            try:
                client.call("quit")
            except (OSError, RuntimeError, ConnectionError, AttributeError):
                pass
            client.close()
            if browser.poll() is None:
                try:
                    browser.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(browser.pid, signal.SIGTERM)
                    try:
                        browser.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        os.killpg(browser.pid, signal.SIGKILL)
                        browser.wait(timeout=2)
    text = log_path.read_text(errors="replace")
    shutil.rmtree(socket_root, ignore_errors=True)
    if driver_result and driver_result.returncode != 0:
        diagnostic_lines = text.splitlines()[-40:]
        driver_result = subprocess.CompletedProcess(
            driver_result.args,
            driver_result.returncode,
            driver_result.stdout,
            "\n".join(value for value in (driver_result.stderr, *diagnostic_lines[-40:]) if value),
        )
    performance = parse_perf(text)
    rpc_metrics = {}
    for sample in rpc_samples:
        rpc_metrics.setdefault(sample["name"], []).append(float(sample["value"]))
    performance.update(rpc_metrics)
    if scenario_name == "video" and driver_result:
        frame_numbers = performance.get("video.frame", [])
        if len(frame_numbers) >= 2 and video_elapsed:
            driver_result = subprocess.CompletedProcess(
                [], 0, f"video_fps={(frame_numbers[-1] - frame_numbers[0]) / video_elapsed:.3f}\n", ""
            )
    if driver_result and scenario_name == "startup":
        for name, value in startup_values:
            performance.setdefault(name, [value])
    return {
        "returncode": driver_result.returncode if driver_result else browser.returncode,
        "driver": driver_result.stdout if driver_result else "",
        "driver_error": driver_result.stderr if driver_result else "",
        "log": text,
        "perf": performance,
        "gpu_failures": [line for line in text.splitlines() if GPU_FAILURE_PATTERN.search(line)],
        "accelerated_callbacks": len(re.findall(r"osr\.accelerated_paint", text)),
    }


def warm(binary):
    with binary.open("rb") as source:
        while source.read(1024 * 1024):
            pass
    cef = binary.parent / "libcef.so"
    if cef.exists():
        with cef.open("rb") as source:
            while source.read(4 * 1024 * 1024):
                pass


def collect_startup(binary, mode, page_uri, temp_root, iterations):
    values = {"startup.shell_first_frame_ms": [], "startup.web_first_paint_ms": []}
    stages = {}
    failures = []
    for index in range(iterations + 1):
        warm(binary)
        result = run_eden_case(binary, mode, "startup", page_uri, temp_root, seconds=20)
        for metric in values:
            if index > 0 and result["perf"].get(metric):
                values[metric].append(result["perf"][metric][0])
        for metric, samples in result["perf"].items():
            if metric.startswith("startup.stage."):
                stages.setdefault(metric, []).extend(samples)
        failures.extend(result["gpu_failures"])
    return values, failures, stages


def collect_scenario(binary, mode, scenario_name, page_uri, temp_root, iterations, tabs=1, engine_stage=None):
    merged = {}
    drivers = []
    gpu_failures = []
    callbacks = 0
    driver_errors = []
    returncodes = []
    run_metrics = []
    for index in range(iterations):
        result = run_eden_case(binary, mode, scenario_name, page_uri, temp_root, tabs=tabs, seconds=45, engine_stage=engine_stage)
        run_metrics.append(result["perf"])
        for metric, values in result["perf"].items():
            merged.setdefault(metric, []).extend(values)
        drivers.append(result["driver"])
        driver_errors.append(result["driver_error"])
        returncodes.append(result["returncode"])
        gpu_failures.extend(result["gpu_failures"])
        callbacks += result["accelerated_callbacks"]
    return merged, drivers, driver_errors, returncodes, gpu_failures, callbacks, run_metrics


def metric_row(name, values):
    measured = summary(values)
    baseline = BASELINE.get(name)
    regression = math.nan if not baseline or math.isnan(measured["p50"]) else 100.0 * (measured["p50"] - baseline) / baseline
    row = {"metric": name, **measured, "budget": BOUNDARIES[name], "regression_percent": regression}
    if name == "frame.interval_ms":
        row["late_60hz_intervals"] = sum(value > 16.667 for value in values)
        row["dropped_60hz_frames"] = sum(value > 25.0005 for value in values)
        row["minimum_interval_ms"] = min(values) if values else math.nan
    return row


def evaluate(row):
    name = row["metric"]
    if not row["samples"]:
        return False
    if name == "idle.shell_cpu_percent":
        return row["p95"] <= 0.0
    if name == "frame.interval_ms":
        return (
            row["p95"] <= row["budget"]
            and row.get("dropped_60hz_frames", 1) == 0
            and row.get("minimum_interval_ms", 1000.0) <= 8.3
        )
    return row["p95"] <= row["budget"]


def print_rows(rows):
    print("metric                                n       p50       p95       p99     budget   regression   status")
    for row in rows:
        status = "PASS" if evaluate(row) else "FAIL"
        regression = "n/a" if math.isnan(row["regression_percent"]) else f"{row['regression_percent']:+.1f}%"
        print(
            f"{row['metric']:<36} {row['samples']:>3} {row['p50']:>9.3f} {row['p95']:>9.3f} "
            f"{row['p99']:>9.3f} {row['budget']:>9.3f} {regression:>12} {status:>7}"
        )


class CdpPipe:
    def __init__(self, write_descriptor, read_descriptor):
        self.write_descriptor = write_descriptor
        self.read_descriptor = read_descriptor
        self.next_id = 1
        self.buffer = b""

    def call(self, method, parameters=None, session_id=None, timeout=20):
        identifier = self.next_id
        self.next_id += 1
        message = {"id": identifier, "method": method}
        if parameters:
            message["params"] = parameters
        if session_id:
            message["sessionId"] = session_id
        os.write(self.write_descriptor, json.dumps(message, separators=(",", ":")).encode() + b"\0")
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            response = self.receive(deadline - time.monotonic())
            if response.get("id") != identifier:
                continue
            if "error" in response:
                raise RuntimeError(response["error"].get("message", "CDP command failed"))
            return response.get("result", {})
        raise TimeoutError(f"CDP command timed out: {method}")

    def receive(self, timeout):
        while b"\0" not in self.buffer:
            readable, unused_write, unused_error = select.select([self.read_descriptor], [], [], max(0.0, timeout))
            if not readable:
                raise TimeoutError("CDP pipe read timed out")
            chunk = os.read(self.read_descriptor, 65536)
            if not chunk:
                raise ConnectionError("CDP pipe closed")
            self.buffer += chunk
        message, self.buffer = self.buffer.split(b"\0", 1)
        return json.loads(message)

    def close(self):
        for descriptor in (self.write_descriptor, self.read_descriptor):
            try:
                os.close(descriptor)
            except OSError:
                pass


def cdp_title(cdp, session_id):
    result = cdp.call(
        "Runtime.evaluate",
        {"expression": "document.title", "returnByValue": True},
        session_id,
    )
    return result.get("result", {}).get("value", "")


def wait_cdp_title(cdp, session_id, prefix, timeout=20):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = cdp_title(cdp, session_id)
        if value.startswith(prefix):
            return value
        time.sleep(0.01)
    raise TimeoutError(f"page title did not reach {prefix}")


def run_external_reference(page_uri, video_uri, temp_root, unused_driver):
    results = []
    for executable_name in ("google-chrome-stable", "chromium"):
        executable = shutil.which(executable_name)
        if not executable:
            continue
        for name, uri in (("scroll", page_uri), ("video", video_uri)):
            profile = tempfile.mkdtemp(prefix=f"eden-bench-{executable_name}-{name}-", dir=temp_root)
            environment = os.environ.copy()
            environment.pop("WAYLAND_DISPLAY", None)
            to_browser_read, to_browser_write = os.pipe()
            from_browser_read, from_browser_write = os.pipe()

            process = subprocess.Popen(
                [
                    "/bin/sh",
                    "-c",
                    'exec 3<&"$1" 4>&"$2"; shift 2; exec "$@"',
                    "eden-cdp-launch",
                    str(to_browser_read),
                    str(from_browser_write),
                    executable,
                    "--no-sandbox",
                    "--no-first-run",
                    "--no-default-browser-check",
                    "--disable-background-networking",
                    "--disable-component-update",
                    "--disable-session-crashed-bubble",
                    "--new-window",
                    "--autoplay-policy=no-user-gesture-required",
                    "--ozone-platform=x11",
                    "--remote-debugging-pipe",
                    f"--user-data-dir={profile}",
                    uri,
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                env=environment,
                start_new_session=True,
                pass_fds=(to_browser_read, from_browser_write),
            )
            os.close(to_browser_read)
            os.close(from_browser_write)
            cdp = CdpPipe(to_browser_write, from_browser_read)
            driver_returncode = 0
            driver_stdout = ""
            driver_stderr = ""
            try:
                deadline = time.monotonic() + 20
                target_id = ""
                while time.monotonic() < deadline and not target_id:
                    target_result = cdp.call("Target.getTargets")
                    for target in target_result.get("targetInfos", []):
                        if target.get("type") == "page" and target.get("url") == uri:
                            target_id = target.get("targetId", "")
                            break
                    if not target_id:
                        time.sleep(0.01)
                if not target_id:
                    raise TimeoutError("system browser target did not appear")
                session_id = cdp.call("Target.attachToTarget", {"targetId": target_id, "flatten": True}).get("sessionId", "")
                if not session_id:
                    raise RuntimeError("system browser target did not attach")
                if name == "scroll":
                    wait_cdp_title(cdp, session_id, "bench:ready")
                    values = []
                    for sample in range(20):
                        before = cdp_title(cdp, session_id)
                        started = time.monotonic()
                        cdp.call(
                            "Input.dispatchMouseEvent",
                            {"type": "mouseWheel", "x": 700, "y": 500, "deltaX": 0, "deltaY": 120, "pointerType": "mouse"},
                            session_id,
                        )
                        deadline = started + 2
                        after = cdp_title(cdp, session_id)
                        while time.monotonic() < deadline and after == before:
                            time.sleep(0.00025)
                            after = cdp_title(cdp, session_id)
                        if after == before or not after.startswith("bench:"):
                            raise TimeoutError("system browser wheel event did not reach the page")
                        values.append((time.monotonic() - started) * 1000)
                        time.sleep(0.008)
                    driver_stdout = "\n".join(f"scroll={value:.6f}" for value in values) + "\n"
                else:
                    before_title = wait_cdp_title(cdp, session_id, "video:frames:")
                    before = int(before_title.removeprefix("video:frames:"))
                    started = time.monotonic()
                    time.sleep(10)
                    elapsed = time.monotonic() - started
                    after_title = cdp_title(cdp, session_id)
                    after = int(after_title.removeprefix("video:frames:"))
                    driver_stdout = f"video_fps={(after - before) / elapsed:.3f} frames={after - before}\n"
            except (OSError, TimeoutError, RuntimeError, ConnectionError, ValueError, json.JSONDecodeError) as error:
                driver_returncode = 124
                driver_stderr = str(error)
            finally:
                cdp.close()
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
            try:
                output, unused = process.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                output, unused = process.communicate(timeout=2)
            fps_values = [float(value) for value in re.findall(r"video_fps=([0-9.]+)", driver_stdout)]
            scroll_values = [float(value) for value in re.findall(r"scroll=([0-9.]+)", driver_stdout)]
            results.append(
                {
                    "browser": executable_name,
                    "page": name,
                    "driver_returncode": driver_returncode,
                    "driver_stdout": driver_stdout,
                    "driver_stderr": driver_stderr,
                    "browser_output_tail": output.splitlines()[-20:],
                    "scroll_p95_ms": summary(scroll_values)["p95"],
                    "video_fps": fps_values[0] if fps_values else None,
                    "gpu_failure_signatures": [line for line in output.splitlines() if GPU_FAILURE_PATTERN.search(line)],
                }
            )
    return results


def arguments():
    parser = argparse.ArgumentParser()
    parser.add_argument("--all", action="store_true")
    parser.add_argument("--startup", action="store_true")
    parser.add_argument("--tabswitch", action="store_true")
    parser.add_argument("--input", action="store_true")
    parser.add_argument("--frames", action="store_true")
    parser.add_argument("--memory", action="store_true")
    parser.add_argument("--idle", action="store_true")
    parser.add_argument("--idle-stages", action="store_true")
    parser.add_argument("--e37-matrix", action="store_true")
    parser.add_argument("--matrix-mode", action="append", choices=("osr", "windowed", "accelerated-probe", "system"))
    parser.add_argument("--matrix-skip-video", action="store_true")
    parser.add_argument("--binary", type=pathlib.Path, default=ROOT / "build-release" / "eden-browser")
    parser.add_argument("--iterations", type=int, default=20)
    parser.add_argument("--output", type=pathlib.Path)
    return parser.parse_args()


def main():
    options = arguments()
    binary = options.binary.resolve()
    if not binary.exists():
        print(f"release binary not found: {binary}", file=sys.stderr)
        return 2
    if not os.access(binary, os.X_OK):
        print(f"binary is not executable: {binary}", file=sys.stderr)
        return 2
    selected = {
        "startup": options.startup,
        "tabswitch": options.tabswitch,
        "input": options.input,
        "frames": options.frames,
        "memory": options.memory,
        "idle": options.idle,
    }
    if options.all or (not any(selected.values()) and not options.e37_matrix and not options.idle_stages):
        selected = {name: True for name in selected}
    with tempfile.TemporaryDirectory(prefix="eden-bench-") as temporary:
        temp_root = pathlib.Path(temporary)
        driver = compile_native(temp_root)
        page_root = temp_root / "pages"
        shutil.copytree(SCRIPT_ROOT / "pages", page_root)
        video_available = generate_video(page_root)
        interaction_uri = (page_root / "interaction.html").as_uri()
        video_uri = (page_root / "video.html").as_uri()
        static_uri = (page_root / "static.html").as_uri()
        rows = []
        details = {
            "binary": str(binary),
            "build_type": "Release",
            "warm_disk": True,
            "iterations": options.iterations,
            "shipped_mode": "osr-onpaint-transport",
            "video_fixture": "generated VP9 3840x2160 at 60 fps" if video_available else "unavailable: ffmpeg missing",
            "speedometer3": "unavailable: no local checkout and downloads prohibited",
            "youtube_4k60": "unavailable: network access prohibited",
        }
        if options.idle_stages:
            stage_results = {}
            for stage_name, engine_stage in (("shell", "shell"), ("cef", "cef"), ("browser", None)):
                result = run_eden_case(
                    binary,
                    "osr",
                    "idle-stage",
                    static_uri,
                    temp_root,
                    seconds=45,
                    engine_stage=engine_stage,
                    idle_seconds=60,
                )
                values = [float(value) for value in re.findall(r"shell_cpu_percent=([0-9.]+)", result["driver"])]
                stage_results[stage_name] = {
                    "cpu_percent": values[0] if values else None,
                    "driver_output": result["driver"],
                    "driver_error": result["driver_error"],
                    "returncode": result["returncode"],
                    "gpu_failure_signatures": result["gpu_failures"],
                    "accelerated_callbacks": result["accelerated_callbacks"],
                    "osr_paints": result["perf"].get("diagnostic.osr_paints", []),
                    "shell_frame_swaps": result["perf"].get("diagnostic.shell_frame_swaps", []),
                }
            details["idle_stages"] = stage_results
        if selected["startup"]:
            metrics, failures, stages = collect_startup(binary, "osr", interaction_uri, temp_root, options.iterations)
            rows.extend(metric_row(name, values) for name, values in metrics.items())
            details["startup_gpu_failures"] = len(failures)
            details["startup_stages_ms"] = {name: summary(values) for name, values in stages.items()}
        for name, metric, tabs in (
            ("tabswitch", "tabswitch.input_to_frame_ms", 10),
            ("input", "input.key_to_frame_ms", 1),
            ("frames", "frame.interval_ms", 10),
            ("memory", "memory.shell_private_mb", 1),
            ("idle", "idle.shell_cpu_percent", 1),
        ):
            if not selected[name]:
                continue
            scenario_uri = static_uri if name in {"memory", "idle"} else interaction_uri
            metrics, driver_text, driver_errors, returncodes, failures, callbacks, run_metrics = collect_scenario(
                binary, "osr", name, scenario_uri, temp_root, options.iterations, tabs=tabs, engine_stage="shell" if name == "memory" else None
            )
            values = list(metrics.get(metric, []))
            if name in {"tabswitch", "input"}:
                key = "tabswitch" if name == "tabswitch" else "input"
                measured_runs = measured_driver_runs(driver_text, key)
                values = [value for run in measured_runs for value in run]
                details[f"{name}_measured_runs_ms"] = measured_runs
                details[f"{name}_phase_delays_ms"] = [phase_delay_seconds(index) * 1000.0 for index in range(PHASE_SAMPLE_COUNT)]
            if name in {"memory", "idle"}:
                pattern = re.compile(rf"{'shell_private_mb' if name == 'memory' else 'shell_cpu_percent'}=([0-9.]+)")
                values = [float(value) for text in driver_text for value in pattern.findall(text)]
            rows.append(metric_row(metric, values))
            if name == "frames":
                details["frames_interval_values_ms"] = values
                details["frames_interval_runs_ms"] = [run.get(metric, []) for run in run_metrics]
            details[f"{name}_gpu_failures"] = len(failures)
            details[f"{name}_accelerated_callbacks"] = callbacks
            details[f"{name}_driver_output"] = driver_text
            details[f"{name}_driver_errors"] = driver_errors
            details[f"{name}_returncodes"] = returncodes
            if name == "memory":
                diagnostic_metrics, diagnostic_output, diagnostic_errors, diagnostic_returncodes, diagnostic_failures, unused_callbacks, unused_runs = (
                    collect_scenario(binary, "osr", "memory", static_uri, temp_root, 1)
                )
                details["memory_engine_loaded_driver_output"] = diagnostic_output
                details["memory_engine_loaded_driver_errors"] = diagnostic_errors
                details["memory_engine_loaded_returncodes"] = diagnostic_returncodes
                details["memory_engine_loaded_gpu_failures"] = diagnostic_failures
                details["memory_after_cef_init_rss_mb"] = diagnostic_metrics.get("memory.after_cef_init_rss_mb", [])
                details["memory_engine_loaded_shell_rss_mb"] = [
                    float(value) for text in diagnostic_output for value in re.findall(r"shell_rss_mb=([0-9.]+)", text)
                ]
        if options.all or options.e37_matrix:
            matrix = {}
            matrix_modes = options.matrix_mode or ("osr", "windowed", "accelerated-probe", "system")
            for mode in (candidate for candidate in ("osr", "windowed", "accelerated-probe") if candidate in matrix_modes):
                scenario_metrics, driver_text, driver_errors, returncodes, failures, callbacks, unused_runs = collect_scenario(
                    binary, mode, "scroll", interaction_uri, temp_root, 1
                )
                video_metrics = {}
                video_drivers = []
                video_failures = []
                video_callbacks = 0
                if video_available and not options.matrix_skip_video:
                    video_metrics, video_drivers, video_driver_errors, video_returncodes, video_failures, video_callbacks, unused_video_runs = collect_scenario(
                        binary, mode, "video", video_uri, temp_root, 1
                    )
                fps_values = [float(value) for text in video_drivers for value in re.findall(r"video_fps=([0-9.]+)", text)]
                matrix[mode] = {
                    "scroll_samples": len(scenario_metrics.get("scroll.input_to_page_event_ms", [])),
                    "scroll_p95_ms": summary(scenario_metrics.get("scroll.input_to_page_event_ms", []))["p95"],
                    "input_to_frame_samples": len(scenario_metrics.get("scroll.input_to_frame_ms", [])),
                    "input_to_frame_p95_ms": summary(scenario_metrics.get("scroll.input_to_frame_ms", []))["p95"],
                    "video_fps": fps_values[0] if fps_values else None,
                    "gpu_failure_signatures": failures + video_failures,
                    "accelerated_callbacks": callbacks + video_callbacks,
                    "scroll_driver_errors": driver_errors,
                    "scroll_returncodes": returncodes,
                    "video_driver_errors": video_driver_errors if video_available and not options.matrix_skip_video else [],
                    "video_returncodes": video_returncodes if video_available and not options.matrix_skip_video else [],
                }
            if "system" in matrix_modes and not options.matrix_skip_video:
                matrix["system_chrome"] = run_external_reference(interaction_uri, video_uri, temp_root, driver)
            matrix_errors = []
            if "osr" in matrix_modes:
                osr_result = matrix.get("osr", {})
                if osr_result.get("scroll_samples") != 20 or any(osr_result.get("scroll_returncodes", [])):
                    matrix_errors.append("OSR scroll samples are incomplete")
                if osr_result.get("input_to_frame_samples") != 20:
                    matrix_errors.append("OSR input-to-frame samples are incomplete")
                if osr_result.get("gpu_failure_signatures"):
                    matrix_errors.append("OSR logged a GPU process failure")
                if not options.matrix_skip_video and (osr_result.get("video_fps") is None or osr_result["video_fps"] < 55.0):
                    matrix_errors.append("OSR did not sustain the full-rate video threshold")
                osr_result["health"] = "healthy" if not osr_result.get("gpu_failure_signatures") else "degraded"
            if "windowed" in matrix_modes:
                windowed_result = matrix.get("windowed", {})
                if windowed_result.get("scroll_samples") != 20 or any(windowed_result.get("scroll_returncodes", [])):
                    matrix_errors.append("Windowed control scroll samples are incomplete")
                if not options.matrix_skip_video and windowed_result.get("video_fps") is None:
                    matrix_errors.append("Windowed control video sample is missing")
                if windowed_result.get("gpu_failure_signatures"):
                    matrix_errors.append("Windowed control logged a GPU process failure")
                windowed_result["health"] = "degraded" if windowed_result.get("gpu_failure_signatures") else "healthy"
            if "accelerated-probe" in matrix_modes:
                probe_result = matrix.get("accelerated-probe", {})
                if probe_result.get("accelerated_callbacks", 0) > 0:
                    probe_result["availability"] = "available"
                else:
                    probe_result["availability"] = "unavailable"
                    probe_result["unavailable_reason"] = "Pinned CEF produced no Linux OnAcceleratedPaint callbacks under x11 and gl-egl"
            if "system" in matrix_modes and not options.matrix_skip_video:
                references = matrix.get("system_chrome", [])
                grouped_references = {}
                for reference in references:
                    grouped_references.setdefault(reference["browser"], {})[reference["page"]] = reference
                healthy_reference = any(
                    pages.get("scroll", {}).get("driver_returncode") == 0
                    and pages.get("scroll", {}).get("scroll_p95_ms") is not None
                    and not math.isnan(pages.get("scroll", {}).get("scroll_p95_ms", math.nan))
                    and pages.get("video", {}).get("driver_returncode") == 0
                    and pages.get("video", {}).get("video_fps") is not None
                    and not pages.get("scroll", {}).get("gpu_failure_signatures")
                    and not pages.get("video", {}).get("gpu_failure_signatures")
                    for pages in grouped_references.values()
                )
                if not healthy_reference:
                    matrix_errors.append("No healthy system browser reference completed")
            details["e37_matrix_valid"] = not matrix_errors
            details["e37_matrix_errors"] = matrix_errors
            details["e37_matrix"] = matrix
        print_rows(rows)
        output = json_ready({"metrics": rows, "details": details})
        print(json.dumps(output["details"], indent=2, sort_keys=True, allow_nan=False))
        if options.output:
            options.output.parent.mkdir(parents=True, exist_ok=True)
            options.output.write_text(json.dumps(output, indent=2, sort_keys=True, allow_nan=False) + "\n")
        return 1 if any(not evaluate(row) for row in rows) or details.get("e37_matrix_valid") is False else 0


if __name__ == "__main__":
    raise SystemExit(main())
