#!/usr/bin/env python3
"""Reproducible offline CLI timings with a separate exhaustive rational oracle.

These measurements include process startup, file read, JSON parsing, screening,
JSON serialization and output capture. They are neither hot-path latency nor a
before/after optimization comparison. No network or account credentials are used.
"""
import argparse
import datetime as dt
import hashlib
import importlib.util
import json
from pathlib import Path
import platform
import re
import statistics
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('basket_oracle', ROOT / 'tests/basket_screen_oracle_tests.py')
ORACLE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ORACLE)


def workload(count, depth, cap):
    """Adjacent threshold pairs share a market; each interval is distinct.

    Alternate deliberately positive/negative synthetic baskets. All levels use
    fractional quantity, and each book can support the full 100-contract grid.
    This probes depth traversal and shared-market fanout without venue data.
    """
    data = ORACLE.fixture(cap)
    for name in ('markets', 'baskets', 'fees', 'books'):
        data[name] = []
    data['sizing']['available_cash_micro'] = 10_000_000_000
    data['max_total_evaluations'] = count * 100
    size = 10000 // depth + 53

    def levels(best):
        return [[max(0, best - (depth - index - 1) * 2), size] for index in range(depth)]

    def market(identity, yes_best, no_best):
        data['markets'].append({'id': identity, 'ticker': 'SYNTHETIC-' + str(identity)})
        data['fees'].append({'market_id': identity, 'coefficient_ppm': 70000,
                             'balance_quantum_micro': 10000 if identity % 3 else 100})
        data['books'].append({'market_id': identity, 'request_time_ms': 9950,
                              'received_time_ms': 9960,
                              'yes_bids': levels(yes_best), 'no_bids': levels(no_best)})

    for index in range(count + 1):
        yes = 6200 - index % 5 * 50
        market(index + 1, yes, 10000 - yes - 200)
    for index in range(count):
        identity = count + 2 + index
        market(identity, 1500 if index % 2 == 0 else 100, 2000)
        lower = 10000 + index * 10000
        upper = lower + 10000
        data['baskets'].append({
            'id': index + 1, 'key': 'synthetic-range-cover-' + str(index),
            'lower_market_id': index + 1, 'upper_market_id': index + 2,
            'range_market_id': identity, 'lower_threshold_cents': lower,
            'upper_threshold_cents': upper, 'interval_lower_cents': lower + 1,
            'interval_upper_cents': upper,
        })
    return data


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify(output, expected):
    if not output.get('solver_complete') or output.get('simulated_fills') is not False:
        raise ValueError('incomplete or incorrectly labeled screening output')
    actual = {row['basket_id']: row for row in output['baskets']}
    if actual.keys() != expected.keys():
        raise ValueError('basket set differs from the reference')
    for identity, (quote, diagnostic) in expected.items():
        for name, reference in (('quote', quote), ('one_contract_diagnostic', diagnostic)):
            value = actual[identity][name]
            if reference is None:
                if value is not None:
                    raise ValueError('unexpected quote for basket ' + str(identity))
            elif value is None or any(value.get(key) != result for key, result in reference.items()):
                raise ValueError('rational reference mismatch for basket ' + str(identity) + ': ' + name)


def peak_memory(command):
    """An extra untimed invocation; platform time reports this child's peak RSS."""
    executable = Path('/usr/bin/time')
    if not executable.exists():
        return {'peak_rss_bytes': None, 'method': 'unavailable'}
    system = platform.system()
    if system == 'Darwin':
        measured = [str(executable), '-l'] + command
        pattern, multiplier = r'(\d+)\s+maximum resident set size', 1
    elif system == 'Linux':
        measured = [str(executable), '-f', '__eme_peak_rss_kib__=%M'] + command
        pattern, multiplier = r'__eme_peak_rss_kib__=(\d+)', 1024
    else:
        return {'peak_rss_bytes': None, 'method': 'unavailable on ' + system}
    result = subprocess.run(measured, capture_output=True, text=True, timeout=60)
    if result.returncode:
        # macOS sandboxed time can execute the child successfully and still
        # fail its own sysctl query. Keep optional RSS failure separate from
        # the already checked native correctness/timing measurements.
        try:
            child = json.loads(result.stdout)
        except ValueError:
            result.check_returncode()
        if child.get('kind') != 'conditional_payoff_screen':
            result.check_returncode()
        return {'peak_rss_bytes': None, 'method': 'unavailable',
                'reason': result.stderr.strip()[-500:]}
    match = re.search(pattern, result.stderr)
    if match is None:
        return {'peak_rss_bytes': None, 'method': 'unavailable',
                'reason': 'time completed without reporting maximum resident set size'}
    return {'peak_rss_bytes': int(match.group(1)) * multiplier,
            'method': '/usr/bin/time, one additional invocation outside timing samples'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--engine', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--samples', type=int, default=9)
    parser.add_argument('--build-notes', required=True)
    args = parser.parse_args()
    if not 3 <= args.samples <= 100:
        parser.error('--samples must be between 3 and 100')
    engine = args.engine.resolve(strict=True)
    if args.output.exists():
        parser.error('--output must name a new file; previous measurements are retained')
    report = {
        'schema_version': 1, 'kind': 'basket_screen_full_cli_benchmark',
        'created_at_utc': dt.datetime.now(dt.timezone.utc).isoformat(),
        'platform': platform.platform(), 'machine': platform.machine(),
        'python': platform.python_version(), 'engine': str(engine), 'engine_sha256': sha256(engine),
        'build_notes': args.build_notes, 'samples_per_workload': args.samples,
        'measurement_scope': 'process startup + file read + JSON parse + screen + JSON output capture',
        'excluded': ['network', 'authentication', 'market discovery', 'live feed', 'order execution'],
        'comparison_baseline': None,
        'interpretation': 'Synthetic fixed-workload baseline, not a speedup or live latency claim.',
        'oracle': 'independent Python Fraction exhaustive quantities 1..cap_contracts; one assumed fill per level',
        'source_sha256': {}, 'workloads': [],
    }
    for name in ('benchmarks/basket_screen_benchmark.py', 'tests/basket_screen_oracle_tests.py',
                 'src/cli/basket_screen.cpp', 'src/core/execution_cost.cpp', 'src/core/net_sizing.cpp'):
        path = ROOT / name
        if path.exists():
            report['source_sha256'][name] = sha256(path)
    with tempfile.TemporaryDirectory(prefix='eme-basket-benchmark-') as directory:
        for count in (20, 100):
            for depth in (1, 16):
                for cap in (100, 10000):
                    data = workload(count, depth, cap)
                    raw = json.dumps(data, sort_keys=True, separators=(',', ':')).encode()
                    path = Path(directory) / 'input.json'
                    path.write_bytes(raw)
                    begin = time.perf_counter_ns()
                    expected = {basket['id']: ORACLE.enumerate_basket(data, basket) for basket in data['baskets']}
                    reference_ms = (time.perf_counter_ns() - begin) / 1_000_000
                    command = [str(engine), 'basket', 'screen', str(path)]
                    elapsed = []
                    digest = None
                    for index in range(args.samples + 1):
                        begin = time.perf_counter_ns()
                        process = subprocess.run(command, capture_output=True, text=True, timeout=60)
                        duration_ms = (time.perf_counter_ns() - begin) / 1_000_000
                        process.check_returncode()
                        output = json.loads(process.stdout)
                        verify(output, expected)
                        this_digest = hashlib.sha256(process.stdout.encode()).hexdigest()
                        if digest is not None and digest != this_digest:
                            raise ValueError('identical input yielded different output')
                        digest = this_digest
                        if index:
                            elapsed.append(duration_ms)
                    result = {
                        'baskets': count, 'markets': len(data['markets']), 'levels_per_side': depth,
                        'cap_contracts': cap // 100, 'shared_threshold_markets': count - 1,
                        'input_bytes': len(raw), 'input_sha256': hashlib.sha256(raw).hexdigest(),
                        'output_sha256': digest, 'positive_quotes': output['positive_quotes'],
                        'evaluated_quantities': output['evaluated_quantities'],
                        'reference_parity': True, 'reference_single_evaluation_ms': reference_ms,
                        'warmups': 1, 'raw_samples_ms': elapsed,
                        'median_ms': statistics.median(elapsed), 'minimum_ms': min(elapsed),
                        'maximum_ms': max(elapsed), 'memory': peak_memory(command),
                    }
                    report['workloads'].append(result)
                    print('baskets={} depth={} cap={} median_cli_ms={:.3f} parity=pass'.format(
                        count, depth, cap // 100, result['median_ms']), flush=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
