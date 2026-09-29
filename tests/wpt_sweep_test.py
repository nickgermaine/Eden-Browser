import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import shutil
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPECIFICATION = importlib.util.spec_from_file_location("eden_wpt_sweep", ROOT / "tests/wpt/wpt_sweep.py")
sweep = importlib.util.module_from_spec(SPECIFICATION)
SPECIFICATION.loader.exec_module(sweep)


def wptreport(results):
    return {"run_info": {"product": "test"}, "results": results}


def result(test, status, subtests=(), message=None):
    return {
        "test": test,
        "status": status,
        "message": message,
        "subtests": [{"name": name, "status": state, "message": text} for name, state, text in subtests],
    }


class SandboxFilesTest(unittest.TestCase):
    def test_hosts_resolution_consults_the_private_hosts_file_first(self):
        system = "passwd: files\nhosts: mymachines resolve [!UNAVAIL=return] files myhostname dns\nnetworks: files\n"
        self.assertEqual(
            sweep.sandbox_nsswitch(system),
            "passwd: files\nhosts: files mymachines resolve [!UNAVAIL=return] myhostname dns\nnetworks: files\n",
        )

    def test_the_files_action_moves_away_with_files(self):
        self.assertEqual(sweep.sandbox_nsswitch("hosts: dns files [NOTFOUND=return] myhostname"), "hosts: files dns myhostname\n")

    def test_a_missing_hosts_line_still_reads_the_hosts_file(self):
        self.assertEqual(sweep.sandbox_nsswitch("passwd: files"), "passwd: files\nhosts: files dns\n")

    def test_platform_hosts_follow_the_system_entries(self):
        hosts = sweep.sandbox_hosts("127.0.0.1 localhost\n::1 localhost\n\n", ["127.0.0.1\tweb-platform.test", "127.0.0.1\twww.web-platform.test"])
        self.assertEqual(hosts, "127.0.0.1 localhost\n::1 localhost\n127.0.0.1\tweb-platform.test\n127.0.0.1\twww.web-platform.test\n")

    def test_server_ports_are_chosen_at_run_time(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            sweep.ensure_server_config(root)
            first = (root / "config.json").read_text()
            config = json.loads(first)
            self.assertEqual(set(config), {"ports"})
            self.assertEqual(config["ports"]["http"], ["auto", "auto"])
            self.assertEqual(config["ports"]["https"], ["auto", "auto"])
            self.assertTrue(all(value == ["auto"] * len(value) for value in config["ports"].values()))
            self.assertNotIn("8000", json.dumps(config))
            sweep.ensure_server_config(root)
            self.assertEqual((root / "config.json").read_text(), first)

    def test_the_sandbox_hides_audio_devices_and_maps_platform_hosts(self):
        with tempfile.TemporaryDirectory() as directory:
            _, arguments = sweep.parse_arguments(["--output", directory])
            command = sweep.sandbox_command(arguments, Path("/run/hosts"), Path("/run/nsswitch"), ["--suite", "wpt"])
            self.assertEqual(command[:2], ["bwrap", "--die-with-parent"])
            self.assertIn(["--ro-bind", "/run/hosts", "/etc/hosts"], [command[index:index + 3] for index in range(len(command))])
            self.assertIn(["--ro-bind", "/run/nsswitch", "/etc/nsswitch.conf"], [command[index:index + 3] for index in range(len(command))])
            self.assertIn(["--bind", str(arguments.output), str(arguments.output)], [command[index:index + 3] for index in range(len(command))])
            separator = command.index("--")
            self.assertEqual(command[separator + 3:], ["--inside", "--suite", "wpt"])

    def test_the_sandbox_environment_is_private(self):
        with tempfile.TemporaryDirectory() as directory:
            runtime = Path(directory)
            environment = sweep.sandbox_environment(runtime)
            self.assertEqual(environment["XDG_RUNTIME_DIR"], str(runtime))
            self.assertEqual(environment["XDG_CONFIG_HOME"], str(runtime / "config"))
            for name in ("WAYLAND_DISPLAY", "DISPLAY", "LD_LIBRARY_PATH", "WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS"):
                self.assertNotIn(name, environment)


class SourcesTest(unittest.TestCase):
    def archive(self, root, name):
        payload = io.BytesIO()
        with tarfile.open(fileobj=payload, mode="w:gz") as bundle:
            data = b"<!doctype html>"
            member = tarfile.TarInfo(f"{name}/index.html")
            member.size = len(data)
            bundle.addfile(member, io.BytesIO(data))
        path = root / f"{name}.tar.gz"
        path.write_bytes(payload.getvalue())
        return path, hashlib.sha256(payload.getvalue()).hexdigest()

    def test_a_verified_archive_is_extracted_in_place(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive, digest = self.archive(root, "wpt-test")
            target = root / "wpt-test"
            self.assertEqual(sweep.ensure_source(target, archive, "https://invalid.example/", digest), target)
            self.assertEqual((target / "index.html").read_text(), "<!doctype html>")
            self.assertEqual(sorted(path.name for path in root.iterdir()), ["wpt-test", "wpt-test.tar.gz"])

    def test_a_tampered_archive_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive, _ = self.archive(root, "wpt-test")
            with self.assertRaisesRegex(RuntimeError, "SHA-256"):
                sweep.ensure_source(root / "wpt-test", archive, "https://invalid.example/", "0" * 64)
            self.assertFalse((root / "wpt-test").exists())

    def test_an_archive_without_the_expected_tree_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive, digest = self.archive(root, "other")
            with self.assertRaisesRegex(RuntimeError, "does not contain"):
                sweep.ensure_source(root / "wpt-test", archive, "https://invalid.example/", digest)
            self.assertFalse((root / "wpt-test").exists())

    def test_an_extracted_tree_is_used_as_is(self):
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "wpt-test"
            target.mkdir()
            self.assertEqual(sweep.ensure_source(target, Path(directory) / "missing.tar.gz", "https://invalid.example/", "0" * 64), target)


class AdapterTest(unittest.TestCase):
    def package(self, root, script):
        payload = io.BytesIO()
        with tarfile.open(fileobj=payload, mode="w:gz") as bundle:
            member = tarfile.TarInfo("package/out/adapter.js")
            member.size = len(script)
            bundle.addfile(member, io.BytesIO(script))
        archive = root / "webrtc-adapter.tgz"
        archive.write_bytes(payload.getvalue())
        return archive, hashlib.sha256(payload.getvalue()).hexdigest(), hashlib.sha256(script).hexdigest()

    def pinned(self, archive_digest, script_digest):
        self.addCleanup(setattr, sweep, "ADAPTER_ARCHIVE_SHA256", sweep.ADAPTER_ARCHIVE_SHA256)
        self.addCleanup(setattr, sweep, "ADAPTER_SHA256", sweep.ADAPTER_SHA256)
        sweep.ADAPTER_ARCHIVE_SHA256, sweep.ADAPTER_SHA256 = archive_digest, script_digest

    def test_the_pinned_adapter_is_extracted_once(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive, archive_digest, script_digest = self.package(root, b"window.adapter = {};")
            self.pinned(archive_digest, script_digest)
            script = root / "adapter.js"
            self.assertEqual(sweep.ensure_adapter(script, archive, "https://invalid.example/"), script)
            self.assertEqual(script.read_bytes(), b"window.adapter = {};")
            archive.unlink()
            self.assertEqual(sweep.ensure_adapter(script, archive, "https://invalid.example/"), script)

    def test_a_tampered_package_or_script_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive, archive_digest, script_digest = self.package(root, b"window.adapter = {};")
            self.pinned("0" * 64, script_digest)
            with self.assertRaisesRegex(RuntimeError, "SHA-256"):
                sweep.ensure_adapter(root / "adapter.js", archive, "https://invalid.example/")
            self.pinned(archive_digest, "0" * 64)
            with self.assertRaisesRegex(RuntimeError, "does not match"):
                sweep.ensure_adapter(root / "adapter.js", archive, "https://invalid.example/")
            self.assertFalse((root / "adapter.js").exists())

    def test_a_stale_copy_is_replaced(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive, archive_digest, script_digest = self.package(root, b"window.adapter = {version: 2};")
            self.pinned(archive_digest, script_digest)
            script = root / "adapter.js"
            script.write_bytes(b"window.adapter = {version: 1};")
            sweep.ensure_adapter(script, archive, "https://invalid.example/")
            self.assertEqual(script.read_bytes(), b"window.adapter = {version: 2};")

    def test_the_adapter_sits_next_to_the_samples(self):
        script, archive = sweep.adapter_paths(Path("/third_party/webrtc-samples/samples-x"))
        self.assertEqual(script, Path(f"/third_party/webrtc-samples/adapter-{sweep.ADAPTER_VERSION}.js"))
        self.assertEqual(archive, Path(f"/third_party/webrtc-samples/webrtc-adapter-{sweep.ADAPTER_VERSION}.tgz"))


class CommandTest(unittest.TestCase):
    def arguments(self, *extra):
        directory = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, directory, True)
        _, arguments = sweep.parse_arguments(["--output", directory, *extra])
        arguments.python = Path("/venv/bin/python")
        arguments.webdriver = Path("/wpe/bin/WPEWebDriver")
        arguments.eden_version = "local-performance-combined6"
        arguments.chromium_version = "Chromium 153"
        return arguments

    def value_after(self, command, flag):
        return command[command.index(flag) + 1]

    def test_eden_runs_through_webkit_webdriver_and_the_host(self):
        arguments = self.arguments("--include", "webrtc", "--include", "webrtc/RTCPeerConnection-createOffer.html")
        command = sweep.wptrunner_command(arguments, "eden", True)
        self.assertEqual(command[:3], ["/venv/bin/python", "-c", sweep.WPTRUNNER_LAUNCHER])
        self.assertIn("--product=wpewebkit_minibrowser", command)
        self.assertIn("--no-headless", command)
        self.assertIn("--binary-arg=--browser-name=MiniBrowser", command)
        self.assertEqual(self.value_after(command, "--binary"), str(arguments.build / "eden-wpt-host"))
        self.assertEqual(self.value_after(command, "--webdriver-binary"), "/wpe/bin/WPEWebDriver")
        self.assertEqual(self.value_after(command, "--browser-version"), "local-performance-combined6")
        self.assertIn("--manifest-update", command)
        self.assertIn("--no-manifest-download", command)
        self.assertIn("--test-type=testharness", command)
        self.assertIn("--processes=1", command)
        self.assertIn("--include=/webrtc/", command)
        self.assertIn("--include=/webrtc/RTCPeerConnection-createOffer.html", command)
        self.assertEqual(self.value_after(command, "--log-wptreport"), str(arguments.output / "wpt/eden/wptreport.json"))

    def test_chromium_runs_shipping_features_headless(self):
        arguments = self.arguments("--timeout-multiplier", "2")
        command = sweep.wptrunner_command(arguments, "chromium", False)
        self.assertIn("--product=chromium", command)
        self.assertIn("--headless", command)
        self.assertIn("--no-enable-experimental", command)
        self.assertIn("--no-manifest-update", command)
        self.assertIn("--timeout-multiplier=2.0", command)
        self.assertEqual(self.value_after(command, "--binary"), str(arguments.chromium))
        self.assertEqual(self.value_after(command, "--webdriver-binary"), str(arguments.chromedriver))
        self.assertNotIn("--binary-arg=--browser-name=MiniBrowser", command)
        includes = [item for item in command if item.startswith("--include=")]
        self.assertEqual(includes, [f"--include=/{name}/" for name in sweep.WPT_DIRECTORIES])

    def test_samples_run_with_the_matching_webdriver(self):
        arguments = self.arguments("--sample", "peerconnection/pc1")
        eden = sweep.samples_command(arguments, "eden")
        chromium = sweep.samples_command(arguments, "chromium")
        self.assertEqual(eden[1], str(sweep.ROOT / "tests/wpt/samples/run.mjs"))
        self.assertEqual(self.value_after(eden, "--webdriver"), "/wpe/bin/WPEWebDriver")
        self.assertEqual(self.value_after(eden, "--binary"), str(arguments.build / "eden-wpt-host"))
        self.assertEqual(self.value_after(chromium, "--webdriver"), str(arguments.chromedriver))
        self.assertEqual(self.value_after(chromium, "--binary"), str(arguments.chromium))
        self.assertEqual(self.value_after(eden, "--filter"), "peerconnection/pc1")
        self.assertEqual(self.value_after(eden, "--adapter"), str(sweep.adapter_paths(arguments.samples)[0]))
        self.assertEqual(self.value_after(eden, "--test-timeout"), "30000")

    def test_arguments_are_validated(self):
        with tempfile.TemporaryDirectory() as directory:
            for extra in (["--timeout-multiplier", "0.1"], ["--timeout-multiplier", "11"], ["--include", "/"]):
                with self.subTest(extra=extra), self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
                    sweep.parse_arguments(["--output", directory, *extra])

    def test_forwarded_arguments_round_trip(self):
        arguments = self.arguments("--browser", "eden", "--suite", "samples", "--sample", "pc1", "--skip-manifest-update")
        forwarded = sweep.forwarded_arguments(arguments, Path("/venv/bin/python"), Path("/wpe/bin/WPEWebDriver"), "v", "c")
        _, parsed = sweep.parse_arguments([*forwarded, "--inside"])
        self.assertTrue(parsed.inside)
        self.assertEqual(parsed.browser, ["eden"])
        self.assertEqual(parsed.suite, ["samples"])
        self.assertEqual(parsed.sample, ["pc1"])
        self.assertTrue(parsed.skip_manifest_update)
        self.assertEqual(parsed.python, Path("/venv/bin/python"))
        self.assertEqual(parsed.webdriver, Path("/wpe/bin/WPEWebDriver"))
        self.assertEqual(parsed.include, list(sweep.WPT_DIRECTORIES))
        self.assertEqual(parsed.output, arguments.output)


class ComparisonTest(unittest.TestCase):
    def write(self, directory, name, payload):
        path = Path(directory) / name
        path.write_text(json.dumps(payload))
        return path

    def test_differences_are_reported_in_both_directions(self):
        eden = {
            "tests": {
                "/webrtc/a.html": {"status": "OK", "message": None, "subtests": {
                    "shared": {"status": "PASS", "message": None},
                    "resize": {"status": "FAIL", "message": "assert_true: resizeMode should be supported expected true got undefined"},
                    "ours": {"status": "PASS", "message": None},
                }},
                "/webrtc/b.html": {"status": "TIMEOUT", "message": None, "subtests": {}},
                "/webcodecs/c.html": {"status": "OK", "message": None, "subtests": {}},
            },
        }
        chromium = {
            "tests": {
                "/webrtc/a.html": {"status": "OK", "message": None, "subtests": {
                    "shared": {"status": "PASS", "message": None},
                    "resize": {"status": "PASS", "message": None},
                    "ours": {"status": "FAIL", "message": "assert_equals: expected 1 but got 2"},
                }},
                "/webrtc/b.html": {"status": "OK", "message": None, "subtests": {}},
                "/mediacapture-streams/d.html": {"status": "OK", "message": None, "subtests": {}},
            },
        }
        comparison = sweep.compare(eden, chromium)
        found = {(item["test"], item["subtest"], item["kind"], item["hint"]) for item in comparison["differences"]}
        self.assertEqual(found, {
            ("/webrtc/a.html", "resize", "eden fails", "missing feature"),
            ("/webrtc/a.html", "ours", "chromium fails", "behavior"),
            ("/webrtc/b.html", None, "eden fails", "timeout"),
            ("/webcodecs/c.html", None, "missing in chromium", "harness"),
            ("/mediacapture-streams/d.html", None, "missing in eden", "harness"),
        })
        webrtc = comparison["summary"]["webrtc"]
        self.assertEqual(webrtc["tests"], 2)
        self.assertEqual(webrtc["subtests"], 3)
        self.assertEqual(webrtc["eden_passes"], 2)
        self.assertEqual(webrtc["chromium_passes"], 2)
        self.assertEqual(webrtc["eden_only_failures"], 1)
        self.assertEqual(webrtc["chromium_only_failures"], 1)
        self.assertEqual(comparison["summary"]["webcodecs"]["missing"], 1)

    def test_shared_failures_are_not_differences(self):
        eden = {"tests": {"/webrtc/a.html": {"status": "OK", "message": None, "subtests": {"x": {"status": "FAIL", "message": "a"}}}}}
        chromium = {"tests": {"/webrtc/a.html": {"status": "OK", "message": None, "subtests": {"x": {"status": "TIMEOUT", "message": None}}}}}
        self.assertEqual(sweep.compare(eden, chromium)["differences"], [])

    def test_hints(self):
        self.assertEqual(sweep.hint("promise_test: Unhandled rejection with value: 'set_permission' is unimplemented", "FAIL"), "harness")
        self.assertEqual(sweep.hint("Unable to set permission to denied for this test", "PRECONDITION_FAILED"), "harness")
        self.assertEqual(sweep.hint("error: Action set_permission not implemented", "FAIL"), "harness")
        self.assertEqual(sweep.hint("pc.getConfiguration is not a function", "FAIL"), "missing feature")
        self.assertEqual(sweep.hint("NotSupportedError: Not implemented", "FAIL"), "missing feature")
        self.assertEqual(sweep.hint(None, "CRASH"), "crash")
        self.assertEqual(sweep.hint(None, "EXTERNAL-TIMEOUT"), "timeout")
        self.assertEqual(sweep.hint("assert_equals: expected 2 but got 3", "FAIL"), "behavior")

    def test_wptreports_and_sample_results_load(self):
        with tempfile.TemporaryDirectory() as directory:
            report = self.write(directory, "wptreport.json", wptreport([
                result("/webrtc/a.html", "OK", [("one", "PASS", None), ("two", "FAIL", "boom")]),
            ]))
            loaded = sweep.load_wptreport(report)
            self.assertEqual(loaded["run_info"], {"product": "test"})
            self.assertEqual(loaded["tests"]["/webrtc/a.html"]["subtests"]["two"], {"status": "FAIL", "message": "boom"})
            samples = self.write(directory, "results.json", {
                "run_info": {"browser": "eden"},
                "results": [
                    {"file": "peerconnection/pc1", "name": "simple peerconnection > call", "status": "PASS", "message": None},
                    {"file": "peerconnection/pc1", "name": "simple peerconnection > hangup", "status": "TIMEOUT", "message": "Exceeded timeout"},
                ],
                "files": [
                    {"file": "peerconnection/pc1", "status": "OK", "message": None},
                    {"file": "capture/broken", "status": "ERROR", "message": "SyntaxError"},
                ],
            })
            loaded = sweep.load_samples(samples)
            self.assertEqual(loaded["tests"]["peerconnection/pc1"]["status"], "OK")
            self.assertEqual(loaded["tests"]["peerconnection/pc1"]["subtests"]["simple peerconnection > hangup"]["status"], "TIMEOUT")
            self.assertEqual(loaded["tests"]["capture/broken"], {"status": "ERROR", "message": "SyntaxError", "subtests": {}})

    def test_an_empty_result_file_is_a_clear_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            empty = Path(directory) / "wptreport.json"
            empty.write_text("")
            with self.assertRaisesRegex(RuntimeError, "produced no results"):
                sweep.load_wptreport(empty)
            empty.write_text("{")
            with self.assertRaisesRegex(RuntimeError, "not valid JSON"):
                sweep.load_wptreport(empty)

    def test_the_report_lists_every_difference(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            for browser, status in (("eden", "FAIL"), ("chromium", "PASS")):
                target = output / "wpt" / browser
                target.mkdir(parents=True)
                (target / "wptreport.json").write_text(json.dumps(wptreport([
                    result("/webrtc/a.html", "OK", [("offer", status, "assert_true: expected true got false" if status == "FAIL" else None)]),
                ])))
            (output / "meta.json").write_text(json.dumps({"eden_version": "combined6", "eden_runtime": "/wpe", "chromium_version": "Chromium 153"}))
            _, arguments = sweep.parse_arguments(["--output", directory, "--report-only"])
            comparisons = sweep.report(arguments)
            self.assertEqual(list(comparisons), ["wpt"])
            text = (output / "report.md").read_text()
            self.assertIn("| webrtc | 1 | 1 | 0 | 1 | 1 | 0 | 0 |", text)
            self.assertIn("### Eden fails (1)", text)
            self.assertIn("`/webrtc/a.html | offer`: FAIL assert_true: expected true got false", text)
            self.assertIn("Chromium 153", text)
            diff = json.loads((output / "diff.json").read_text())
            self.assertEqual(diff["wpt"]["differences"][0]["kind"], "eden fails")

    def finished_sweep(self, directory, differences):
        root = Path(directory)
        root.mkdir(parents=True, exist_ok=True)
        (root / "diff.json").write_text(json.dumps(differences))
        return root

    def test_a_confirmation_run_reruns_only_what_differed(self):
        with tempfile.TemporaryDirectory() as directory:
            previous = self.finished_sweep(Path(directory) / "first", {
                "wpt": {"summary": {}, "differences": [
                    {"test": "/webrtc/a.html?rest", "subtest": "x", "kind": "eden fails"},
                    {"test": "/webrtc/a.html?interop", "subtest": "y", "kind": "chromium fails"},
                    {"test": "/webcodecs/b.any.worker.html", "subtest": None, "kind": "eden fails"},
                ]},
                "samples": {"summary": {}, "differences": [
                    {"test": "peerconnection/pc1", "subtest": "call", "kind": "eden fails"},
                ]},
            })
            _, arguments = sweep.parse_arguments(["--output", str(Path(directory) / "second"), "--differences-from", str(previous)])
            self.assertEqual(arguments.suite, ["wpt", "samples"])
            self.assertEqual(arguments.include, ["/webcodecs/b.any.worker.html", "/webrtc/a.html"])
            self.assertEqual(arguments.sample, ["peerconnection/pc1"])
            _, only_wpt = sweep.parse_arguments(["--output", str(Path(directory) / "third"), "--differences-from", str(previous), "--suite", "wpt"])
            self.assertEqual(only_wpt.suite, ["wpt"])
            self.assertEqual(only_wpt.sample, [])

    def test_a_confirmation_run_needs_differences_and_owns_the_selection(self):
        with tempfile.TemporaryDirectory() as directory:
            empty = self.finished_sweep(Path(directory) / "empty", {"wpt": {"summary": {}, "differences": []}})
            full = self.finished_sweep(Path(directory) / "full", {"wpt": {"summary": {}, "differences": [
                {"test": "/webrtc/a.html", "subtest": None, "kind": "eden fails"},
            ]}})
            for extra in (["--differences-from", str(empty)], ["--differences-from", str(full), "--include", "webrtc"],
                          ["--differences-from", str(Path(directory) / "missing")]):
                with self.subTest(extra=extra), self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
                    sweep.parse_arguments(["--output", str(Path(directory) / "out"), *extra])

    def test_differences_that_did_not_reproduce_are_marked_flaky(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "first"
            for browser, first, second in (("eden", "FAIL", "FAIL"), ("chromium", "PASS", "PASS")):
                target = output / "wpt" / browser
                target.mkdir(parents=True)
                (target / "wptreport.json").write_text(json.dumps(wptreport([
                    result("/webrtc/a.html", "OK", [("steady", first, "assert_true: steady"), ("flaky", second, "assert_true: flaky")]),
                ])))
            rerun = self.finished_sweep(Path(directory) / "rerun", {"wpt": {"summary": {}, "differences": [
                {"test": "/webrtc/a.html", "subtest": "steady", "kind": "eden fails"},
            ]}})
            _, arguments = sweep.parse_arguments(["--output", str(output), "--report-only", "--confirmation", str(rerun)])
            comparisons = sweep.report(arguments)
            marks = {item["subtest"]: item["reproduced"] for item in comparisons["wpt"]["differences"]}
            self.assertEqual(marks, {"steady": True, "flaky": False})
            text = (output / "report.md").read_text()
            self.assertIn("1 reproduced in the confirmation run, 1 did not and are marked flaky.", text)
            self.assertLess(text.index("`/webrtc/a.html | steady`"), text.index("(flaky) `/webrtc/a.html | flaky`"))
            stored = {item["subtest"]: item["reproduced"] for item in json.loads((output / "diff.json").read_text())["wpt"]["differences"]}
            self.assertEqual(stored, {"steady": True, "flaky": False})

    def test_only_rerun_suites_are_marked(self):
        comparisons = {
            "wpt": {"differences": [{"test": "/webrtc/a.html", "subtest": "x", "kind": "eden fails"}]},
            "samples": {"differences": [{"test": "peerconnection/pc1", "subtest": "call", "kind": "eden fails"}]},
        }
        sweep.mark_reproduced(comparisons, {"wpt": {"differences": []}})
        self.assertIs(comparisons["wpt"]["differences"][0]["reproduced"], False)
        self.assertNotIn("reproduced", comparisons["samples"]["differences"][0])

    def test_runs_into_the_same_directory_keep_their_history(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "meta.json"
            versions = {"eden_version": "combined6", "chromium_version": "Chromium 153"}
            first = {"suites": ["wpt"], "started": "2026-09-27T20:58:20-03:00"}
            sweep.record_run(path, versions, first)
            first["finished"] = "2026-09-27T22:10:46-03:00"
            sweep.record_run(path, versions, first)
            second = {"suites": ["samples"], "started": "2026-09-27T22:20:00-03:00"}
            meta = sweep.record_run(path, {**versions, "eden_version": "combined7"}, second)
            self.assertEqual(meta["eden_version"], "combined7")
            self.assertEqual([run["suites"] for run in meta["runs"]], [["wpt"], ["samples"]])
            self.assertEqual(meta["runs"][0]["finished"], "2026-09-27T22:10:46-03:00")
            self.assertEqual(json.loads(path.read_text()), meta)

    def test_a_report_needs_both_browsers(self):
        with tempfile.TemporaryDirectory() as directory:
            _, arguments = sweep.parse_arguments(["--output", directory, "--report-only"])
            with self.assertRaisesRegex(RuntimeError, "no results from both browsers"):
                sweep.report(arguments)


if __name__ == "__main__":
    unittest.main()
