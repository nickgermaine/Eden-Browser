import importlib.util
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from wpe.jsc_experiments import PATCHES
from wpe.engine_experiments import without_comments


class JavaScriptCorePatchTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = ROOT / 'third_party/wpe/source/wpewebkit-2.53.92-jsc-experiments'
        archive = ROOT / 'third_party/wpe/packages/wpewebkit-2.53.92.tar.xz'
        if not archive.exists():
            raise unittest.SkipTest('The pinned WebKit source archive is required')
        with tarfile.open(archive) as package:
            cls.originals = {path: package.extractfile('wpewebkit-2.53.92/' + path).read().decode() for path in PATCHES}

    def test_generated_object_constructor_table_is_preserved(self):
        path = 'Source/JavaScriptCore/runtime/ObjectConstructor.cpp'
        original = self.originals[path]
        candidate = without_comments(PATCHES[path](original))
        generator = self.source / 'Source/JavaScriptCore/create_hash_table'
        with tempfile.TemporaryDirectory() as temporary:
            file = Path(temporary) / 'ObjectConstructor.cpp'
            file.write_text(original)
            expected = subprocess.check_output(['perl', str(generator), str(file)], text=True)
            file.write_text(candidate)
            actual = subprocess.check_output(['perl', str(generator), str(file)], text=True)
        self.assertEqual(actual, expected)
        self.assertIn('objectConstructorTable', actual)
        self.assertIn('ObjectAssignIntrinsic', actual)

    def test_unmatched_source_is_rejected(self):
        for path, patch in PATCHES.items():
            with self.subTest(path=path), self.assertRaises((RuntimeError, ValueError)):
                patch('unrelated source')

    def test_scoring_control_uses_requested_baseline(self):
        spec = importlib.util.spec_from_file_location('wpe_sweep', ROOT / 'scripts/bench/wpe/sweep.py')
        sweep = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(sweep)
        records = [{'configuration': name, 'summary': {'score': score, 'asyncStalls': 0}} for name, score in [('canvas-replay-2', 25), ('jsc-all', 26), ('default', 20)]]
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary)
            sweep.write_summary(output, records, {'control': 'canvas-replay-2'})
            import json
            result = json.loads((output / 'summary.json').read_text())
        self.assertAlmostEqual(result['configurations']['jsc-all']['changePercent'], 4)
        self.assertEqual(result['configurations']['canvas-replay-2']['changePercent'], 0)


if __name__ == '__main__':
    unittest.main()
