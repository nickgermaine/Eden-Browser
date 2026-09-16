import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

root = Path(__file__).resolve().parents[1]
specification = importlib.util.spec_from_file_location('wpe_pgo', root / 'scripts/wpe/pgo.py')
pgo = importlib.util.module_from_spec(specification)
specification.loader.exec_module(pgo)


class ProfilePathsTest(unittest.TestCase):
    def test_rewrites_function_and_indirect_target_names(self):
        text = '/train/part.cpp;work\n# Func Hash:\n42\n# Counter Values:\n1000\n/train/part.cpp;work:1000\n'
        result, counts = pgo.normalize_symbols(text, [('/train', '/use')])
        self.assertEqual(result, text.replace('/train/', '/use/'))
        self.assertEqual(counts, {'remappedFunctions': 1, 'remappedReferences': 1})

    def test_keeps_unrelated_paths_and_counter_values(self):
        text = '/training/part.cpp;work\n# Func Hash:\n42\n1000\n'
        result, counts = pgo.normalize_symbols(text, [('/train', '/use')])
        self.assertEqual(result, text)
        self.assertEqual(counts, {'remappedFunctions': 0, 'remappedReferences': 0})

    def test_rejects_colliding_names(self):
        text = '/train/a.cpp;work\n# Func Hash:\n1\n/use/a.cpp;work\n# Func Hash:\n2\n'
        with self.assertRaises(RuntimeError):
            pgo.normalize_symbols(text, [('/train', '/use')])

    def test_keeps_multiple_hashes_of_the_same_function(self):
        text = '/train/a.cpp;work\n# Func Hash:\n1\n/train/a.cpp;work\n# Func Hash:\n2\n'
        result, counts = pgo.normalize_symbols(text, [('/train', '/use')])
        self.assertEqual(result, text.replace('/train/', '/use/'))
        self.assertEqual(counts['remappedFunctions'], 2)

    def test_compiler_applies_counts_and_promotes_indirect_target(self):
        compiler = shutil.which('clang++')
        tool = shutil.which('llvm-profdata')
        if not compiler or not tool:
            self.skipTest('Clang and llvm-profdata are required')
        with tempfile.TemporaryDirectory(prefix='eden-pgo-test-') as temporary:
            stage = Path(temporary)
            training = stage / 'train'
            optimized = stage / 'use'
            training.mkdir()
            optimized.mkdir()
            source = 'static __attribute__((noinline)) int work(int a) { return a > 6 ? a * 7 : a - 8; }\nint (*volatile target)(int)=work;\nint main(int argc, char**) { int result=0; for (int n=0; n<argc*1000; ++n) result+=target(n); return result==7; }\n'
            for directory in [training, optimized]:
                (directory / 'part.cpp').write_text(source)
            environment = os.environ.copy()
            environment['LLVM_PROFILE_FILE'] = str(training / 'data.profraw')
            def run(arguments):
                return subprocess.run(arguments, env=environment, capture_output=True, text=True, check=True).stdout
            run([compiler, '-O2', '-fprofile-generate', str(training / 'part.cpp'), '-o', str(training / 'app')])
            run([str(training / 'app')])
            profile = stage / 'input.profdata'
            run([tool, 'merge', '--num-threads=1', str(training / 'data.profraw'), '-o', str(profile)])
            (training / 'CMakeCache.txt').write_text('ENABLE_LLVM_PROFILE_GENERATION:BOOL=ON\nCMAKE_HOME_DIRECTORY:INTERNAL=' + str(training) + '\n')
            pgo.record_training_paths(profile, training)
            original_hash = pgo.digest(profile)
            prepared, metadata = pgo.prepare_profile(profile, optimized, optimized, [], environment)
            self.assertEqual(metadata['remappedFunctions'], 1)
            self.assertEqual(metadata['remappedReferences'], 1)
            self.assertEqual(pgo.digest(profile), original_hash)
            evidence = run([tool, 'show', '--all-functions', '--counts', '--ic-targets', str(prepared)])
            self.assertIn(str(optimized / 'part.cpp') + ';_ZL4worki', evidence)
            self.assertNotIn('External Symbol', evidence)
            self.assertIn('1000', evidence)
            ir = stage / 'result.ll'
            run([compiler, '-O2', '-fprofile-use=' + str(prepared), '-S', '-emit-llvm', str(optimized / 'part.cpp'), '-o', str(ir)])
            contents = ir.read_text()
            self.assertRegex(contents, r'define internal[^\n]*_ZL4worki[^\n]*!prof')
            self.assertIn('!"function_entry_count", i64 1000', contents)
            self.assertIn('icmp eq ptr', contents)
            timestamp = prepared.stat().st_mtime_ns
            self.assertEqual(pgo.prepare_profile(profile, optimized, optimized, [], environment), (prepared, metadata))
            self.assertEqual(prepared.stat().st_mtime_ns, timestamp)
            record = optimized / 'eden-pgo-paths.json'
            stale = json.loads(record.read_text())
            stale['normalizerSHA256'] = 'previous-normalizer'
            record.write_text(json.dumps(stale))
            self.assertEqual(pgo.prepare_profile(profile, optimized, optimized, [], environment), (prepared, metadata))
            self.assertEqual(prepared.stat().st_mtime_ns, timestamp)
            profile.write_bytes(profile.read_bytes() + b'changed')
            with self.assertRaises(RuntimeError):
                pgo.prepare_profile(profile, optimized, optimized, [], environment)


if __name__ == '__main__':
    unittest.main()
