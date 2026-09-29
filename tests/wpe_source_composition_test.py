from argparse import Namespace
import hashlib
import io
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from build_wpe import EMBEDDED_PATHS, embedded_patches, prepared_managed_source_digest
from build_wpe_combined import build_commands, verify_pair
from wpe.adopted_patches import load, prepare_adopted_patches, render
from wpe.patch_support import without_comments
from wpe.performance_patches import resolve_patches, selections
from wpe.pgo import profile_identity


class SourceCompositionTest(unittest.TestCase):
    def test_generated_fixtures_create_directories_missing_from_release_archive(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source"
            source.mkdir()
            payload = root / "fixture.html"
            payload.write_text("<p>fixture</p>\n")
            target = "LayoutTests/fast/css/eden-fixture.html"
            archive = root / "third_party/wpe/packages/wpewebkit-test.tar.xz"
            archive.parent.mkdir(parents=True)
            with tarfile.open(archive, "w:xz"):
                pass
            with patch("wpe.adopted_patches.load", return_value=({}, {target: payload}, [])):
                prepare_adopted_patches(root, source, "test", True)
                self.assertEqual((source / target).read_text(), payload.read_text())
                payload.write_text("<p>updated fixture</p>\n")
                prepare_adopted_patches(root, source, "test", True)
                self.assertEqual((source / target).read_text(), payload.read_text())
                sibling = (source / target).with_name("unrelated.html")
                sibling.write_text("preserve")
                prepare_adopted_patches(root, source, "test", False)
                self.assertFalse((source / target).exists())
                self.assertEqual(sibling.read_text(), "preserve")

    def test_prepared_source_digest_tracks_generated_contents(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory)
            for name, contents in (("embedded.cpp", "first"), ("adopted.cpp", "second"),
                                   ("generated.h", "third")):
                (source / name).write_text(contents)
            with patch("build_wpe.EMBEDDED_PATHS", ["embedded.cpp"]), patch(
                "build_wpe.load_adopted_patches",
                return_value=({"adopted.cpp": []}, {"generated.h": source / "generated.h"}, []),
            ):
                original = prepared_managed_source_digest(source, ("candidate",), False)
                (source / "embedded.cpp").write_text("changed")
                self.assertNotEqual(original, prepared_managed_source_digest(source, ("candidate",), False))
                (source / "embedded.cpp").write_text("first")
                (source / "generated.h").write_text("changed")
                self.assertNotEqual(original, prepared_managed_source_digest(source, ("candidate",), False))

    def test_inventory_is_explicit_and_complete(self):
        selected = selections(resolve_patches(True), "adopted")
        patches, files, sources = load(selected)
        self.assertIn("Source/WebKit/UIProcess/API/glib/WebKitWebView.cpp", patches)
        self.assertIn("Source/WebKit/WebProcess/WebPage/glib/WebPageGLib.cpp", patches)
        self.assertIn("Source/JavaScriptCore/yarr/YarrJIT.cpp", patches)
        self.assertTrue(all(path.is_file() for path in sources))
        self.assertEqual(len(sources), len(set(sources)))
        self.assertTrue(files)
        self.assertEqual(load([])[0], {})
        with self.assertRaisesRegex(RuntimeError, "Unknown"):
            load(["missing"])
        with self.assertRaisesRegex(RuntimeError, "duplicate"):
            load([selected[0], selected[0]])

    def test_reset_then_embedding_then_performance_preserves_both_changes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source"
            source.mkdir()
            target = "WebKitWebView.cpp"
            destination = source / target
            destination.write_text("stale candidate")
            archive = root / "third_party/wpe/packages/wpewebkit-test.tar.xz"
            archive.parent.mkdir(parents=True)
            with tarfile.open(archive, "w:xz") as package:
                payload = b"original\n"
                member = tarfile.TarInfo("wpewebkit-test/" + target)
                member.size = len(payload)
                package.addfile(member, io.BytesIO(payload))
            transformations = {target: [lambda text: text + "preconnect\n", lambda text: text + "second\n"]}
            with patch("wpe.adopted_patches.load", return_value=(transformations, {}, [])), patch(
                "wpe.adopted_patches.render", side_effect=lambda root, text, patches, path: patches[1](patches[0](text))
            ):
                for _ in range(2):
                    prepare_adopted_patches(root, source, "test", False)
                    self.assertEqual(destination.read_text(), "original\n")
                    destination.write_text(destination.read_text() + "embedding\n")
                    prepare_adopted_patches(root, source, "test", True)
                    self.assertEqual(destination.read_text(), "original\nembedding\npreconnect\nsecond\n")
                prepare_adopted_patches(root, source, "test", False)
                self.assertEqual(destination.read_text(), "original\n")

    def test_complete_registry_composes_against_pinned_archive(self):
        archive = ROOT / "third_party/wpe/packages/wpewebkit-2.53.92.tar.xz"
        if not archive.is_file():
            self.skipTest("The pinned WebKit archive is unavailable")
        selected = selections(resolve_patches(True), "adopted")
        patches, files, sources = load(selected)
        originals = {}
        with tarfile.open(archive) as package:
            for path in set(patches) | set(EMBEDDED_PATHS):
                stream = package.extractfile("wpewebkit-2.53.92/" + path)
                self.assertIsNotNone(stream, path)
                originals[path] = stream.read().decode()
        embedded = embedded_patches({path: originals[path] for path in EMBEDDED_PATHS}, "2.53.92")
        for path, transformations in patches.items():
            starting_text = embedded.get(path, originals[path])
            generated = render(ROOT, starting_text, transformations, path)
            self.assertNotEqual(generated, starting_text, path)
            self.assertEqual(without_comments(generated), generated, path)
        for path, payload in files.items():
            self.assertTrue(payload.is_file(), path)
            self.assertTrue(payload.read_bytes(), path)
        self.assertTrue(all(path.is_file() for path in sources))

    def test_pair_uses_one_profile_and_budget(self):
        arguments = Namespace(mode="performance", profile=Path("/tmp/combined.profdata"), jobs=12,
                              link_jobs=3, cpus="0-5,16-21", prepare_only=False, configure_only=False,
                              verify_patches=False)
        commands = build_commands(ROOT, arguments)
        self.assertEqual(len(commands), 2)
        for command in commands:
            self.assertIn("--experiments", command)
            self.assertEqual(command[command.index("--jobs") + 1], "12")
            self.assertEqual(command[command.index("--link-jobs") + 1], "3")
            self.assertEqual(command[command.index("--profile") + 1], "/tmp/combined.profdata")
        self.assertNotIn("--frame-pointers", commands[0])
        self.assertIn("--frame-pointers", commands[1])
        self.assertEqual(commands[0][commands[0].index("--variant") + 1], "combined")
        self.assertEqual(commands[1][commands[1].index("--variant") + 1], "combined-frame-profile")
        arguments.mode = "profile-generate"
        arguments.profile = None
        self.assertEqual(len(build_commands(ROOT, arguments)), 1)

    def test_pair_refuses_different_source_or_profile_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            profile = root / "combined.profdata"
            profile.write_bytes(b"merged phase one profile")
            profile_hash = hashlib.sha256(profile.read_bytes()).hexdigest()
            patches = [patch.name for patch in resolve_patches(True)]
            base = {
                "mode": "performance",
                "patches": patches,
                "profileSHA256": profile_hash,
                "version": "2.53.92",
                "sourceSHA256": hashlib.sha256(b"pinned-source").hexdigest(),
                "patchCatalogSHA256": hashlib.sha256(b"registry").hexdigest(),
                "adoptedPatchSources": {
                    "canvas_thread.py": hashlib.sha256(b"payload").hexdigest(),
                },
                "canvasReplayPatch": False,
                "canvasReplaySources": {},
                "hugePageAlignment": False,
                "verifyPatches": False,
                "embeddedPatchGeneratorSHA256": hashlib.sha256(b"embedded-generator").hexdigest(),
                "preparedManagedSourceSHA256": hashlib.sha256(b"prepared-source").hexdigest(),
                "compiler": "clang 22.1.8",
            }
            base["profileTrainingIdentity"] = profile_identity(base)
            paths = []
            for variant, frame_pointers in (("combined", False), ("combined-frame-profile", True)):
                path = root / "third_party/wpe" / ("local-performance-" + variant) / "share/eden-wpe-build.json"
                path.parent.mkdir(parents=True)
                path.write_text(json.dumps({**base, "variant": variant, "framePointers": frame_pointers}))
                paths.append(path)
            self.assertEqual(len(verify_pair(root, profile)), 2)
            changed = {**base, "variant": "combined-frame-profile", "framePointers": True,
                       "sourceSHA256": hashlib.sha256(b"different-source").hexdigest()}
            changed["profileTrainingIdentity"] = {
                **base["profileTrainingIdentity"],
                "sourceSHA256": changed["sourceSHA256"],
            }
            paths[1].write_text(json.dumps(changed))
            with self.assertRaisesRegex(RuntimeError, "canonical PGO identity"):
                verify_pair(root, profile)
            changed["sourceSHA256"] = base["sourceSHA256"]
            changed["profileSHA256"] = "stale-profile"
            paths[1].write_text(json.dumps(changed))
            with self.assertRaisesRegex(RuntimeError, "identity mismatch"):
                verify_pair(root, profile)


if __name__ == "__main__":
    unittest.main()
