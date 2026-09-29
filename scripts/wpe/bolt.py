from contextlib import contextmanager
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import signal
import stat
import struct
import subprocess
import tempfile
import time

from wpe.performance_patches import names as patch_names, resolve_patches
from wpe.pgo import profile_identity


STAGES = ("instrument", "train", "merge", "optimize", "validate", "validate-performance", "promote", "rollback")
WAIVABLE_VALIDATION = ("baseline", "browsing", "layout")
SPEEDOMETER_ITERATIONS = 10
SPEEDOMETER_SUITE_COUNT = 20
SPEEDOMETER_VIEWPORT = [1600, 1200, 1]
BROWSING_MINIMUM_SECONDS = 3600
BROWSING_MINIMUM_VISITS = 120
DEFAULT_MINIMUM_FUNCTION_COVERAGE_PERCENT = 5.0
MINIMUM_FUNCTION_COVERAGE_PERCENT = 1.0
INTERPRETER_SKIP_FUNCTIONS = (
    "(ipint_|llint_|op_|js_|mint_|uint_|argumINT_|wasm_|checkpoint_|fuzzer_|loop_|normal_|array_|"
    "relocate_|get_|jsc_llint_begin|jsc_llint_end|llintPCRange|wasmIPIntPCRange|vmEntry|"
    "sanitizeStackForVMImpl|enterWebAssemblySuspendingFunction|pinballHandler|exit_|"
    "relocateJITReturnPC|getSentinelFrameReturnPC).*"
)


def verify_interpreter_skip_symbols(symbols):
    functions = []
    for line in symbols.splitlines():
        parts = line.split(maxsplit=2)
        if len(parts) != 3 or parts[1].lower() not in {"t", "w"}:
            continue
        try:
            address = int(parts[0], 16)
        except ValueError:
            continue
        functions.append((address, parts[2]))
    begins = [address for address, name in functions if name == "jsc_llint_begin"]
    ends = [address for address, name in functions if name == "jsc_llint_end"]
    if len(begins) != 1 or len(ends) != 1 or begins[0] >= ends[0]:
        raise RuntimeError("The JSC interpreter symbol boundaries are missing or invalid")
    protected = [name for address, name in functions if begins[0] <= address <= ends[0]]
    if len(protected) < 100:
        raise RuntimeError("The JSC interpreter symbol table is incomplete")
    missing = [name for name in protected if not re.fullmatch(INTERPRETER_SKIP_FUNCTIONS, name)]
    if missing:
        raise RuntimeError(f"BOLT would rewrite JSC interpreter symbols: {missing[:8]}")
    return len(protected)


def require_interpreter_skip_coverage(library):
    symbols = subprocess.check_output(["nm", "-n", "--defined-only", str(library)], text=True)
    return verify_interpreter_skip_symbols(symbols)


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def sync_directory(path):
    descriptor = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def write_json(path, value):
    descriptor, name = tempfile.mkstemp(prefix=path.name + ".", dir=path.parent)
    temporary = Path(name)
    try:
        with os.fdopen(descriptor, "w") as stream:
            json.dump(value, stream, indent=2, allow_nan=False)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        temporary.replace(path)
        sync_directory(path.parent)
    finally:
        temporary.unlink(missing_ok=True)


def atomic_copy(source, destination):
    descriptor, name = tempfile.mkstemp(prefix=destination.name + ".", dir=destination.parent)
    temporary = Path(name)
    try:
        with source.open("rb") as incoming, os.fdopen(descriptor, "wb") as outgoing:
            shutil.copyfileobj(incoming, outgoing)
            outgoing.flush()
            os.fchmod(outgoing.fileno(), stat.S_IMODE(source.stat().st_mode))
            os.fsync(outgoing.fileno())
        temporary.replace(destination)
        sync_directory(destination.parent)
    finally:
        temporary.unlink(missing_ok=True)


def elf_sections(path):
    with path.open("rb") as stream:
        header = stream.read(64)
        if len(header) != 64 or header[:6] != b"\x7fELF\x02\x01":
            raise RuntimeError("BOLT requires a little-endian ELF64 library")
        values = struct.unpack("<16sHHIQQQIHHHHHH", header)
        if values[1:3] != (3, 62):
            raise RuntimeError("The WPE BOLT pipeline requires an x86-64 shared library")
        offset, width, count, names_index = values[6], values[11], values[12], values[13]
        size = path.stat().st_size
        if width != 64 or not count or names_index >= count or offset + width * count > size:
            raise RuntimeError("Invalid or unsupported ELF section table")
        stream.seek(offset)
        sections = [struct.unpack("<IIQQQQIIQQ", stream.read(64)) for _ in range(count)]
        names = sections[names_index]
        if names[4] + names[5] > size or names[5] > 16 * 1024 * 1024:
            raise RuntimeError("Invalid ELF section names")
        stream.seek(names[4])
        strings = stream.read(names[5])
        result = set()
        for section in sections:
            start = section[0]
            end = strings.find(b"\0", start)
            if start >= len(strings) or end < 0:
                raise RuntimeError("Invalid ELF section name offset")
            result.add(strings[start:end].decode("ascii"))
        return result


def require_non_executable_stack(path):
    with path.open("rb") as stream:
        header = stream.read(64)
        if len(header) != 64 or header[:6] != b"\x7fELF\x02\x01":
            raise RuntimeError("BOLT requires a little-endian ELF64 library")
        values = struct.unpack("<16sHHIQQQIHHHHHH", header)
        offset, width, count = values[5], values[9], values[10]
        if width != 56 or not count or offset + width * count > path.stat().st_size:
            raise RuntimeError("Invalid ELF program header table")
        stream.seek(offset)
        for _ in range(count):
            program = struct.unpack("<IIQQQQQQ", stream.read(width))
            if program[0] == 0x6474E551:
                if program[1] & 1:
                    raise RuntimeError("The BOLT library requires an executable stack")
                return
    raise RuntimeError("The BOLT library has no non-executable GNU_STACK program header")


def require_original(path):
    sections = elf_sections(path)
    if not {".symtab", ".rela.text"}.issubset(sections):
        raise RuntimeError("Rebuild the unstripped performance library with --emit-relocs before BOLT")
    if any("bolt" in name.lower() for name in sections):
        raise RuntimeError("The original library has already been processed by BOLT")
    require_non_executable_stack(path)


def resolve_tool(root, directory, name):
    if directory:
        candidates = [directory / name]
    else:
        executable = shutil.which(name)
        candidates = [Path(executable)] if executable else []
        candidates += sorted((root / "third_party/wpe").glob(f"llvm-*/bin/{name}"), reverse=True)
    for candidate in candidates:
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate.resolve()
    raise RuntimeError(f"BOLT requires an executable {name}; pass --bolt-tool-dir")


def coverage_from_log(log, minimum_coverage_percent=None):
    counts = [int(value.replace(",", "")) for value in re.findall(
        r"^\s*([\d,]+)\s*:\s*executed instructions(?:\s|$)", log, re.MULTILINE
    )]
    if not counts or min(counts) < 1_000_000_000:
        raise RuntimeError("BOLT dyno-stats must cover at least one billion executed instructions")
    if re.search(r"BOLT-(?:ERROR|WARNING):.*(?:profile|data)", log, re.IGNORECASE):
        raise RuntimeError("BOLT reported invalid, stale or incomplete profile data; inspect the retained log")
    stale = re.findall(r"([\d,]+)\s+functions?\s+.*?stale", log, re.IGNORECASE)
    if any(int(value.replace(",", "")) for value in stale):
        raise RuntimeError("BOLT reported stale functions; retrain against the exact input library")
    functions = re.search(r"BOLT-INFO: (\d+) out of (\d+) functions in the binary "
                          r"\(([\d.]+)%\) have non-empty execution profile", log)
    if not functions or not 0 < int(functions[1]) <= int(functions[2]):
        raise RuntimeError("BOLT did not report coverage of native functions")
    profiled_functions = int(functions[1])
    total_functions = int(functions[2])
    reported_percent = float(functions[3])
    actual_percent = 100.0 * profiled_functions / total_functions
    if not math.isfinite(reported_percent) or abs(reported_percent - actual_percent) > 0.051:
        raise RuntimeError("BOLT reported an inconsistent native function coverage percentage")
    if minimum_coverage_percent is None:
        configured = os.environ.get("EDEN_WPE_BOLT_MINIMUM_FUNCTION_COVERAGE_PERCENT")
        minimum_coverage_percent = (DEFAULT_MINIMUM_FUNCTION_COVERAGE_PERCENT
                                    if configured is None else configured)
    try:
        minimum_coverage_percent = float(minimum_coverage_percent)
    except (TypeError, ValueError) as error:
        raise RuntimeError("The minimum BOLT function coverage must be a number from 1 to 100") from error
    if (not math.isfinite(minimum_coverage_percent)
            or not MINIMUM_FUNCTION_COVERAGE_PERCENT <= minimum_coverage_percent <= 100.0):
        raise RuntimeError("The minimum BOLT function coverage must be a number from 1 to 100")
    if actual_percent < minimum_coverage_percent:
        raise RuntimeError(
            f"BOLT profiled {actual_percent:.3f}% of native functions, below the "
            f"{minimum_coverage_percent:.3f}% minimum; inspect training coverage or set "
            "EDEN_WPE_BOLT_MINIMUM_FUNCTION_COVERAGE_PERCENT for a reviewed runtime-specific threshold"
        )
    return {"executedInstructions": counts, "minimumRequired": 1_000_000_000,
            "profiledFunctions": profiled_functions, "totalFunctions": total_functions,
            "functionCoveragePercent": actual_percent,
            "minimumFunctionCoveragePercent": minimum_coverage_percent}


def read_layout_results(path):
    text = path.read_text()
    if text.startswith("ADD_RESULTS(") and text.rstrip().endswith(");"):
        text = text[len("ADD_RESULTS("):].rstrip()[:-2]
    value = json.loads(text)
    if not isinstance(value, dict) or value.get("interrupted") or not isinstance(value.get("tests"), dict):
        raise RuntimeError("The layout smoke run did not finish")
    results = {}

    def visit(prefix, tree):
        if not isinstance(tree, dict):
            raise RuntimeError("The layout smoke report contains an invalid result tree")
        for name, entry in tree.items():
            if (not isinstance(name, str) or not name or "/" in name or name in (".", "..")
                    or not isinstance(entry, dict)):
                raise RuntimeError("The layout smoke report contains an invalid test entry")
            key = prefix + name
            if "actual" in entry:
                actual = entry["actual"]
                if not isinstance(actual, str):
                    raise RuntimeError(f"The layout smoke report contains an invalid outcome for {key}")
                outcomes = actual.split()
                if not outcomes or "SKIP" in outcomes:
                    raise RuntimeError(f"Layout smoke did not execute {key}")
                results[key] = outcomes
            else:
                visit(key + "/", entry)

    visit("", value["tests"])
    if not results:
        raise RuntimeError("The layout smoke report contains no executed tests")
    return results


def compare_layout_results(baseline, candidate):
    if baseline.keys() != candidate.keys():
        raise RuntimeError("The baseline and BOLT layout smoke runs executed different tests")
    regressions = [name for name in baseline if any(
        outcome != "PASS" and outcome not in baseline[name] for outcome in candidate[name]
    )]
    if regressions:
        raise RuntimeError("BOLT layout smoke regressions: " + ", ".join(sorted(regressions)))


def require_speedometer(path):
    value = json.loads(path.read_text())
    score = value.get("score") if isinstance(value, dict) else None
    mean = score.get("mean") if isinstance(score, dict) else None
    iterations = value.get("measuredValues") if isinstance(value, dict) else None
    if (score is None or score.get("isValid") is not True or not finite_number(mean)
            or not isinstance(iterations, list) or len(iterations) != SPEEDOMETER_ITERATIONS
            or value.get("viewport") != SPEEDOMETER_VIEWPORT or value.get("visibility") != "visible"
            or value.get("accelerated") is not True or not positive_integer(value.get("textureImports"))
            or not isinstance(value.get("thumbnailRequests"), int)
            or isinstance(value.get("thumbnailRequests"), bool) or value.get("thumbnailRequests") != 0):
        raise RuntimeError(
            "BOLT training and validation require ten visible GPU iterations at 1600 by 1200 with no thumbnails"
        )
    suite_names = None
    step_names = {}
    for iteration in iterations:
        tests = iteration.get("tests") if isinstance(iteration, dict) else None
        if not isinstance(tests, dict) or len(tests) != SPEEDOMETER_SUITE_COUNT:
            raise RuntimeError("Every Speedometer iteration must contain all 20 suites")
        current_suites = set(tests)
        if (any(not isinstance(name, str) or not name for name in current_suites)
                or suite_names is not None and current_suites != suite_names):
            raise RuntimeError("The Speedometer suite inventory changed between iterations")
        suite_names = current_suites
        for suite, result in tests.items():
            total = result.get("total") if isinstance(result, dict) else None
            steps = result.get("tests") if isinstance(result, dict) else None
            if not finite_number(total) or not isinstance(steps, dict) or not steps:
                raise RuntimeError("Speedometer suite results require finite totals and step data")
            current_steps = set(steps)
            if (any(not isinstance(name, str) or not name for name in current_steps)
                    or suite in step_names and current_steps != step_names[suite]):
                raise RuntimeError("The Speedometer step inventory changed between iterations")
            step_names[suite] = current_steps
            for timing in steps.values():
                metrics = timing.get("tests") if isinstance(timing, dict) else None
                asynchronous = metrics.get("Async") if isinstance(metrics, dict) else None
                if not finite_number(asynchronous, allow_zero=True):
                    raise RuntimeError("Every Speedometer step requires a finite asynchronous result")


def finite_number(value, allow_zero=False):
    return (isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)
            and (value >= 0 if allow_zero else value > 0))


def positive_integer(value):
    return isinstance(value, int) and not isinstance(value, bool) and value > 0


def require_browsing(path, elapsed_seconds):
    value = json.loads(path.read_text())
    report_seconds = value.get("seconds") if isinstance(value, dict) else None
    visits = value.get("visits") if isinstance(value, dict) else None
    pages = value.get("pages") if isinstance(value, dict) else None
    assertions = value.get("assertions") if isinstance(value, dict) else None
    terminations = value.get("rendererTerminations") if isinstance(value, dict) else None
    if (not finite_number(elapsed_seconds, allow_zero=True)
            or not finite_number(report_seconds, allow_zero=True)
            or elapsed_seconds < BROWSING_MINIMUM_SECONDS or report_seconds < BROWSING_MINIMUM_SECONDS
            or not isinstance(visits, int) or isinstance(visits, bool) or visits < BROWSING_MINIMUM_VISITS
            or not isinstance(pages, int) or isinstance(pages, bool) or pages < 5
            or not isinstance(assertions, int) or isinstance(assertions, bool) or assertions < visits
            or terminations != 0 or isinstance(terminations, bool) or value.get("passed") is not True):
        raise RuntimeError(
            "BOLT promotion requires one hour of successful page-level browsing assertions"
        )


def process_maps(pid):
    try:
        return (Path("/proc") / str(pid) / "maps").read_text()
    except OSError:
        return ""


def process_group_members(process_group):
    members = {}
    for entry in Path("/proc").iterdir():
        if not entry.name.isdecimal():
            continue
        try:
            fields = (entry / "stat").read_text().rsplit(") ", 1)[1].split()
            if int(fields[2]) != process_group:
                continue
            if fields[0] in ("Z", "X"):
                continue
        except (OSError, UnicodeError, ValueError, IndexError):
            continue
        try:
            executable = (entry / "exe").resolve(strict=True).name
        except OSError:
            executable = "unresolved"
        members[entry.name] = executable
    return members


def require_unused(library):
    for process in Path("/proc").iterdir():
        if not process.name.isdecimal():
            continue
        try:
            if process.stat().st_uid != os.getuid():
                continue
            maps = process_maps(process.name)
        except OSError:
            continue
        if str(library) in maps:
            raise RuntimeError(f"Close processes using this runtime before changing it: PID {process.name}")


def run_workload(command, environment, log, timeout, library, allow_layout_failures=False):
    started = time.monotonic()
    observed = set()
    library_identity = library.stat()
    with log.open("wb") as output:
        process = subprocess.Popen(command, env=environment, stdout=output, stderr=subprocess.STDOUT,
                                   start_new_session=True)
        group_drained = False
        try:
            while True:
                for entry in Path("/proc").iterdir():
                    if not entry.name.isdecimal():
                        continue
                    try:
                        status = (entry / "stat").read_text().rsplit(") ", 1)[1].split()
                        if int(status[2]) != process.pid:
                            continue
                        executable = (entry / "exe").resolve(strict=True).name
                        if executable != "WPEWebProcess":
                            continue
                        maps = process_maps(entry.name)
                        for line in maps.splitlines():
                            fields = line.split(maxsplit=5)
                            if len(fields) != 6 or fields[5] != str(library):
                                continue
                            major, minor = (int(part, 16) for part in fields[3].split(":"))
                            if (int(fields[4]) == library_identity.st_ino
                                    and os.makedev(major, minor) == library_identity.st_dev):
                                observed.add(entry.name)
                    except (OSError, UnicodeError, ValueError, IndexError):
                        continue
                code = process.poll()
                if code is not None:
                    if code and not (allow_layout_failures and 0 < code < 255):
                        raise RuntimeError(f"BOLT workload failed with exit {code}; see {log}")
                    elapsed = time.monotonic() - started
                    deadline = time.monotonic() + 30
                    while True:
                        remaining = process_group_members(process.pid)
                        if not remaining:
                            group_drained = True
                            break
                        if time.monotonic() >= deadline:
                            children = ", ".join(
                                f"{pid}:{name}" for pid, name in sorted(remaining.items())
                            )
                            raise RuntimeError(
                                "BOLT workload exited before its child processes drained: " + children
                            )
                        time.sleep(0.1)
                    break
                if time.monotonic() - started > timeout:
                    raise RuntimeError(f"BOLT workload exceeded {timeout} seconds; see {log}")
                time.sleep(0.1)
        finally:
            if not group_drained:
                try:
                    os.killpg(process.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
                deadline = time.monotonic() + 10
                while True:
                    try:
                        os.killpg(process.pid, 0)
                    except ProcessLookupError:
                        break
                    process.poll()
                    if time.monotonic() >= deadline:
                        try:
                            os.killpg(process.pid, signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                        break
                    time.sleep(0.05)
            process.wait()
    if not observed:
        raise RuntimeError("No WPEWebProcess in the workload mapped the selected WebKit library")
    return {"seconds": elapsed, "mappedBy": sorted(observed), "exitCode": code}


class BoltPipeline:
    def __init__(self, root, prefix, tool_directory, affinity, workers):
        self.root = root
        self.prefix = prefix.resolve()
        self.library = (self.prefix / "lib/libWPEWebKit-2.0.so").resolve(strict=True)
        if not self.library.is_relative_to(self.prefix / "lib"):
            raise RuntimeError("The WebKit library symlink escapes its runtime prefix")
        self.original = self.library.with_name(self.library.name + ".pre-bolt")
        self.directory = self.prefix / "share/eden-bolt"
        self.directory.mkdir(parents=True, exist_ok=True, mode=0o700)
        self.state_path = self.directory / "state.json"
        self.manifest_path = self.prefix / "share/eden-wpe-build.json"
        self.tools = tool_directory
        self.affinity = affinity
        self.workers = workers
        self.state = {}

    @contextmanager
    def locked(self):
        with (self.directory / "lock").open("a") as lock:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                raise RuntimeError("Another BOLT operation is using this runtime") from None
            if self.state_path.exists():
                self.state = json.loads(self.state_path.read_text())
                if self.state.get("schema") != 1 or self.state.get("library") != str(self.library):
                    raise RuntimeError("The BOLT journal does not belong to this runtime")
                if self.state.get("initializing"):
                    self.verify(self.library, self.state["inputSHA256"])
                    if not self.original.exists():
                        atomic_copy(self.library, self.original)
                    self.verify(self.original, self.state["inputSHA256"])
                    self.state.pop("initializing")
                    self.save()
                self.verify(self.original, self.state["inputSHA256"])
                if digest(self.manifest_path) != self.state["manifestSHA256"]:
                    raise RuntimeError("The runtime was rebuilt after BOLT preparation; use a fresh variant")
                if self.state.get("temporaryActivation"):
                    require_unused(self.library)
                    atomic_copy(self.original, self.library)
                    self.state.pop("temporaryActivation")
                    self.save()
                if self.state.get("promotionPending"):
                    current = digest(self.library)
                    if current == self.state["optimized"]["sha256"]:
                        waived = self.state.get("validation", {}).get("waived", [])
                        self.state["promoted"] = self.state["optimized"] | ({"waivedValidation": waived} if waived else {})
                    elif current != self.state["inputSHA256"]:
                        raise RuntimeError("The interrupted BOLT promotion has an unknown installed library")
                    self.state.pop("promotionPending")
                    self.save()
            else:
                require_original(self.library)
                if self.original.exists():
                    raise RuntimeError("An untracked pre-BOLT library exists; use a fresh runtime variant")
                manifest = json.loads(self.manifest_path.read_text())
                expected_patches = list(patch_names(resolve_patches(True)))
                registry_hash = digest(self.root / "scripts/wpe/performance_patches.py")
                if (manifest.get("mode") != "performance" or manifest.get("framePointers") is not False
                        or manifest.get("patches") != expected_patches
                        or manifest.get("patchCatalogSHA256") != registry_hash):
                    raise RuntimeError("BOLT requires the current complete Phase 1 performance patch set")
                try:
                    if profile_identity(manifest.get("profileTrainingIdentity")) != profile_identity(manifest):
                        raise RuntimeError("BOLT profile provenance does not match the prepared source identity")
                except RuntimeError as error:
                    raise RuntimeError("BOLT requires a PGO profile trained on the prepared source identity") from error
                for name in ("profileSHA256", "effectiveProfileSHA256"):
                    if not isinstance(manifest.get(name), str) or not re.fullmatch(r"[0-9a-f]{64}", manifest[name]):
                        raise RuntimeError("BOLT requires a matching, completed PGO performance profile")
                self.state = {"schema": 1, "library": str(self.library),
                              "inputSHA256": digest(self.library), "initializing": True,
                              "manifestSHA256": digest(self.manifest_path)}
                self.save()
                atomic_copy(self.library, self.original)
                self.state.pop("initializing")
                self.save()
            yield

    def save(self):
        write_json(self.state_path, self.state)

    def verify(self, path, expected):
        if not path.is_file() or digest(path) != expected:
            raise RuntimeError(f"BOLT artifact is missing or changed: {path}")

    def artifact(self, name):
        record = self.state.get(name)
        if not record:
            raise RuntimeError(f"Complete the BOLT {name} stage first")
        path = Path(record["path"])
        self.verify(path, record["sha256"])
        return path

    def record(self, path):
        return {"path": str(path), "sha256": digest(path)}

    def tool(self, name):
        return resolve_tool(self.root, self.tools, name)

    def toolchain(self):
        tools = {name: self.tool(name) for name in ("llvm-bolt", "merge-fdata")}
        parents = {path.parent for path in tools.values()}
        if len(parents) != 1:
            raise RuntimeError("llvm-bolt and merge-fdata must come from the same LLVM installation")
        current = {name: self.record(path) for name, path in tools.items()}
        previous = self.state.get("toolchain")
        if previous and previous != current:
            raise RuntimeError("The BOLT toolchain changed during this training cycle")
        if not previous:
            self.state["toolchain"] = current
            self.save()
        return tools

    def rewrite(self, kind, arguments):
        require_interpreter_skip_coverage(self.original)
        tool = self.toolchain()["llvm-bolt"]
        destination = self.directory / (kind + ".so")
        log = self.directory / (kind + ".log")
        with tempfile.TemporaryDirectory(prefix=kind + "-", dir=self.directory) as temporary:
            output = Path(temporary) / "lib.so"
            command = self.affinity + [str(tool), str(self.original), "-o", str(output),
                                      f"--thread-count={self.workers}",
                                      f"--skip-funcs={INTERPRETER_SKIP_FUNCTIONS}", *arguments]
            with log.open("w") as stream:
                subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, check=True,
                               env=os.environ | {"LC_ALL": "C"})
            if ".note.bolt_info" not in elf_sections(output):
                raise RuntimeError("BOLT did not produce a rewritten ELF library")
            require_non_executable_stack(output)
            coverage = coverage_from_log(log.read_text()) if kind == "optimized" else None
            output.chmod(stat.S_IMODE(self.original.stat().st_mode))
            atomic_copy(output, destination)
        record = self.record(destination) | {"tool": self.record(tool), "command": command,
                                              "log": self.record(log)}
        if coverage:
            record["coverage"] = coverage
        self.state[kind] = record

    def instrument(self):
        if self.state.get("promoted"):
            raise RuntimeError("Roll back before starting another BOLT training cycle")
        profiles = Path(tempfile.mkdtemp(prefix="profiles-", dir=self.directory))
        profile_prefix = profiles / "webprocess"
        for key in ("instrumented", "training", "merged", "optimized", "validation"):
            self.state.pop(key, None)
        self.save()
        self.rewrite("instrumented", ["--instrument", f"--instrumentation-file={profile_prefix}",
                                      "--instrumentation-file-append-pid"])
        self.state["profilePrefix"] = str(profile_prefix)
        self.save()

    @contextmanager
    def activated(self, path):
        if self.state.get("promoted"):
            raise RuntimeError("Roll back before running BOLT training or validation")
        require_unused(self.library)
        self.verify(self.library, self.state["inputSHA256"])
        self.state["temporaryActivation"] = self.record(path)
        self.save()
        try:
            atomic_copy(path, self.library)
            yield
        finally:
            atomic_copy(self.original, self.library)
            self.state.pop("temporaryActivation", None)
            self.save()

    def workload(self, plan, role, label):
        specification = plan.get(role)
        if not isinstance(specification, dict):
            raise RuntimeError(f"The BOLT plan requires a {role} workload")
        arguments = specification.get("argv")
        if (not isinstance(arguments, list) or not arguments
                or any(not isinstance(value, str) or not value for value in arguments)):
            raise RuntimeError(f"{role}.argv must be a nonempty JSON argument array")
        timeout = specification.get("timeoutSeconds", 7200 if role == "browsing" else 2400)
        if (not isinstance(timeout, (int, float)) or isinstance(timeout, bool)
                or not math.isfinite(timeout) or not 0 < timeout <= 14400):
            raise RuntimeError(f"Invalid {role} timeout")
        output = Path(tempfile.mkdtemp(prefix=label + "-", dir=self.directory))
        report = output / "result.json"
        substitutions = {"{prefix}": str(self.prefix), "{report}": str(report), "{output}": str(output),
                         "{profiles}": str(Path(self.state["profilePrefix"]).parent) if self.state.get("profilePrefix") else ""}
        command = list(arguments)
        for key, value in substitutions.items():
            command = [argument.replace(key, value) for argument in command]
        command = self.affinity + command
        environment = os.environ.copy()
        for name in tuple(environment):
            if (name.startswith(("JSC_", "WEBKIT_", "EDEN_WPE_", "LLVM_PROFILE", "SYSPROF_"))
                    or name in ("LD_LIBRARY_PATH", "PKG_CONFIG_PATH")):
                environment.pop(name)
        environment.update({"EDEN_WPE_BOLT_RUNTIME": str(self.prefix),
                            "EDEN_WPE_BOLT_REPORT": str(report)})
        overrides = specification.get("environment", {})
        if not isinstance(overrides, dict) or any(
                not isinstance(key, str) or not isinstance(value, str) for key, value in overrides.items()):
            raise RuntimeError(f"{role}.environment must contain string keys and values")
        protected = {"EDEN_WPE_BOLT_RUNTIME", "EDEN_WPE_BOLT_REPORT", "EDEN_WPE_REQUIRE_PHASE1"}
        if set(overrides) & protected:
            raise RuntimeError(f"{role}.environment cannot override required BOLT identity fields")
        if ("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS" in overrides
                and (role != "training" or overrides["WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS"] != "1")):
            raise RuntimeError("Only isolated BOLT training may disable the WebProcess sandbox")
        environment.update(overrides)
        identity = digest(self.library)
        result = run_workload(command, environment, output / "run.log", timeout, self.library, role == "layout")
        self.verify(self.library, identity)
        result |= {"command": command, "environment": overrides, "librarySHA256": identity,
                   "log": self.record(output / "run.log")}
        if role in ("training", "speedometer"):
            require_speedometer(report)
        if role == "browsing":
            require_browsing(report, result["seconds"])
        if role == "layout":
            result["layoutResults"] = read_layout_results(report)
        if report.exists():
            result["report"] = self.record(report)
        return result

    def train(self, plan):
        instrumented = self.artifact("instrumented")
        profile_prefix = Path(self.state["profilePrefix"])
        if any(profile_prefix.parent.iterdir()):
            raise RuntimeError("Training profiles already exist; instrument again to start a clean collection")
        with self.activated(instrumented):
            result = self.workload(plan, "training", "training")
        profiles = sorted(profile_prefix.parent.glob(profile_prefix.name + ".*.fdata"))
        if not profiles or any(not path.stat().st_size for path in profiles):
            raise RuntimeError("Training produced no complete per-process BOLT profiles")
        expected_profiles = {f"{profile_prefix.name}.{pid}.fdata" for pid in result["mappedBy"]}
        available_profiles = {path.name for path in profiles}
        missing_profiles = expected_profiles - available_profiles
        if missing_profiles:
            missing_pids = ", ".join(sorted(name.rsplit(".", 2)[1] for name in missing_profiles))
            raise RuntimeError(
                "Training did not flush a BOLT profile for every observed WPEWebProcess: " + missing_pids
            )
        self.state["training"] = result | {"profiles": [self.record(path) for path in profiles]}
        self.save()

    def merge(self):
        training = self.state.get("training")
        if not training:
            raise RuntimeError("Complete instrumented Speedometer training before merging")
        profiles = training["profiles"]
        if not profiles:
            raise RuntimeError("The training journal contains no per-process profiles")
        for record in profiles:
            self.verify(Path(record["path"]), record["sha256"])
        for key in ("merged", "optimized", "validation"):
            self.state.pop(key, None)
        self.save()
        tool = self.toolchain()["merge-fdata"]
        destination = self.directory / "merged.fdata"
        with tempfile.TemporaryDirectory(prefix="merge-", dir=self.directory) as temporary:
            staged = Path(temporary)
            inputs = [record["path"] for record in profiles]
            level = 0
            while True:
                outputs = []
                for start in range(0, len(inputs), 128):
                    output = staged / f"{level}-{start}.fdata"
                    with output.open("wb") as stream, (self.directory / "merge.log").open("ab") as log:
                        subprocess.run(self.affinity + [str(tool), *inputs[start:start + 128]],
                                       stdout=stream, stderr=log, check=True)
                    if not output.stat().st_size:
                        raise RuntimeError("merge-fdata produced an empty profile")
                    outputs.append(str(output))
                if len(outputs) == 1:
                    atomic_copy(Path(outputs[0]), destination)
                    break
                inputs = outputs
                level += 1
        self.state["merged"] = self.record(destination) | {"inputs": profiles, "tool": self.record(tool)}
        self.state.pop("optimized", None)
        self.state.pop("validation", None)
        self.save()

    def optimize(self):
        profile = self.artifact("merged")
        self.state.pop("optimized", None)
        self.state.pop("validation", None)
        self.save()
        self.rewrite("optimized", [f"--data={profile}", "--reorder-blocks=ext-tsp",
                                   "--reorder-functions=hfsort+", "--split-functions", "--split-all-cold",
                                   "--split-eh", "--icf=1", "--dyno-stats"])
        self.state["optimized"]["profileSHA256"] = digest(profile)
        self.state.pop("validation", None)
        self.save()

    def validate(self, plan):
        candidate = self.artifact("optimized")
        self.state.pop("validation", None)
        self.save()
        with self.activated(self.original):
            baseline = self.workload(plan, "layout", "layout-baseline")
        with self.activated(candidate):
            smoke = self.workload(plan, "smoke", "smoke")
            speedometer = self.workload(plan, "speedometer", "speedometer")
            browsing = self.workload(plan, "browsing", "browsing")
            layout = self.workload(plan, "layout", "layout-candidate")
        compare_layout_results(baseline["layoutResults"], layout["layoutResults"])
        self.state["validation"] = {"sha256": digest(candidate), "baseline": baseline, "smoke": smoke,
                                    "speedometer": speedometer, "browsing": browsing, "layout": layout}
        self.save()

    def validate_performance(self, plan):
        candidate = self.artifact("optimized")
        self.state.pop("validation", None)
        self.save()
        with self.activated(candidate):
            smoke = self.workload(plan, "smoke", "smoke")
            speedometer = self.workload(plan, "speedometer", "speedometer")
        self.state["validation"] = {"sha256": digest(candidate), "smoke": smoke, "speedometer": speedometer,
                                    "waived": list(WAIVABLE_VALIDATION)}
        self.save()

    def promote(self):
        candidate = self.artifact("optimized")
        validation = self.state.get("validation", {})
        candidate_hash = digest(candidate)
        if validation.get("sha256") != candidate_hash:
            raise RuntimeError("Validate this exact BOLT candidate before promotion")
        waived = validation.get("waived", [])
        if waived not in ([], list(WAIVABLE_VALIDATION)):
            raise RuntimeError("Only the unavailable baseline, browsing and layout validation can be waived")
        candidate_roles = ("smoke", "speedometer") if waived else ("smoke", "speedometer", "browsing", "layout")
        if not waived and validation.get("baseline", {}).get("librarySHA256") != self.state["inputSHA256"]:
            raise RuntimeError("BOLT baseline validation did not use the immutable pre-BOLT library")
        for role in candidate_roles:
            if validation.get(role, {}).get("librarySHA256") != candidate_hash:
                raise RuntimeError(f"BOLT {role} validation did not use the exact optimized candidate")
        for role in (() if waived else ("baseline",)) + candidate_roles:
            evidence = validation[role]
            if "log" not in evidence:
                raise RuntimeError(f"BOLT {role} validation has no retained log")
            self.verify(Path(evidence["log"]["path"]), evidence["log"]["sha256"])
        for role in ("speedometer",) if waived else ("baseline", "speedometer", "browsing", "layout"):
            evidence = validation[role]
            if "report" not in evidence:
                raise RuntimeError(f"BOLT {role} validation has no retained report")
            report = Path(evidence["report"]["path"])
            self.verify(report, evidence["report"]["sha256"])
            if role == "speedometer":
                require_speedometer(report)
            elif role == "browsing":
                require_browsing(report, evidence["seconds"])
            elif role == "baseline":
                baseline = read_layout_results(report)
            else:
                compare_layout_results(baseline, read_layout_results(report))
        require_unused(self.library)
        current = digest(self.library)
        if current not in (self.state["inputSHA256"], digest(candidate)):
            raise RuntimeError("The installed library changed outside the BOLT pipeline")
        self.state["promotionPending"] = True
        self.save()
        atomic_copy(candidate, self.library)
        self.state["promoted"] = self.record(candidate) | ({"waivedValidation": waived} if waived else {})
        self.state.pop("promotionPending", None)
        self.save()

    def rollback(self):
        require_unused(self.library)
        atomic_copy(self.original, self.library)
        self.state.pop("promoted", None)
        self.state.pop("promotionPending", None)
        self.save()


def run_stage(root, prefix, stage, tool_directory, affinity, workers, plan_path=None):
    if stage not in STAGES:
        raise RuntimeError("Unknown BOLT pipeline stage")
    pipeline = BoltPipeline(root, prefix, tool_directory, affinity, workers)
    plan = json.loads(plan_path.read_text()) if plan_path else {}
    if not isinstance(plan, dict):
        raise RuntimeError("The BOLT workload plan must be a JSON object")
    with pipeline.locked():
        if pipeline.state.get("promoted") and stage not in ("promote", "rollback"):
            raise RuntimeError("Roll back before starting another BOLT operation")
        action = getattr(pipeline, stage.replace("-", "_"))
        action(plan) if stage in ("train", "validate", "validate-performance") else action()
        return pipeline.state
