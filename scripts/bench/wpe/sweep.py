import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import re
import statistics
import subprocess
import sys
import tempfile
import time


CONFIGURATIONS = {
    'default': {},
    'jsc-assign': {'WEBKIT_SKIA_CANVAS_REPLAY_THREADS': '2', 'WEBKIT_EDEN_JSC_ASSIGN_CLONE': '1'},
    'jsc-rest': {'WEBKIT_SKIA_CANVAS_REPLAY_THREADS': '2', 'WEBKIT_EDEN_JSC_REST_CLONE': '1'},
    'jsc-regexp': {'WEBKIT_SKIA_CANVAS_REPLAY_THREADS': '2', 'WEBKIT_EDEN_JSC_REGEXP_ALTERNATIVES': '1'},
    'jsc-all': {
        'WEBKIT_SKIA_CANVAS_REPLAY_THREADS': '2',
        'WEBKIT_EDEN_JSC_ASSIGN_CLONE': '1',
        'WEBKIT_EDEN_JSC_REST_CLONE': '1',
        'WEBKIT_EDEN_JSC_REGEXP_ALTERNATIVES': '1',
    },
    'ack-import': {'EDEN_WPE_FRAME_ACK': 'import'},
    'msaa-0': {'WEBKIT_SKIA_MSAA_SAMPLE_COUNT': '0'},
    'msaa-2': {'WEBKIT_SKIA_MSAA_SAMPLE_COUNT': '2'},
    'msaa-4': {'WEBKIT_SKIA_MSAA_SAMPLE_COUNT': '4'},
    'dynamic-msaa': {'WEBKIT_SKIA_ENABLE_DYNAMIC_MSAA': '1'},
    'canvas-cpu': {'EDEN_WPE_CANVAS_ACCELERATION': '0'},
    'editor-window': {'WEBKIT_EDEN_EDITOR_WINDOW': '1024'},
    'early-scene': {'WEBKIT_EDEN_EARLY_SCENE_COMPLETION': '1'},
    'display-sync': {'EDEN_WPE_DISPLAY_SYNC': '1'},
    'helper-nice3': {'WEBKIT_EDEN_HELPER_NICE': '3'},
    'text-huge-pages': {'WEBKIT_EDEN_TEXT_HUGE_PAGES': '1'},
    'jit-huge-pages': {'WEBKIT_EDEN_JIT_HUGE_PAGES': '1'},
    'scaled-font-reuse': {'WEBKIT_EDEN_REUSE_SCALED_FONT': '1'},
    'paint-partition': {'WEBKIT_EDEN_PAINT_PARTITION_RATIO': '2'},
    'engine-all': {
        'WEBKIT_SKIA_CANVAS_REPLAY_THREADS': '2',
        'WEBKIT_EDEN_EDITOR_WINDOW': '1024',
        'WEBKIT_EDEN_EARLY_SCENE_COMPLETION': '1',
        'EDEN_WPE_DISPLAY_SYNC': '1',
        'WEBKIT_EDEN_HELPER_NICE': '3',
        'WEBKIT_EDEN_TEXT_HUGE_PAGES': '1',
        'WEBKIT_EDEN_JIT_HUGE_PAGES': '1',
        'WEBKIT_EDEN_REUSE_SCALED_FONT': '1',
        'WEBKIT_EDEN_PAINT_PARTITION_RATIO': '2',
    },
    'canvas-replay-1': {'WEBKIT_SKIA_CANVAS_REPLAY_THREADS': '1'},
    'canvas-replay-2': {'WEBKIT_SKIA_CANVAS_REPLAY_THREADS': '2'},
    'canvas-replay-2-batch64': {'WEBKIT_SKIA_CANVAS_REPLAY_THREADS': '2', 'WEBKIT_SKIA_CANVAS_REPLAY_BATCH': '64'},
    'paint-1': {'WEBKIT_SKIA_CPU_PAINTING_THREADS': '1'},
    'paint-2': {'WEBKIT_SKIA_CPU_PAINTING_THREADS': '2'},
    'paint-4': {'WEBKIT_SKIA_CPU_PAINTING_THREADS': '4'},
    'ddl-off': {'WEBKIT_SKIA_ENABLE_DDL': '0'},
    'cpu-rendering': {'WEBKIT_SKIA_ENABLE_CPU_RENDERING': '1'},
}


def cpu_counters():
    values = [int(value) for value in Path('/proc/stat').read_text().splitlines()[0].split()[1:9]]
    return sum(values), values[3] + values[4]


def conditions(maximum_busy_fraction):
    policies = sorted(Path('/sys/devices/system/cpu/cpufreq').glob('policy*'))
    governors = {policy.name: (policy / 'scaling_governor').read_text().strip() for policy in policies}
    preferences = {
        policy.name: (policy / 'energy_performance_preference').read_text().strip()
        for policy in policies if (policy / 'energy_performance_preference').exists()
    }
    before = cpu_counters()
    time.sleep(1)
    after = cpu_counters()
    total = after[0] - before[0]
    busy = 1 - (after[1] - before[1]) / total if total > 0 else 1
    temperatures = {}
    for sensor in Path('/sys/class/hwmon').glob('hwmon*'):
        try:
            device = (sensor / 'name').read_text().strip()
            if device not in {'k10temp', 'coretemp', 'amdgpu'}:
                continue
            for value in sensor.glob('temp*_input'):
                label = value.with_name(value.name.replace('_input', '_label'))
                name = label.read_text().strip() if label.exists() else value.stem
                temperatures[f'{device}/{sensor.name}/{name}'] = int(value.read_text()) / 1000
        except (OSError, ValueError):
            continue
    return {
        'governors': governors,
        'energyPreferences': preferences,
        'busyFraction': busy,
        'temperaturesCelsius': temperatures,
        'performanceGovernor': bool(governors) and all(value == 'performance' for value in governors.values()),
        'idle': busy <= maximum_busy_fraction,
    }


def summarize_run(result):
    iterations = result['measuredValues']
    if len(iterations) != 10 or not result['score']['isValid']:
        raise ValueError('Expected a valid, complete ten-iteration Speedometer run')
    if result['viewport'] != [1600, 1200, 1] or result['visibility'] != 'visible':
        raise ValueError('The benchmark viewport or visibility changed')
    if not result['accelerated'] or result['textureImports'] <= 0:
        raise ValueError('The benchmark did not use the GPU surface')
    steps = {}
    suites = {}
    for iteration in iterations:
        for suite, values in iteration['tests'].items():
            suites.setdefault(suite, []).append(values['total'])
            for step, timing in values['tests'].items():
                steps.setdefault(f'{suite}/{step}', []).append(timing['tests']['Async'])
    if len(suites) != 20 or any(len(values) != 10 for values in steps.values()):
        raise ValueError('The suite or step set changed between iterations')
    stalls = {
        name: sum(value > statistics.median(values) + 6 for value in values)
        for name, values in steps.items()
    }
    return {
        'score': result['score']['mean'],
        'asyncStalls': sum(stalls.values()),
        'stallsByStep': {name: count for name, count in stalls.items() if count},
        'suiteMeans': {name: statistics.mean(values) for name, values in suites.items()},
    }


def write_summary(output, records, provenance):
    groups = {}
    for record in records:
        groups.setdefault(record['configuration'], []).append(record)
    medians = {name: statistics.median(run['summary']['score'] for run in runs) for name, runs in groups.items()}
    baseline = medians.get(provenance.get('control', 'default'))
    result = {
        'provenance': provenance,
        'runs': records,
        'configurations': {
            name: {
                'runs': len(runs),
                'medianScore': medians[name],
                'scoreRange': [min(run['summary']['score'] for run in runs), max(run['summary']['score'] for run in runs)],
                'medianAsyncStalls': statistics.median(run['summary']['asyncStalls'] for run in runs),
                'changePercent': (medians[name] / baseline - 1) * 100 if baseline else None,
            }
            for name, runs in groups.items()
        },
    }
    temporary = output / 'summary.tmp'
    temporary.write_text(json.dumps(result, indent=2) + '\n')
    temporary.replace(output / 'summary.json')


def main():
    root = Path(__file__).resolve().parents[3]
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', type=Path, default=root / 'build-webkit')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--configurations', nargs='+', choices=CONFIGURATIONS)
    parser.add_argument('--control', choices=CONFIGURATIONS, default='default')
    parser.add_argument('--repeats', type=int, default=3)
    parser.add_argument('--seed', type=int, default=731)
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--allow-current-governor', action='store_true')
    parser.add_argument('--max-background-cpu-percent', type=float, default=1.0)
    arguments = parser.parse_args()
    if not 0 < arguments.max_background_cpu_percent <= 100:
        parser.error('The background CPU limit must be greater than zero and at most 100 percent')
    maximum_busy_fraction = arguments.max_background_cpu_percent / 100
    state = conditions(maximum_busy_fraction)
    if arguments.check:
        print(json.dumps(state, indent=2))
        return 0 if state['performanceGovernor'] and state['idle'] else 2
    if not arguments.output or arguments.repeats < 3:
        parser.error('Specify a new output directory and at least three repeats')
    if not state['performanceGovernor'] and not arguments.allow_current_governor:
        parser.error('Use the performance governor, or explicitly allow and record the current governor')
    def wait_until_idle():
        deadline = time.monotonic() + 90
        while True:
            current = conditions(maximum_busy_fraction)
            if current['governors'] != state['governors'] or current['energyPreferences'] != state['energyPreferences']:
                raise RuntimeError('CPU power settings changed during the sweep')
            if current['idle']:
                return current
            if time.monotonic() >= deadline:
                raise RuntimeError('The machine remained busy; completed runs remain in the output directory')
            time.sleep(2)

    wait_until_idle()
    output = arguments.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    build = arguments.build.resolve()
    cache = (build / 'CMakeCache.txt').read_text()
    prefix = re.search(r'^EDEN_WPE_PREFIX:PATH=(.+)$', cache, re.MULTILINE)
    if not prefix:
        raise RuntimeError('The build does not record its WebKit runtime prefix')
    runtime = Path(prefix[1])
    runtime_build = json.loads((runtime / 'share/eden-wpe-build.json').read_text())
    if runtime_build.get('mode') == 'profile-generate':
        raise RuntimeError('Instrumented PGO training builds cannot be used for scoring comparisons')
    available = [
        name for name, settings in CONFIGURATIONS.items()
        if (not any(key.startswith('WEBKIT_SKIA_CANVAS_REPLAY_') for key in settings) or runtime_build.get('canvasReplayPatch'))
        and (not any(key.startswith('WEBKIT_EDEN_') for key in settings) or runtime_build.get('engineExperiments'))
        and (not any(key.startswith('WEBKIT_EDEN_JSC_') for key in settings) or runtime_build.get('jscExperiments'))
    ]
    requested = arguments.configurations if arguments.configurations is not None else available
    if any(name not in available for name in [arguments.control, *requested]):
        raise RuntimeError('A requested configuration requires engine patches absent from the runtime build manifest')
    binaries = [build / 'eden-wpe-bench', build / 'eden-engine-wpe.so', runtime / 'lib/libWPEWebKit-2.0.so']
    provenance = {
        'control': arguments.control,
        'seed': arguments.seed,
        'repeats': arguments.repeats,
        'binaries': {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in binaries},
        'cpuTopology': subprocess.check_output(['lscpu', '-e=CPU,CORE,SOCKET,ONLINE'], text=True),
        'configurations': CONFIGURATIONS,
        'initialConditions': state,
        'allowCurrentGovernor': arguments.allow_current_governor,
        'backgroundCpuLimitPercent': arguments.max_background_cpu_percent,
        'runtimeBuild': runtime_build,
    }
    def identity(path):
        state = path.stat()
        return state.st_ino, state.st_size, state.st_mtime_ns

    binary_states = {path: identity(path) for path in binaries}
    environment = os.environ.copy()
    for name in list(environment):
        if name.startswith(('JSC_', 'WEBKIT_SKIA_', 'WEBKIT_EDEN_', 'SYSPROF_', 'EDEN_WPE_')) or name in {
            'WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS', 'WEBKIT_FORCE_SANDBOX', 'WEBKIT_A11Y_BUS_ADDRESS', 'LLVM_PROFILE_FILE',
        }:
            environment.pop(name)
    names = list(dict.fromkeys([arguments.control, *requested]))
    records = []
    for repeat in range(arguments.repeats):
        order = names.copy()
        random.Random(arguments.seed + repeat).shuffle(order)
        for name in order:
            run_conditions = wait_until_idle()
            if any(identity(path) != binary_states[path] for path in binaries):
                raise RuntimeError('A benchmark binary changed during the sweep')
            label = f'{name}-{repeat + 1}'
            print(f'Running {label}', flush=True)
            with tempfile.TemporaryDirectory(prefix='eden-webkit-score-') as temporary:
                temporary_output = Path(temporary) / f'{label}.json'
                command = [
                    sys.executable, str(root / 'scripts/test_wpe.py'), '--build', str(build), '--benchmark', '--',
                    '--output', str(temporary_output), '--label', label,
                ]
                with (output / f'{label}.log').open('w') as log:
                    subprocess.run(command, env=environment | CONFIGURATIONS[name], stdout=log, stderr=subprocess.STDOUT, check=True)
                contents = temporary_output.read_bytes()
                (output / f'{label}.json').write_bytes(contents)
            end_conditions = conditions(maximum_busy_fraction)
            if end_conditions['governors'] != run_conditions['governors'] or end_conditions['energyPreferences'] != run_conditions['energyPreferences']:
                (output / f'{label}.conditions.json').write_text(json.dumps({'before': run_conditions, 'after': end_conditions, 'comparable': False}, indent=2) + '\n')
                raise RuntimeError('CPU power settings changed during the run; its raw result was retained outside the comparison')
            summary = summarize_run(json.loads(contents))
            records.append({'configuration': name, 'repeat': repeat + 1, 'file': f'{label}.json', 'conditions': run_conditions, 'endConditions': end_conditions, 'summary': summary})
            write_summary(output, records, provenance)
            print(f"{label}: {summary['score']:.3f}, {summary['asyncStalls']} Async stalls", flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
