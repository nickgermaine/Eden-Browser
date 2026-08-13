import importlib.util
import pathlib
import unittest


BENCH_PATH = pathlib.Path(__file__).with_name("bench.py")
SPEC = importlib.util.spec_from_file_location("eden_bench", BENCH_PATH)
BENCH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BENCH)


class BenchTest(unittest.TestCase):
    def test_discards_warmup_from_each_process(self):
        outputs = ["input=90\ninput=1\ninput=2", "input=80\ninput=3\ninput=4"]
        self.assertEqual(BENCH.measured_driver_runs(outputs, "input"), [[1.0, 2.0], [3.0, 4.0]])

    def test_phase_delays_cover_one_refresh(self):
        delays = [BENCH.phase_delay_seconds(index) for index in range(BENCH.PHASE_SAMPLE_COUNT)]
        self.assertEqual(len(set(delays)), BENCH.PHASE_SAMPLE_COUNT)
        self.assertEqual(min(delays), 0.0)
        self.assertLess(max(delays), 1.0 / BENCH.TARGET_REFRESH_HZ)


if __name__ == "__main__":
    unittest.main()
