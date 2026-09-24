#!/usr/bin/env python3
"""Run chronological basket execution scenarios on an existing read-only capture."""
import argparse
import datetime
import hashlib
import json
from pathlib import Path
import subprocess
import time


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(capture, binary, output):
    capture, binary, output = Path(capture), Path(binary), Path(output)
    basket = capture / 'session/basket-policy.json'
    if not basket.is_file() or not binary.is_file():
        raise ValueError('missing_finalized_capture_or_binary')
    output.mkdir(parents=True, exist_ok=False)
    policy = {'schema_version': 1, 'kind': 'chronological_basket_execution',
              'basket_policy_sha256': digest(basket), 'arrival_and_response_ms': [1, 10, 50, 100, 250],
              'minimum_margin_micro': 10000, 'max_attempts': 1000}
    write = lambda path, value: path.write_text(json.dumps(value, indent=2) + '\n')
    policy_path = output / 'policy.json'
    write(policy_path, policy)
    plan = {'registered_at': datetime.datetime.now(datetime.timezone.utc).isoformat(),
            'exploratory_on_previously_analyzed_capture': True, 'policy_sha256': digest(policy_path),
            'binary_sha256': digest(binary), 'runner_sha256': digest(Path(__file__)),
            'capture_manifest_sha256': digest(capture / 'session/manifest.json'),
            'orders_sent': 0, 'delay_is_measured_exchange_latency': False,
            'rules': ['Entry on new positive episode from continuous native control; no later peak selection.',
                      'One active basket per scenario; completed basket capital remains locked.',
                      'Native size search uses remaining capital and own consumed physical liquidity.',
                      'Same one-way arrival and acknowledgment delays; complete sequential arrivals are d, 3d, 5d.',
                      'Recheck total projected basket cost before each buy; IOC limit is current last required price.',
                      'Guard is a decision-time estimate, not a guarantee against later within-limit depth changes.',
                      'On partial buy or failed guard, attempt one IOC sale per acquired leg, then halt if residual remains.',
                      'Consumed physical level quantities persist across subsequent feed updates; no reset/reuse.',
                      'No settlement credits, market impact calibration or actual fill claim.'],
            'analysis_timeout_seconds': 600}
    write(output / 'plan.json', plan)
    start = time.monotonic()
    with (output / 'trace.jsonl').open('xb') as stream:
        subprocess.run([str(binary.resolve()), str((capture / 'session').resolve()), str(policy_path.resolve())],
                       stdout=stream, check=True, timeout=600)
    last = None
    with (output / 'trace.jsonl').open() as stream:
        for line in stream:
            last = json.loads(line)
    if not last or last.get('type') != 'basket_execution_complete' or last.get('orders_sent') != 0:
        raise ValueError('execution_report_incomplete')
    if (digest(binary) != plan['binary_sha256'] or digest(policy_path) != plan['policy_sha256'] or
            digest(basket) != policy['basket_policy_sha256'] or
            digest(capture / 'session/manifest.json') != plan['capture_manifest_sha256']):
        raise ValueError('analysis_inputs_changed')
    last.update(offline_processing_seconds=time.monotonic()-start,
                trace_sha256=digest(output / 'trace.jsonl'), registered_before_capture=False,
                capture_window_complete=json.loads((capture / 'result.json').read_text()).get('planned_window_complete') is True)
    write(output / 'report.json', last)
    return last


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = run(args.capture, args.binary, args.output)
    for row in result['scenarios']:
        print(json.dumps(row, sort_keys=True))
    print('Offline report:', args.output / 'report.json')


if __name__ == '__main__':
    main()
