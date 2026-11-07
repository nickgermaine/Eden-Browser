import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPECIFICATION = importlib.util.spec_from_file_location('wpe_sweep', ROOT / 'scripts/bench/wpe/sweep.py')
SWEEP = importlib.util.module_from_spec(SPECIFICATION)
SPECIFICATION.loader.exec_module(SWEEP)


class SweepBenchmarkCommandTest(unittest.TestCase):
    def test_benchmark_command_uses_the_scoring_viewport(self):
        command = SWEEP.benchmark_command(
            ROOT,
            Path('/tmp/build'),
            Path('/tmp/result.json'),
            'default-1',
        )
        separator = command.index('--')

        self.assertEqual(
            command[separator + 1:],
            [
                '--output',
                '/tmp/result.json',
                '--label',
                'default-1',
                '--viewport-width',
                '1600',
                '--viewport-height',
                '1200',
            ],
        )

    def test_adopted_patch_switches_are_available_without_legacy_manifest_flags(self):
        available = SWEEP.available_configurations({
            'patches': ['canvas-thread', 'jsc-object-assign-clone', 'simple-text'],
            'canvasReplayPatch': False,
        })

        self.assertIn('default', available)
        self.assertIn('canvas-thread-off', available)
        self.assertIn('jsc-assign-off', available)
        self.assertIn('simple-text-off', available)
        self.assertNotIn('canvas-replay-2', available)
        self.assertNotIn('jsc-rest-off', available)
        self.assertNotIn('jsc-json-keys-off', available)
        self.assertNotIn('text-cache-keys-off', available)

        extended = SWEEP.available_configurations({
            'patches': ['jsc-json-property-keys', 'text-cache-key-length'],
        })
        self.assertIn('jsc-json-keys-off', extended)
        self.assertIn('text-cache-keys-off', extended)

    def test_legacy_experiment_switches_require_matching_manifest_flags(self):
        available = SWEEP.available_configurations({
            'engineExperiments': True,
            'jscExperiments': True,
            'canvasReplayPatch': True,
        })

        self.assertIn('jsc-assign', available)
        self.assertIn('canvas-replay-2', available)
        self.assertNotIn('canvas-thread-off', available)

    def test_four_addition_comparisons_enable_exactly_one_change_on_the_same_control(self):
        patches = set(SWEEP.COMBINED3_ADDITION_SWITCHES)
        available = SWEEP.available_configurations({'patches': list(patches)})
        baseline = SWEEP.CONFIGURATIONS['combined2-control']
        self.assertEqual(set(baseline.values()), {'0'})
        self.assertEqual(len(baseline), 4)
        self.assertIn('combined2-control', available)
        for patch, switch in SWEEP.COMBINED3_ADDITION_SWITCHES.items():
            with self.subTest(patch=patch):
                name = 'combined2-plus-' + patch
                self.assertIn(name, available)
                settings = SWEEP.CONFIGURATIONS[name]
                self.assertEqual(settings.keys(), baseline.keys())
                self.assertEqual({key for key in settings if settings[key] != baseline[key]}, {switch})
                self.assertEqual(settings[switch], '1')
                missing = SWEEP.available_configurations({'patches': list(patches - {patch})})
                self.assertNotIn('combined2-control', missing)
                self.assertNotIn(name, missing)
                self.assertNotIn(patch + '-off', missing)


if __name__ == '__main__':
    unittest.main()
