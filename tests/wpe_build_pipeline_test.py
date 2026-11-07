from pathlib import Path
import json
import os
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from wpe import bolt
from wpe import bolt_workloads
from wpe import pgo
from wpe.adopted import navigation_preconnect, process_prewarm
from wpe.performance_patches import names as patch_names, resolve_patches
import build_wpe_combined
import build_wpe
import test_wpe


class WebRTCBackendTest(unittest.TestCase):
    def pack(self, directory, version, files):
        archive = directory / f"libwebrtc-wpewebkit-{version}.tar.xz"
        root = directory / "content"
        for name, data in files.items():
            path = root / f"libwebrtc-wpewebkit-{version}" / "Source/ThirdParty/libwebrtc" / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        import tarfile
        with tarfile.open(archive, "w:xz") as package:
            package.add(root / f"libwebrtc-wpewebkit-{version}", arcname=f"libwebrtc-wpewebkit-{version}")
        import hashlib
        return archive, hashlib.sha256(archive.read_bytes()).hexdigest()

    def test_libwebrtc_subtree_is_verified_and_extracted_once(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            packages = root / "third_party/wpe/packages"
            packages.mkdir(parents=True)
            archive, digest = self.pack(packages, "9.9.9", {"CMakeLists.txt": b"project(webrtc)\n", "Source/webrtc/api/a.cc": b"int a;\n"})
            source = root / "source"
            source.mkdir()
            build_wpe.prepare_libwebrtc_source(root, source, "9.9.9", digest)
            subtree = source / build_wpe.LIBWEBRTC_SOURCE_SUBTREE
            self.assertEqual((subtree / "CMakeLists.txt").read_bytes(), b"project(webrtc)\n")
            self.assertEqual((subtree / "Source/webrtc/api/a.cc").read_bytes(), b"int a;\n")
            (subtree / "Source/webrtc/api/a.cc").write_bytes(b"int changed;\n")
            build_wpe.prepare_libwebrtc_source(root, source, "9.9.9", digest)
            self.assertEqual((subtree / "Source/webrtc/api/a.cc").read_bytes(), b"int changed;\n")
            with self.assertRaisesRegex(RuntimeError, "SHA-256"):
                build_wpe.prepare_libwebrtc_source(root, root / "other", "9.9.9", "0" * 64)
            archive.unlink()
            with self.assertRaisesRegex(RuntimeError, "missing"):
                build_wpe.prepare_libwebrtc_source(root, root / "other", "9.9.9", digest)

    def test_libwebrtc_subtree_patches_apply_to_the_pinned_archive(self):
        version = "2.53.92"
        archive = ROOT / "third_party/wpe/packages" / f"libwebrtc-wpewebkit-{version}.tar.xz"
        if not archive.is_file():
            self.skipTest("The pinned libwebrtc archive is not present")
        patched = build_wpe.libwebrtc_subtree_patches(archive, version)
        self.assertEqual(set(patched), {build_wpe.LIBWEBRTC_SOURCE_SUBTREE + "/CMakeLists.txt"})
        cmake = patched[build_wpe.LIBWEBRTC_SOURCE_SUBTREE + "/CMakeLists.txt"]
        block = cmake[cmake.index("if (USE_LIBWEBRTC_AV1 AND NOT APPLE)"):]
        block = block[:block.index("\nendif ()") + len("\nendif ()")]
        for expected in (
            "Source/webrtc/modules/video_coding/codecs/av1/dav1d_decoder.cc",
            "Source/webrtc/modules/video_coding/codecs/av1/libaom_av1_encoder.cc",
            "Source/webrtc/modules/video_coding/codecs/av1/libaom_speed_config_factory.cc",
            'target_include_directories(webrtc BEFORE PRIVATE "${_libwebrtc_av1_forward}")',
            "target_link_libraries(webrtc PkgConfig::LIBWEBRTC_AOM PkgConfig::LIBWEBRTC_DAV1D)",
            "RTC_USE_LIBAOM_AV1_ENCODER RTC_DAV1D_IN_INTERNAL_DECODER_FACTORY",
        ):
            self.assertIn(expected, block)
        self.assertLess(cmake.index("target_include_directories(webrtc ${webrtc_INCLUDE_DIRECTORIES})"), cmake.index("if (USE_LIBWEBRTC_AV1 AND NOT APPLE)"))
        self.assertEqual(cmake.count("    Source/webrtc/modules/audio_processing/capture_mixer/capture_mixer.cc\n"), 1)
        self.assertLess(cmake.index("capture_mixer/audio_content_analyzer.cc"), cmake.index("capture_mixer/capture_mixer.cc"))
        self.assertEqual(patched, build_wpe.libwebrtc_subtree_patches(archive, version))
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            packages = root / "third_party/wpe/packages"
            packages.mkdir(parents=True)
            (packages / archive.name).symlink_to(archive)
            destination = root / "source" / build_wpe.LIBWEBRTC_SOURCE_SUBTREE / "CMakeLists.txt"
            destination.parent.mkdir(parents=True)
            destination.write_text("stale")
            build_wpe.apply_libwebrtc_subtree_patches(root, root / "source", version)
            self.assertEqual(destination.read_text(), cmake)
            modified = destination.stat().st_mtime_ns
            build_wpe.apply_libwebrtc_subtree_patches(root, root / "source", version)
            self.assertEqual(destination.stat().st_mtime_ns, modified)

    def test_managed_sources_include_the_libwebrtc_subtree_only_for_that_backend(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)
            subtree = source / build_wpe.LIBWEBRTC_SOURCE_SUBTREE
            (subtree / "Source").mkdir(parents=True)
            (subtree / "CMakeLists.txt").write_text("x")
            (subtree / "Source/b.cc").write_text("y")
            gstreamer = build_wpe.managed_source_paths(source, (), False, "gstreamer")
            libwebrtc = build_wpe.managed_source_paths(source, (), False, "libwebrtc")
            self.assertEqual(libwebrtc - gstreamer, {build_wpe.LIBWEBRTC_SOURCE_SUBTREE + "/CMakeLists.txt", build_wpe.LIBWEBRTC_SOURCE_SUBTREE + "/Source/b.cc"})

    def test_variant_and_configure_flags_follow_the_backend(self):
        self.assertEqual(build_wpe.automatic_variant(False, False, "gstreamer"), "")
        self.assertEqual(build_wpe.automatic_variant(True, False, "gstreamer"), "experiments")
        self.assertEqual(build_wpe.automatic_variant(False, False, "libwebrtc"), "libwebrtc")
        self.assertEqual(build_wpe.automatic_variant(True, True, "libwebrtc"), "libwebrtc-experiments-frame-profile")
        self.assertIn("-DUSE_GSTREAMER_WEBRTC=OFF", build_wpe.webrtc_configure_flags("libwebrtc"))
        self.assertIn("-DUSE_LIBWEBRTC=ON", build_wpe.webrtc_configure_flags("libwebrtc"))
        self.assertIn("-DUSE_GSTREAMER_WEBRTC=ON", build_wpe.webrtc_configure_flags("gstreamer"))
        self.assertIn("-DUSE_LIBWEBRTC=OFF", build_wpe.webrtc_configure_flags("gstreamer"))
        self.assertIs(build_wpe.correctness_modules("gstreamer"), build_wpe.GSTREAMER_CORRECTNESS_MODULES)
        self.assertIs(build_wpe.correctness_modules("libwebrtc"), build_wpe.LIBWEBRTC_CORRECTNESS_MODULES)

    def test_verification_builds_can_skip_pgo(self):
        with tempfile.TemporaryDirectory() as temporary:
            dependency = Path(temporary)
            (dependency / "profiles").mkdir()
            generic = dependency / "profiles" / "2.53.92.profdata"
            generic.write_bytes(b"profile")
            variant = dependency / "profiles" / "2.53.92-webrtcbin.profdata"
            variant.write_bytes(b"profile")
            self.assertEqual(build_wpe.select_profile(dependency, "2.53.92", "webrtcbin", "-webrtcbin", "gstreamer", None, False), variant)
            self.assertEqual(build_wpe.select_profile(dependency, "2.53.92", "", "", "gstreamer", None, False), generic)
            self.assertIsNone(build_wpe.select_profile(dependency, "2.53.92", "webrtcbin", "-webrtcbin", "gstreamer", None, True))
            self.assertIsNone(build_wpe.select_profile(dependency, "2.53.92", "webrtcbin", "-webrtcbin", "libwebrtc", "a" * 64, False))

    def test_profiles_are_only_selected_for_their_own_backend(self):
        with tempfile.TemporaryDirectory() as temporary:
            profile = Path(temporary) / "2.53.92-libwebrtc.profdata"
            profile.write_bytes(b"profile")
            self.assertFalse(build_wpe.profile_matches_backend(profile, "a" * 64))
            sidecar = profile.with_suffix(".profdata.json")
            sidecar.write_text(json.dumps({"trainingIdentity": {"embeddedPatchGeneratorSHA256": "b" * 64}}))
            self.assertFalse(build_wpe.profile_matches_backend(profile, "a" * 64))
            self.assertTrue(build_wpe.profile_matches_backend(profile, "b" * 64))


class ManagedSourceTimestampTest(unittest.TestCase):
    def test_only_identical_final_contents_recover_original_timestamps(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary)
            original = {"same.cpp": "same", "changed.cpp": "old", "removed.h": "removed", "payload.h": "payload"}
            for name, content in original.items():
                path = source / name
                path.write_text(content)
                os.utime(path, ns=(1_000_000_000, 1_000_000_000))
            snapshot = build_wpe.snapshot_managed_sources(source, [*original, "new.cpp"])
            (source / "same.cpp").write_text("temporary reset")
            (source / "same.cpp").write_text("same")
            (source / "changed.cpp").write_text("new")
            (source / "removed.h").unlink()
            (source / "payload.h").unlink()
            (source / "payload.h").write_text("payload")
            (source / "new.cpp").write_text("new")
            changed_time = (source / "changed.cpp").stat().st_mtime_ns
            new_time = (source / "new.cpp").stat().st_mtime_ns
            build_wpe.restore_unchanged_source_timestamps(source, snapshot)
            for name in ("same.cpp", "payload.h"):
                self.assertEqual((source / name).stat().st_mtime_ns, 1_000_000_000)
            self.assertEqual((source / "changed.cpp").stat().st_mtime_ns, changed_time)
            self.assertEqual((source / "changed.cpp").read_text(), "new")
            self.assertEqual((source / "new.cpp").stat().st_mtime_ns, new_time)
            self.assertFalse((source / "removed.h").exists())

    def test_paths_dropped_from_the_embedded_patches_return_to_the_archive(self):
        import tarfile
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            content = root / "content" / "wpewebkit-9.9.9"
            pristine = {"Source/kept.cpp": b"kept\n", "Source/dropped.cpp": b"dropped\n", "Source/retired.cpp": b"retired\n", "Source/other.cpp": b"other\n"}
            for name, data in pristine.items():
                path = content / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
            archive = root / "wpewebkit-9.9.9.tar.xz"
            with tarfile.open(archive, "w:xz") as package:
                package.add(content, arcname="wpewebkit-9.9.9")
            source = root / "source"
            for name in pristine:
                (source / name).parent.mkdir(parents=True, exist_ok=True)
                (source / name).write_bytes(b"patched " + pristine[name])
            (source / "Source/generated.h").write_bytes(b"generated\n")
            manifest = source / build_wpe.EMBEDDED_MANIFEST
            with mock.patch.object(build_wpe, "RETIRED_EMBEDDED_PATHS", ("Source/retired.cpp",)):
                self.assertEqual(build_wpe.restore_dropped_embedded_paths(source, archive, "9.9.9", ["Source/kept.cpp", "Source/dropped.cpp", "Source/generated.h"]), ["Source/retired.cpp"])
            self.assertEqual((source / "Source/retired.cpp").read_bytes(), b"retired\n")
            self.assertEqual((source / "Source/other.cpp").read_bytes(), b"patched other\n")
            self.assertEqual(json.loads(manifest.read_text()), ["Source/dropped.cpp", "Source/generated.h", "Source/kept.cpp"])
            kept_time = (source / "Source/kept.cpp").stat().st_mtime_ns
            self.assertEqual(build_wpe.restore_dropped_embedded_paths(source, archive, "9.9.9", ["Source/kept.cpp"]), ["Source/dropped.cpp", "Source/generated.h"])
            self.assertEqual((source / "Source/dropped.cpp").read_bytes(), b"dropped\n")
            self.assertFalse((source / "Source/generated.h").exists())
            self.assertEqual((source / "Source/kept.cpp").read_bytes(), b"patched kept\n")
            self.assertEqual((source / "Source/kept.cpp").stat().st_mtime_ns, kept_time)
            self.assertEqual(json.loads(manifest.read_text()), ["Source/kept.cpp"])
            self.assertEqual(build_wpe.restore_dropped_embedded_paths(source, archive, "9.9.9", ["Source/kept.cpp"]), [])

    def test_retired_embedded_paths_are_no_longer_patched(self):
        self.assertTrue(set(build_wpe.RETIRED_EMBEDDED_PATHS).isdisjoint(build_wpe.EMBEDDED_PATHS))

    def test_hardware_video_encoders_are_preferred_unless_configured(self):
        runtime = (ROOT / "src/engine/wpe/wperuntime.cpp").read_text()
        helper = runtime[runtime.index("static void preferHardwareVideoEncoders()"):runtime.index("static WPEDisplay *runtimeDisplay")]
        for encoder in ("vah264enc", "vah264lpenc", "vah265enc", "vah265lpenc", "vaav1enc", "vavp9enc"):
            self.assertIn(f'"{encoder}"', helper)
        self.assertIn('ranks.append(name + QStringLiteral(":257"));', helper)
        self.assertIn("return entry.section(':', 0, 0).trimmed() == name;", helper)
        initialize = runtime[runtime.index("bool WpeRuntime::initialize("):]
        self.assertLess(initialize.index('qEnvironmentVariable("EDEN_WPE_HARDWARE_VIDEO_ENCODING") != "0"'), initialize.index("wpe_display_connect("))

    def test_native_tests_never_reach_the_speakers(self):
        environment = {"GST_PLUGIN_FEATURE_RANK": "vah264enc:MAX,alsasink:PRIMARY"}
        test_wpe.silence_audio_output(environment)
        ranks = environment["GST_PLUGIN_FEATURE_RANK"].split(",")
        self.assertEqual(ranks[0], "vah264enc:MAX")
        self.assertNotIn("alsasink:PRIMARY", ranks)
        for sink in ("pulsesink", "pipewiresink", "alsasink", "jackaudiosink", "osssink", "oss4sink", "openalsink"):
            self.assertEqual(ranks.count(f"{sink}:NONE"), 1, sink)
        runner = (ROOT / "scripts/test_wpe.py").read_text()
        self.assertIn('*(["--tmpfs", "/dev/snd"] if Path("/dev/snd").is_dir() else []),', runner)
        self.assertLess(runner.index('"--dev-bind",\n            "/dev",'), runner.index('"--tmpfs", "/dev/snd"'))
        self.assertLess(runner.index("seed_gstreamer_registry(environment)\n        silence_audio_output(environment)"), runner.index('command = [\n            "bwrap",'))

    def test_first_preparation_has_no_timestamps_to_restore(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "new-source"
            snapshot = build_wpe.snapshot_managed_sources(source, ["Source/file.cpp"])
            self.assertEqual(snapshot, {})
            build_wpe.restore_unchanged_source_timestamps(source, snapshot)
            self.assertFalse(source.exists())


def elf_library(bolted=False, relocations=True, marker=b"original"):
    names = ["", ".shstrtab", ".text", ".symtab"]
    if relocations:
        names.append(".rela.text")
    if bolted:
        names.append(".note.bolt_info")
    strings = b""
    offsets = []
    for name in names:
        offsets.append(len(strings))
        strings += name.encode() + b"\0"
    section_offset = 120
    string_offset = section_offset + len(names) * 64
    header = struct.pack("<16sHHIQQQIHHHHHH", b"\x7fELF\x02\x01" + b"\0" * 10,
                         3, 62, 1, 0, 64, section_offset, 0, 64, 56, 1, 64, len(names), 1)
    stack = struct.pack("<IIQQQQQQ", 0x6474E551, 6, 0, 0, 0, 0, 0, 16)
    sections = b"".join(struct.pack("<IIQQQQIIQQ", offset, 3 if index == 1 else 1,
                                    0, 0, string_offset if index == 1 else 0,
                                    len(strings) if index == 1 else 0, 0, 0, 1, 0)
                        for index, offset in enumerate(offsets))
    return header + stack + sections + strings + marker


COVERAGE = """BOLT-INFO: 123 out of 1000 functions in the binary (12.3%) have non-empty execution profile
       2000000000 : executed instructions
       1800000000 : executed instructions (-10.0%)
"""


class BoltPipelineTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="eden-bolt-test-")
        self.addCleanup(self.temporary.cleanup)
        self.stage = Path(self.temporary.name)
        self.prefix = self.stage / "prefix"
        libraries = self.prefix / "lib"
        libraries.mkdir(parents=True)
        self.library = libraries / "libWPEWebKit-2.0.so.1"
        self.original_bytes = elf_library()
        self.library.write_bytes(self.original_bytes)
        self.link = libraries / "libWPEWebKit-2.0.so"
        self.link.symlink_to(self.library.name)
        (self.prefix / "share").mkdir()
        manifest = {
            "mode": "performance",
            "variant": "combined",
            "framePointers": False,
            "experiments": True,
            "patches": list(patch_names(resolve_patches(True))),
            "patchCatalogSHA256": bolt.digest(ROOT / "scripts/wpe/performance_patches.py"),
            "profileSHA256": "a" * 64,
            "effectiveProfileSHA256": "b" * 64,
        }
        identity_manifest = pgo_identity_manifest("performance")
        manifest.update(identity_manifest)
        manifest["profileTrainingIdentity"] = pgo.profile_identity(pgo_identity_manifest())
        (self.prefix / "share/eden-wpe-build.json").write_text(json.dumps(manifest))
        self.tools = self.stage / "tools"
        self.tools.mkdir()
        for name in ("llvm-bolt", "merge-fdata"):
            tool = self.tools / name
            tool.write_bytes(b"tool")
            tool.chmod(0o755)
        self.pipeline = self.reopen()
        self.commands = []
        self.process_mock = mock.patch.object(bolt.subprocess, "run", side_effect=self.execute).start()
        self.addCleanup(mock.patch.stopall)
        mock.patch.object(bolt, "require_unused").start()
        mock.patch.object(bolt, "require_interpreter_skip_coverage", return_value=2409).start()

    def reopen(self):
        return bolt.BoltPipeline(ROOT, self.prefix, self.tools, ["taskset", "--cpu-list", "0-7,16-23"], 16)

    def test_interpreter_skip_covers_dispatch_without_excluding_jsc_helpers(self):
        labels = [(0x1000, "jsc_llint_begin")]
        labels.extend((0x1001 + index, f"ipint_opcode_{index}") for index in range(100))
        labels.extend([(0x1100, "op_get_by_id_return_location"), (0x1101, "relocateJITReturnPC")])
        labels.extend([(0x2000, "jsc_llint_end"), (0x3000, "operationGetByIdOptimize")])
        symbols = "\n".join(f"{address:016x} T {name}" for address, name in labels)
        self.assertEqual(bolt.verify_interpreter_skip_symbols(symbols), len(labels) - 1)
        self.assertIsNone(bolt.re.fullmatch(bolt.INTERPRETER_SKIP_FUNCTIONS, "operationGetByIdOptimize"))
        self.assertIsNone(bolt.re.fullmatch(bolt.INTERPRETER_SKIP_FUNCTIONS, "jsDocumentPrototypeFunction_getElementById"))
        broken = symbols.replace("ipint_opcode_50", "unrecognized_dispatch_entry")
        with self.assertRaisesRegex(RuntimeError, "unrecognized_dispatch_entry"):
            bolt.verify_interpreter_skip_symbols(broken)

    def test_rejects_missing_or_executable_stack(self):
        for program_type, flags in ((0, 6), (0x6474E551, 7)):
            altered = bytearray(self.original_bytes)
            struct.pack_into("<II", altered, 64, program_type, flags)
            self.library.write_bytes(altered)
            with self.assertRaisesRegex(RuntimeError, "GNU_STACK|executable stack"):
                with self.reopen().locked():
                    pass

    def execute(self, command, **kwargs):
        self.commands.append(command)
        if str(self.tools / "llvm-bolt") in command:
            source = Path(command[command.index(str(self.tools / "llvm-bolt")) + 1])
            self.assertEqual(source.read_bytes(), self.original_bytes)
            output = Path(command[command.index("-o") + 1])
            output.write_bytes(elf_library(bolted=True, marker=b"instrument" if "--instrument" in command else b"optimized"))
            kwargs["stdout"].write(COVERAGE)
        else:
            tool_index = command.index(str(self.tools / "merge-fdata"))
            kwargs["stdout"].write(b"".join(Path(path).read_bytes() for path in command[tool_index + 1:]))
        return subprocess.CompletedProcess(command, 0)

    def prepare_profile(self):
        self.pipeline.instrument()
        prefix = Path(self.pipeline.state["profilePrefix"])
        profiles = []
        for pid in (101, 202):
            path = prefix.with_name(prefix.name + f".{pid}.fdata")
            path.write_bytes(f"profile-{pid}\n".encode())
            profiles.append(self.pipeline.record(path))
        self.pipeline.state["training"] = {"profiles": profiles}
        self.pipeline.save()
        self.pipeline.merge()

    def prepare_validation(self):
        self.prepare_profile()
        self.pipeline.optimize()
        candidate = self.pipeline.artifact("optimized")
        log = self.stage / "validation.log"
        log.write_text("completed")
        candidate_hash = bolt.digest(candidate)
        layout_baseline = self.stage / "layout-baseline.json"
        layout_candidate = self.stage / "layout-candidate.json"
        layout_report = '{"tests":{"fast":{"a.html":{"actual":"PASS"}}}}'
        layout_baseline.write_text(layout_report)
        layout_candidate.write_text(layout_report)
        speedometer = self.stage / "speedometer.json"
        speedometer.write_text(json.dumps(speedometer_result()))
        browsing = self.stage / "browsing.json"
        browsing.write_text(json.dumps(browsing_result()))
        self.pipeline.state["validation"] = {
            "sha256": bolt.digest(candidate),
            "baseline": {
                "librarySHA256": self.pipeline.state["inputSHA256"],
                "log": self.pipeline.record(log),
                "report": self.pipeline.record(layout_baseline),
            },
            "smoke": {
                "librarySHA256": candidate_hash,
                "log": self.pipeline.record(log),
            },
            "speedometer": {
                "librarySHA256": candidate_hash,
                "log": self.pipeline.record(log),
                "report": self.pipeline.record(speedometer),
            },
            "browsing": {
                "librarySHA256": candidate_hash,
                "seconds": 3600,
                "log": self.pipeline.record(log),
                "report": self.pipeline.record(browsing),
            },
            "layout": {
                "librarySHA256": candidate_hash,
                "log": self.pipeline.record(log),
                "report": self.pipeline.record(layout_candidate),
            },
        }
        self.pipeline.save()
        return candidate

    def test_instrumentation_leaves_runtime_and_symlink_unchanged(self):
        with self.pipeline.locked():
            self.pipeline.instrument()
            self.assertEqual(self.library.read_bytes(), self.original_bytes)
            self.assertEqual(self.pipeline.original.read_bytes(), self.original_bytes)
            self.assertTrue(self.link.is_symlink())
            self.assertIn("--instrumentation-file-append-pid", self.commands[0])
            self.assertIn("--thread-count=16", self.commands[0])
            self.assertEqual(self.commands[0][:3], ["taskset", "--cpu-list", "0-7,16-23"])

    def test_reinstrumentation_preserves_backup_and_uses_new_profile_directory(self):
        with self.pipeline.locked():
            self.pipeline.instrument()
            old_prefix = self.pipeline.state["profilePrefix"]
            self.pipeline.instrument()
            self.assertNotEqual(old_prefix, self.pipeline.state["profilePrefix"])
            self.assertEqual(self.pipeline.original.read_bytes(), self.original_bytes)

    def test_merge_consumes_only_recorded_profiles(self):
        with self.pipeline.locked():
            self.prepare_profile()
            merged = self.pipeline.artifact("merged")
            self.assertEqual(merged.read_bytes(), b"profile-101\nprofile-202\n")
            self.assertEqual(len(self.pipeline.state["merged"]["inputs"]), 2)

    def test_training_requires_a_profile_for_every_observed_web_process(self):
        with self.pipeline.locked():
            self.pipeline.instrument()
            prefix = Path(self.pipeline.state["profilePrefix"])
            def workload(*args):
                prefix.with_name(prefix.name + ".101.fdata").write_bytes(b"profile-101\n")
                return {"mappedBy": ["101", "202"]}

            with mock.patch.object(self.pipeline, "workload", side_effect=workload):
                with self.assertRaisesRegex(RuntimeError, "every observed WPEWebProcess"):
                    self.pipeline.train({"training": {}})
            self.assertNotIn("training", self.pipeline.state)

    def test_merge_rejects_changed_inputs(self):
        with self.pipeline.locked():
            self.prepare_profile()
            Path(self.pipeline.state["training"]["profiles"][0]["path"]).write_bytes(b"changed")
            with self.assertRaisesRegex(RuntimeError, "changed"):
                self.pipeline.merge()

    def test_empty_profile_list_fails_without_invoking_tool(self):
        with self.pipeline.locked():
            self.pipeline.state["training"] = {"profiles": []}
            with self.assertRaisesRegex(RuntimeError, "no per-process"):
                self.pipeline.merge()
            self.assertFalse(self.commands)

    def test_optimization_uses_original_and_defers_install(self):
        with self.pipeline.locked():
            self.prepare_profile()
            self.pipeline.optimize()
            self.assertEqual(self.library.read_bytes(), self.original_bytes)
            command = self.commands[-1]
            for option in ("--reorder-blocks=ext-tsp", "--reorder-functions=hfsort+", "--split-functions",
                           "--split-all-cold", "--split-eh", "--icf=1", "--dyno-stats"):
                self.assertIn(option, command)
            self.assertNotIn("--use-gnu-stack", command)
            self.assertEqual(self.pipeline.state["optimized"]["coverage"]["profiledFunctions"], 123)

    def test_failed_tool_does_not_replace_runtime_or_preserve_validation(self):
        with self.pipeline.locked():
            self.prepare_validation()
            self.process_mock.side_effect = subprocess.CalledProcessError(1, "llvm-bolt")
            with self.assertRaises(subprocess.CalledProcessError):
                self.pipeline.optimize()
            self.assertEqual(self.library.read_bytes(), self.original_bytes)
            self.assertNotIn("optimized", self.pipeline.state)
            self.assertNotIn("validation", self.pipeline.state)

    def test_workload_failure_restores_original(self):
        with self.pipeline.locked():
            self.pipeline.instrument()
            with self.assertRaisesRegex(RuntimeError, "workload"):
                with self.pipeline.activated(self.pipeline.artifact("instrumented")):
                    self.assertNotEqual(self.library.read_bytes(), self.original_bytes)
                    raise RuntimeError("workload failed")
            self.assertEqual(self.library.read_bytes(), self.original_bytes)
            self.assertNotIn("temporaryActivation", self.pipeline.state)

    def test_workload_rejects_sandbox_opt_out_outside_instrumented_training(self):
        plan = {
            "speedometer": {
                "argv": ["/bin/true"],
                "environment": {"WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS": "1"},
            }
        }
        with mock.patch.object(bolt, "run_workload") as run:
            with self.assertRaisesRegex(RuntimeError, "Only isolated BOLT training"):
                self.pipeline.workload(plan, "speedometer", "speedometer")
        run.assert_not_called()

    def test_aborted_activation_is_recovered_on_next_invocation(self):
        with self.pipeline.locked():
            self.pipeline.instrument()
            instrumented = self.pipeline.artifact("instrumented")
            self.pipeline.state["temporaryActivation"] = self.pipeline.record(instrumented)
            self.pipeline.save()
            bolt.atomic_copy(instrumented, self.library)
        pipeline = self.reopen()
        with pipeline.locked():
            self.assertEqual(self.library.read_bytes(), self.original_bytes)
            self.assertNotIn("temporaryActivation", pipeline.state)

    def test_promotion_requires_validation_of_exact_candidate(self):
        with self.pipeline.locked():
            self.prepare_profile()
            self.pipeline.optimize()
            with self.assertRaisesRegex(RuntimeError, "Validate"):
                self.pipeline.promote()
            self.assertEqual(self.library.read_bytes(), self.original_bytes)

    def test_modified_validation_evidence_blocks_promotion(self):
        with self.pipeline.locked():
            self.prepare_validation()
            (self.stage / "validation.log").write_text("different")
            with self.assertRaisesRegex(RuntimeError, "changed"):
                self.pipeline.promote()

    def test_promote_then_rollback_retains_immutable_original(self):
        with self.pipeline.locked():
            candidate = self.prepare_validation()
            expected = candidate.read_bytes()
            self.pipeline.promote()
            self.assertEqual(self.library.read_bytes(), expected)
            self.pipeline.promote()
            self.pipeline.rollback()
            self.assertEqual(self.library.read_bytes(), self.original_bytes)
            self.assertEqual(self.pipeline.original.read_bytes(), self.original_bytes)
            self.assertTrue(self.link.is_symlink())

    def prepare_performance_validation(self):
        self.prepare_profile()
        self.pipeline.optimize()
        candidate = self.pipeline.artifact("optimized")
        candidate_hash = bolt.digest(candidate)
        log = self.stage / "validation.log"
        log.write_text("completed")
        speedometer = self.stage / "speedometer.json"
        speedometer.write_text(json.dumps(speedometer_result()))
        self.pipeline.state["validation"] = {
            "sha256": candidate_hash,
            "smoke": {"librarySHA256": candidate_hash, "log": self.pipeline.record(log)},
            "speedometer": {
                "librarySHA256": candidate_hash,
                "log": self.pipeline.record(log),
                "report": self.pipeline.record(speedometer),
            },
            "waived": list(bolt.WAIVABLE_VALIDATION),
        }
        self.pipeline.save()
        return candidate

    def test_performance_validation_records_waived_gates_and_promotes_exact_candidate(self):
        with self.pipeline.locked():
            self.prepare_profile()
            self.pipeline.optimize()
            candidate = self.pipeline.artifact("optimized")
            roles = []

            def workload(plan, role, label):
                roles.append((role, bolt.digest(self.library)))
                log = self.stage / f"{label}.log"
                log.write_text("completed")
                result = {"librarySHA256": bolt.digest(self.library), "log": self.pipeline.record(log)}
                if role == "speedometer":
                    report = self.stage / "speedometer.json"
                    report.write_text(json.dumps(speedometer_result()))
                    result["report"] = self.pipeline.record(report)
                return result

            with mock.patch.object(self.pipeline, "workload", side_effect=workload):
                self.pipeline.validate_performance({})
            self.assertEqual([role for role, _ in roles], ["smoke", "speedometer"])
            self.assertTrue(all(identity == bolt.digest(candidate) for _, identity in roles))
            self.assertEqual(self.library.read_bytes(), self.original_bytes)
            self.assertEqual(self.pipeline.state["validation"]["waived"], ["baseline", "browsing", "layout"])
            self.pipeline.promote()
            self.assertEqual(self.library.read_bytes(), candidate.read_bytes())
            self.assertEqual(self.pipeline.state["promoted"]["waivedValidation"], ["baseline", "browsing", "layout"])
            self.pipeline.rollback()
            self.assertEqual(self.library.read_bytes(), self.original_bytes)
            self.assertNotIn("promoted", self.pipeline.state)

    def test_waived_promotion_rejects_partial_or_tampered_evidence(self):
        with self.pipeline.locked():
            self.prepare_performance_validation()
            validation = self.pipeline.state["validation"]
            for broken, error in (
                (validation | {"waived": ["layout"]}, "can be waived"),
                (validation | {"waived": ["smoke", "browsing", "layout"]}, "can be waived"),
                ({key: value for key, value in validation.items() if key != "smoke"}, "smoke validation"),
                (validation | {"speedometer": validation["speedometer"] | {"librarySHA256": "0" * 64}}, "speedometer validation"),
                (validation | {"speedometer": {key: value for key, value in validation["speedometer"].items() if key != "report"}}, "no retained report"),
            ):
                with self.subTest(error=error):
                    self.pipeline.state["validation"] = broken
                    with self.assertRaisesRegex(RuntimeError, error):
                        self.pipeline.promote()
                    self.assertEqual(self.library.read_bytes(), self.original_bytes)
            self.pipeline.state["validation"] = validation
            (self.stage / "validation.log").write_text("different")
            with self.assertRaisesRegex(RuntimeError, "changed"):
                self.pipeline.promote()
            self.assertEqual(self.library.read_bytes(), self.original_bytes)

    def test_interrupted_waived_promotion_keeps_the_waiver_record(self):
        with self.pipeline.locked():
            candidate = self.prepare_performance_validation()
            self.pipeline.state["promotionPending"] = True
            self.pipeline.save()
            bolt.atomic_copy(candidate, self.library)
        pipeline = self.reopen()
        with pipeline.locked():
            self.assertEqual(pipeline.state["promoted"]["waivedValidation"], ["baseline", "browsing", "layout"])
            pipeline.rollback()
            self.assertEqual(self.library.read_bytes(), self.original_bytes)

    def test_interrupted_promotion_recovers_installed_candidate(self):
        with self.pipeline.locked():
            candidate = self.prepare_validation()
            self.pipeline.state["promotionPending"] = True
            self.pipeline.save()
            bolt.atomic_copy(candidate, self.library)
        pipeline = self.reopen()
        with pipeline.locked():
            self.assertIn("promoted", pipeline.state)
            self.assertNotIn("promotionPending", pipeline.state)
            pipeline.rollback()
            self.assertEqual(self.library.read_bytes(), self.original_bytes)

    def test_runtime_rebuild_is_rejected_without_overwriting_fallback(self):
        with self.pipeline.locked():
            self.pipeline.instrument()
        self.pipeline.manifest_path.write_text(json.dumps({"mode": "performance", "changed": True}))
        with self.assertRaisesRegex(RuntimeError, "rebuilt"):
            with self.reopen().locked():
                pass
        self.assertEqual(self.pipeline.original.read_bytes(), self.original_bytes)

    def test_missing_relocations_are_rejected_before_backup(self):
        self.library.write_bytes(elf_library(relocations=False))
        with self.assertRaisesRegex(RuntimeError, "emit-relocs"):
            with self.pipeline.locked():
                pass
        self.assertFalse(self.pipeline.original.exists())

    def test_bolted_input_cannot_become_original(self):
        self.library.write_bytes(elf_library(bolted=True))
        with self.assertRaisesRegex(RuntimeError, "already"):
            with self.pipeline.locked():
                pass

    def test_explicit_tool_directory_does_not_fall_back_to_path(self):
        with self.assertRaisesRegex(RuntimeError, "requires an executable"):
            bolt.resolve_tool(ROOT, self.stage / "missing", "llvm-bolt")

    def test_bolt_requires_the_full_default_pgo_manifest(self):
        manifest_path = self.prefix / "share/eden-wpe-build.json"
        original = json.loads(manifest_path.read_text())
        invalid_manifests = (
            (original | {"mode": "debug"}, "complete Phase 1 performance patch set"),
            (original | {"framePointers": True}, "complete Phase 1 performance patch set"),
            (original | {"patches": original["patches"][:-1]}, "complete Phase 1 performance patch set"),
            (original | {"patches": []}, "complete Phase 1 performance patch set"),
            (original | {"patchCatalogSHA256": "0" * 64}, "complete Phase 1 performance patch set"),
            (original | {"profileSHA256": "invalid"}, "matching, completed PGO performance profile"),
            (original | {"effectiveProfileSHA256": "invalid"}, "matching, completed PGO performance profile"),
            (original | {"profileTrainingIdentity": pgo.profile_identity(original) | {"embeddedPatchGeneratorSHA256": "6" * 64}}, "PGO profile trained on the prepared source identity"),
            (original | {"profileTrainingIdentity": pgo.profile_identity(original) | {"preparedManagedSourceSHA256": "8" * 64}}, "PGO profile trained on the prepared source identity"),
            (original | {"profileTrainingIdentity": pgo.profile_identity(original) | {"verifyPatches": True}}, "PGO profile trained on the prepared source identity"),
        )
        for invalid, error in invalid_manifests:
            with self.subTest(manifest=invalid):
                manifest_path.write_text(json.dumps(invalid))
                with self.assertRaisesRegex(RuntimeError, error):
                    with self.reopen().locked():
                        pass
                self.assertFalse(self.pipeline.original.exists())
        manifest_path.write_text(json.dumps(original))

    def test_toolchain_requires_one_installation(self):
        first = self.stage / "llvm-install-a/bin"
        second = self.stage / "llvm-install-b/bin"
        first.mkdir(parents=True)
        second.mkdir(parents=True)
        tools = {"llvm-bolt": first / "llvm-bolt", "merge-fdata": second / "merge-fdata"}
        for tool in tools.values():
            tool.write_bytes(b"tool")
            tool.chmod(0o755)
        self.pipeline.tools = None
        with mock.patch.object(bolt.shutil, "which", side_effect=lambda name: str(tools[name])):
            with self.assertRaisesRegex(RuntimeError, "same LLVM installation"):
                self.pipeline.toolchain()


class BoltValidationTest(unittest.TestCase):
    def test_coverage_requires_instructions_and_native_function_statistics(self):
        coverage = bolt.coverage_from_log(COVERAGE)
        self.assertEqual(coverage["totalFunctions"], 1000)
        self.assertEqual(coverage["functionCoveragePercent"], 12.3)
        self.assertEqual(coverage["minimumFunctionCoveragePercent"], 5.0)
        for log in ("", COVERAGE.replace("2000000000", "1"),
                    COVERAGE.split("\n", 1)[1], COVERAGE + "BOLT-WARNING: stale profile\n"):
            with self.subTest(log=log):
                with self.assertRaises(RuntimeError):
                    bolt.coverage_from_log(log)

    def test_function_coverage_rejects_profile_of_only_a_few_functions(self):
        low_coverage = COVERAGE.replace("123 out of 1000", "4 out of 1000").replace("12.3%", "0.4%")
        with self.assertRaisesRegex(RuntimeError, "below the 5.000% minimum"):
            bolt.coverage_from_log(low_coverage)
        accepted_override = COVERAGE.replace("123 out of 1000", "10 out of 1000").replace("12.3%", "1.0%")
        with mock.patch.dict(os.environ, {"EDEN_WPE_BOLT_MINIMUM_FUNCTION_COVERAGE_PERCENT": "1"}):
            coverage = bolt.coverage_from_log(accepted_override)
        self.assertEqual(coverage["functionCoveragePercent"], 1.0)
        for threshold in (0.99, 0, 100.01, float("nan"), "invalid"):
            with self.subTest(threshold=threshold):
                with self.assertRaisesRegex(RuntimeError, "number from 1 to 100"):
                    bolt.coverage_from_log(COVERAGE, threshold)
        inconsistent = COVERAGE.replace("12.3%", "99.0%")
        with self.assertRaisesRegex(RuntimeError, "inconsistent"):
            bolt.coverage_from_log(inconsistent)


class PGOProvenanceTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="eden-pgo-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.build = self.root / "build"
        self.build.mkdir()
        self.runtime = self.root / "runtime"
        self.manifest_path = self.runtime / "share/eden-wpe-build.json"
        self.manifest_path.parent.mkdir(parents=True)
        self.manifest = pgo_identity_manifest()
        self.manifest_path.write_text(json.dumps(self.manifest))
        (self.build / "CMakeCache.txt").write_text(
            "ENABLE_LLVM_PROFILE_GENERATION:BOOL=ON\n"
            f"CMAKE_HOME_DIRECTORY:INTERNAL={self.root / 'source'}\n"
            f"CMAKE_INSTALL_PREFIX:PATH={self.runtime}\n"
        )
        self.profile = self.root / "training.profdata"
        self.profile.write_bytes(b"profile-data")

    def test_profile_identity_captures_registry_and_embedded_patch_generator(self):
        identity = pgo.profile_identity(self.manifest)
        self.assertEqual(identity["patches"], self.manifest["patches"])
        self.assertEqual(identity["embeddedPatchGeneratorSHA256"], "5" * 64)
        changed = self.manifest | {"embeddedPatchGeneratorSHA256": "6" * 64}
        self.assertNotEqual(identity, pgo.profile_identity(changed))
        changed = self.manifest | {"verifyPatches": True}
        self.assertNotEqual(identity, pgo.profile_identity(changed))
        changed = self.manifest | {"preparedManagedSourceSHA256": "8" * 64}
        self.assertNotEqual(identity, pgo.profile_identity(changed))

    def test_training_sidecar_requires_the_matching_profile_generate_manifest(self):
        metadata = pgo.record_training_paths(self.profile, self.build)
        self.assertEqual(metadata["formatVersion"], 2)
        self.assertEqual(metadata["trainingIdentity"], pgo.profile_identity(self.manifest))
        self.assertEqual(metadata["trainingManifest"], str(self.manifest_path.resolve()))
        self.assertEqual(metadata["trainingManifestSHA256"], pgo.digest(self.manifest_path))
        self.manifest_path.write_text(json.dumps(self.manifest | {"mode": "performance"}))
        with self.assertRaisesRegex(RuntimeError, "profile-generate runtime"):
            pgo.record_training_paths(self.profile, self.build)

    def test_profile_preparation_rejects_stale_source_and_missing_provenance(self):
        metadata = pgo.record_training_paths(self.profile, self.build)
        expected = pgo.profile_identity(self.manifest)
        with mock.patch.object(pgo.shutil, "which") as find_tool:
            with self.assertRaisesRegex(RuntimeError, "prepared source identity"):
                pgo.prepare_profile(
                    self.profile, self.root / "source", self.root / "performance-build",
                    [], {}, expected | {"embeddedPatchGeneratorSHA256": "6" * 64},
                )
            find_tool.assert_not_called()
        self.profile.with_suffix(".profdata.json").unlink()
        with self.assertRaisesRegex(RuntimeError, "missing its profile-generate provenance"):
            pgo.prepare_profile(
                self.profile, self.root / "source", self.root / "performance-build", [], {}, expected,
            )

    def test_combined_pair_rejects_top_level_identity_drift(self):
        with tempfile.TemporaryDirectory(prefix="eden-combined-identity-test-") as temporary:
            root = Path(temporary)
            profile = root / "merged.profdata"
            profile.write_bytes(b"profile")
            profile_hash = bolt.digest(profile)
            identity = pgo_identity_manifest("performance")
            for variant, frame_pointers in (("combined", False), ("combined-frame-profile", True)):
                runtime = root / "third_party/wpe" / ("local-performance-" + variant) / "share"
                runtime.mkdir(parents=True)
                manifest = identity | {
                    "variant": variant,
                    "mode": "performance",
                    "framePointers": frame_pointers,
                    "profileSHA256": profile_hash,
                }
                manifest["profileTrainingIdentity"] = pgo.profile_identity(identity)
                (runtime / "eden-wpe-build.json").write_text(json.dumps(manifest))
            build_wpe_combined.verify_pair(root, profile)
            manifest_path = (root / "third_party/wpe/local-performance-combined-frame-profile/share/eden-wpe-build.json")
            manifest = json.loads(manifest_path.read_text())
            manifest["verifyPatches"] = True
            manifest_path.write_text(json.dumps(manifest))
            with self.assertRaisesRegex(RuntimeError, "canonical PGO identity"):
                build_wpe_combined.verify_pair(root, profile)

    def test_layout_comparison_preserves_baseline_failures_but_rejects_new_ones(self):
        baseline = {"fast/a.html": ["PASS"], "fast/b.html": ["TEXT"]}
        bolt.compare_layout_results(baseline, baseline)
        bolt.compare_layout_results(baseline, {name: ["PASS"] for name in baseline})
        for candidate in ({"fast/a.html": ["TEXT"], "fast/b.html": ["TEXT"]},
                          {"fast/a.html": ["PASS"]},
                          {"fast/a.html": ["PASS"], "fast/b.html": ["CRASH"]}):
            with self.subTest(candidate=candidate):
                with self.assertRaises(RuntimeError):
                    bolt.compare_layout_results(baseline, candidate)

    def test_layout_report_requires_completed_executed_tests(self):
        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / "result.json"
            report.write_text('ADD_RESULTS({"tests":{"fast":{"a.html":{"actual":"PASS"}}}});')
            self.assertEqual(bolt.read_layout_results(report), {"fast/a.html": ["PASS"]})
            for value in ({"tests": {}}, {"interrupted": True, "tests": {}},
                          {"tests": {"a.html": {"actual": "SKIP"}}},
                          {"tests": {"fast": ["PASS"]}},
                          {"tests": {"fast": {"a.html": {"actual": 1}}}}):
                report.write_text(json.dumps(value))
                with self.assertRaises(RuntimeError):
                    bolt.read_layout_results(report)

    def test_incomplete_speedometer_run_cannot_supply_training_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / "result.json"
            value = speedometer_result()
            report.write_text(json.dumps(value))
            bolt.require_speedometer(report)
            invalid_values = []
            invalid_values.append(value | {"measuredValues": []})
            invalid_values.append(value | {"accelerated": False})
            invalid_values.append(value | {"textureImports": 0})
            invalid_values.append(value | {"thumbnailRequests": 1})
            invalid_values.append(value | {"thumbnailRequests": None})
            invalid_values.append(value | {"thumbnailRequests": -1})
            invalid_values.append(value | {"thumbnailRequests": 0.0})
            invalid_values.append(value | {"thumbnailRequests": False})
            invalid_values.append({key: item for key, item in value.items() if key != "thumbnailRequests"})
            invalid_values.append(value | {"textureImports": True})
            invalid_values.append(value | {"visibility": "hidden"})
            invalid_values.append(value | {"viewport": [1280, 900, 1]})
            invalid_values.append(value | {"score": {"isValid": True, "mean": float("nan")}})
            missing_suite = json.loads(json.dumps(value))
            del missing_suite["measuredValues"][0]["tests"]["Suite00"]
            invalid_values.append(missing_suite)
            changed_inventory = json.loads(json.dumps(value))
            changed_inventory["measuredValues"][1]["tests"]["DifferentSuite"] = (
                changed_inventory["measuredValues"][1]["tests"].pop("Suite00")
            )
            invalid_values.append(changed_inventory)
            missing_async = json.loads(json.dumps(value))
            missing_async["measuredValues"][0]["tests"]["Suite00"]["tests"]["step"]["tests"].pop("Async")
            invalid_values.append(missing_async)
            nonfinite_total = json.loads(json.dumps(value))
            nonfinite_total["measuredValues"][0]["tests"]["Suite00"]["total"] = float("nan")
            invalid_values.append(nonfinite_total)
            for invalid in invalid_values:
                report.write_text(json.dumps(invalid))
                with self.assertRaises(RuntimeError):
                    bolt.require_speedometer(report)

    def test_browsing_evidence_requires_an_hour_and_page_assertions(self):
        with tempfile.TemporaryDirectory() as directory:
            report = Path(directory) / "result.json"
            value = browsing_result()
            report.write_text(json.dumps(value))
            bolt.require_browsing(report, 3600)
            for invalid, elapsed in (
                (value | {"seconds": 3599}, 3600),
                (value | {"visits": 119}, 3600),
                (value | {"pages": 4}, 3600),
                (value | {"assertions": 119}, 3600),
                (value | {"rendererTerminations": 1}, 3600),
                (value | {"passed": False}, 3600),
                (value, 3599),
            ):
                report.write_text(json.dumps(invalid))
                with self.assertRaises(RuntimeError):
                    bolt.require_browsing(report, elapsed)


class BoltWorkloadTest(unittest.TestCase):
    def arguments(self, role):
        return SimpleNamespace(
            run=role,
            build=Path("/tmp/eden-build"),
            prefix=Path("/tmp/eden-prefix"),
            report=Path("/tmp/eden-report.json"),
            output=Path("/tmp/eden-output"),
            profiles=Path("/tmp/eden-profiles"),
        )

    def test_generated_plan_disables_sandbox_only_for_instrumented_training(self):
        with tempfile.TemporaryDirectory(prefix="eden-bolt-plan-test-") as temporary:
            root = Path(temporary)
            build = root / "build"
            build.mkdir()
            (build / "eden-wpe-bench").touch()
            (build / "eden-wpe-tests").touch()
            checkout = root / "webkit"
            layout_root = checkout / "LayoutTests"
            layout_root.mkdir(parents=True)
            (layout_root / "smoke.html").touch()
            runner = checkout / "Tools/Scripts/run-webkit-tests"
            runner.parent.mkdir(parents=True)
            runner.touch()
            webkit_build = root / "webkit-build"
            webkit_build.mkdir()
            tests = root / "tests.txt"
            tests.write_text("smoke.html\n")
            pages = root / "pages.json"
            pages.write_text(json.dumps(page_set()))
            plan_path = root / "plan.json"
            arguments = SimpleNamespace(
                build=build,
                webkit_checkout=checkout,
                webkit_build=webkit_build,
                layout_test_list=tests,
                pages=pages,
                plan=plan_path,
            )
            bolt_workloads.create_plan(arguments)
            plan = json.loads(plan_path.read_text())
        self.assertEqual(
            plan["training"]["environment"],
            {"WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS": "1"},
        )
        for role in ("speedometer", "smoke", "browsing", "layout"):
            self.assertNotIn("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS", plan[role].get("environment", {}))

    def test_training_only_forwards_explicit_sandbox_opt_out_and_bolt_identity(self):
        variables = {
            "WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS": "1",
            "EDEN_WPE_BOLT_RUNTIME": "/wrong/runtime",
            "EDEN_WPE_BOLT_REPORT": "/wrong/report",
            "LD_LIBRARY_PATH": "/wrong/lib",
        }
        with mock.patch.dict(os.environ, variables, clear=True):
            with mock.patch.object(bolt_workloads.subprocess, "run") as run:
                bolt_workloads.run(self.arguments("training"))
        command = run.call_args.args[0]
        environment = run.call_args.kwargs["env"]
        self.assertIn("--allow-webkit-sandbox-disabled", command)
        self.assertEqual(environment["WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS"], "1")
        self.assertEqual(environment["EDEN_WPE_BOLT_RUNTIME"], "/tmp/eden-prefix")
        self.assertEqual(environment["EDEN_WPE_BOLT_REPORT"], "/tmp/eden-report.json")
        self.assertNotIn("LD_LIBRARY_PATH", environment)

    def test_speedometer_discards_inherited_sandbox_opt_out(self):
        variables = {"WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS": "1"}
        with mock.patch.dict(os.environ, variables, clear=True):
            with mock.patch.object(bolt_workloads.subprocess, "run") as run:
                bolt_workloads.run(self.arguments("speedometer"))
        command = run.call_args.args[0]
        environment = run.call_args.kwargs["env"]
        self.assertNotIn("--allow-webkit-sandbox-disabled", command)
        self.assertNotIn("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS", environment)

    def test_page_set_requires_distinct_safe_http_pages(self):
        valid = page_set()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "pages.json"
            path.write_text(json.dumps(valid))
            self.assertEqual(bolt_workloads.read_page_set(path), valid)
            invalid_sets = (
                valid[:4],
                valid[:-1] + [valid[0]],
                valid[:-1] + [valid[0] | {"url": "https://page0.example.test:443/article"}],
                valid[:-1] + [valid[-1] | {"url": "https://user:password@example.test/"}],
                valid[:-1] + [valid[-1] | {"url": "file:///etc/passwd"}],
                valid[:-1] + [valid[-1] | {"url": "https://example.test:65536/"}],
                valid[:-1] + [valid[-1] | {"url": "https://example.test:"}],
                valid[:-1] + [valid[-1] | {"selector": "  "}],
                valid[:-1] + [valid[-1] | {"expectedText": "  "}],
            )
            for pages in invalid_sets:
                with self.subTest(pages=pages):
                    path.write_text(json.dumps(pages))
                    with self.assertRaises(RuntimeError):
                        bolt_workloads.read_page_set(path)


class AdoptedPerformancePatchTest(unittest.TestCase):
    def test_registry_selects_one_canvas_implementation_and_both_phase_one_apis(self):
        defaults = patch_names(resolve_patches(False))
        self.assertEqual(defaults, patch_names(resolve_patches(True)))
        self.assertIn("canvas-thread", defaults)
        self.assertNotIn("canvas-replay", defaults)
        self.assertIn("process-prewarm", defaults)
        self.assertIn("navigation-preconnect", defaults)

    def test_process_prewarm_patch_binds_one_data_store_and_rejects_cross_session_use(self):
        pool = """    for (Ref process : m_prewarmedProcesses) {
        if (process->lockdownMode() != lockdownMode || process->enhancedSecurity() != enhancedSecurity)
            continue;
    }
    bool didRemove = m_prewarmedProcesses.remove(*prewarmedProcess);
#if PLATFORM(GTK) || PLATFORM(WPE)
    if (m_sandboxEnabled)
        return nullptr;
#endif
    prewarmedProcess->setWebsiteDataStore(websiteDataStore);
void WebProcessPool::prewarmProcess()
{
    if (m_prewarmedProcesses.computeSize() >= prewarmedProcessCountLimit())
        return;
    if (WebProcessProxy::hasReachedProcessCountLimit())
        return;
    auto lockdownMode = lockdownModeEnabledBySystem() ? WebProcessProxy::LockdownMode::Enabled : WebProcessProxy::LockdownMode::Disabled;
    auto enhancedSecurity = EnhancedSecurity::Disabled;
    createNewWebProcess(nullptr, lockdownMode, enhancedSecurity, WebProcessProxy::EnableWebAssemblyDebugger::No, WebProcessProxy::IsPrewarmed::Yes);
}
void WebProcessPool::processDidFinishLaunching(WebProcessProxy& process)
{
    if (process.isPrewarmed() && m_prewarmedProcesses.computeSize() < prewarmedProcessCountLimit())
        dispatchPrewarm();
}
unsigned WebProcessPool::prewarmedProcessCountLimit() const
{
    return 4;
}
HashSet<ProcessID> WebProcessPool::prewarmedProcessIdentifiers()
{
    HashSet<ProcessID> pids;
    for (Ref process : m_prewarmedProcesses) {
        if (auto pid = process->processID())
            pids.add(pid);
    }
    return pids;
}
"""
        patched = process_prewarm.pool_patch(pool)
        self.assertIn("m_prewarmingDataStore.get() != &dataStore", patched)
        self.assertIn("process->websiteDataStore() != &websiteDataStore", patched)
        self.assertIn("MemoryPressureHandler::singleton().isUnderMemoryPressure()", patched)
        self.assertIn("createNewWebProcess(m_prewarmingDataStore.get()", patched)
        self.assertIn("return livePrewarmedProcessCount() > 0;", patched)
        self.assertIn("process->isShuttingDown()", patched)
        self.assertIn("livePrewarmedProcessCount() >= prewarmedProcessCountLimit()", patched)
        self.assertIn("process.isPrewarmed() && livePrewarmedProcessCount() < prewarmedProcessCountLimit()", patched)
        self.assertIn("state() != AuxiliaryProcessProxy::State::Running", patched)
        self.assertIn("return 1;", patched)
        self.assertNotIn("if (m_sandboxEnabled)\n        return nullptr;", patched)
        header = process_prewarm.pool_header_patch(
            "    void prewarmProcess();\n"
            "    WeakHashSet<WebProcessProxy> m_prewarmedProcesses;\n"
            "    unsigned prewarmedProcessCountLimit() const;\n"
        )
        self.assertIn("unsigned livePrewarmedProcessCount() const;", header)
        proxy_header = process_prewarm.proxy_header_patch("    void shutDown();\n")
        self.assertIn("bool isShuttingDown() const", proxy_header)

    def test_context_disable_clears_prewarms_and_view_preconnect_is_https_only(self):
        context = process_prewarm.context_patch(
            "    configuration->setUsesWebProcessCache(true);\n"
        )
        self.assertIn("setIsAutomaticProcessWarmingEnabled(true)", context)
        self.assertIn("if (!enabled)", context)
        self.assertIn("pool.clearPrewarmedProcesses()", context)
        self.assertIn("prewarmProcessForDataStore(store)", context)
        view = navigation_preconnect.view_patch('#include "APIData.h"\n')
        self.assertIn('#include <WebCore/ResourceRequest.h>', view)
        self.assertIn('url.protocolIs("https"_s)', view)
        self.assertIn('url.setPath("/"_s)', view)
        self.assertIn("url.removeQueryAndFragmentIdentifier()", view)
        self.assertIn("getPage(view).preconnectTo", view)


def speedometer_result():
    iterations = []
    for _ in range(bolt.SPEEDOMETER_ITERATIONS):
        suites = {}
        for index in range(bolt.SPEEDOMETER_SUITE_COUNT):
            suites[f"Suite{index:02}"] = {
                "total": 12.5,
                "tests": {"step": {"tests": {"Async": 4.0}}},
            }
        iterations.append({"tests": suites})
    return {
        "score": {"isValid": True, "mean": 25},
        "measuredValues": iterations,
        "viewport": list(bolt.SPEEDOMETER_VIEWPORT),
        "accelerated": True,
        "textureImports": 10,
        "thumbnailRequests": 0,
        "visibility": "visible",
    }


def browsing_result():
    return {
        "seconds": 3600,
        "visits": 120,
        "pages": 5,
        "assertions": 1080,
        "rendererTerminations": 0,
        "passed": True,
    }


def pgo_identity_manifest(mode="profile-generate"):
    return {
        "mode": mode,
        "version": "2.53.92",
        "sourceSHA256": "1" * 64,
        "patches": list(patch_names(resolve_patches(True))),
        "patchCatalogSHA256": bolt.digest(ROOT / "scripts/wpe/performance_patches.py"),
        "adoptedPatchSources": {"process_prewarm.py": "3" * 64},
        "canvasReplayPatch": True,
        "canvasReplaySources": {"canvas_replay.py": "4" * 64},
        "hugePageAlignment": True,
        "verifyPatches": False,
        "embeddedPatchGeneratorSHA256": "5" * 64,
        "preparedManagedSourceSHA256": "7" * 64,
        "compiler": "clang version 21",
    }


def page_set():
    return [
        {
            "url": f"https://page{index}.example.test/article",
            "selector": "main h1",
            "expectedText": f"Article {index}",
        }
        for index in range(5)
    ]


if __name__ == "__main__":
    unittest.main()
