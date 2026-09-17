#!/usr/bin/env python3
"""Write a deterministic 48-market / 240-constraint rejection workload.

Import capture.json with event-engine session import before benchmarking.
No network, credentials or observed market data. Output directory must be new.
"""
import argparse
import json
from pathlib import Path

from capture_readonly import paper_policy


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    args.directory.mkdir(parents=True, exist_ok=False)
    markets = [{'id': i + 1, 'ticker': 'SYNTHETIC-' + str(i + 1)} for i in range(48)]
    constraints = []
    for group in range(2):
        for low in range(1 + group * 16, 17 + group * 16):
            for high in range(low + 1, 17 + group * 16):
                constraints.append({'id': len(constraints) + 1, 'semantic_version': 1,
                    'key': 'synthetic.' + str(high) + '-implies-' + str(low),
                    'provenance': 'Synthetic ordered thresholds; no observed economic evidence.',
                    'relationship': {'type': 'implication', 'antecedent': high, 'consequent': low}})
    metadata = {'schema_version': 1, 'metadata_version': 1, 'venue': 'kalshi',
                'markets': markets, 'constraints': constraints}
    sequence, records = [0] * 48, []
    for i in range(30000):
        market = markets[i % 48]
        sequence[i % 48] += 1
        msg = {'market_ticker': market['ticker']}
        if i < 48:
            kind = 'orderbook_snapshot'
            msg.update(yes_dollars_fp=[['0.4900', '100.00']], no_dollars_fp=[['0.5100', '100.00']])
        else:
            kind = 'orderbook_delta'
            msg.update(side='yes', price_dollars='0.4900', delta_fp='1.00' if (i // 48) % 2 else '-1.00')
        records.append({'generation': 1, 'observed_at_ns': 1800000000000000000 + i * 1000,
                        'time_ns': 1000 + i * 1000, 'sequence': sequence[i % 48],
                        'payload': {'type': kind, 'sid': market['id'], 'seq': sequence[i % 48], 'msg': msg}})
    capture = {'schema_version': 1, 'source_kind': 'synthetic', 'use_yes_price': True,
               'provenance': 'Synthetic 48-market rejection workload; no return or execution-speed evidence.',
               'controls': [{'action': 'open', 'before_record': 0, 'target': 1, 'time_ns': 0},
                            {'action': 'close', 'before_record': len(records), 'target': 1,
                             'time_ns': len(records) * 1000 + 1000}], 'records': records}
    for name, value in [('metadata.json', metadata), ('policy.json', paper_policy(metadata)), ('capture.json', capture)]:
        (args.directory / name).write_text(json.dumps(value, sort_keys=True) + '\n', encoding='utf-8')


if __name__ == '__main__':
    main()
