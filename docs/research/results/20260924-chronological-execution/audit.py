"""Independent rational cost and chronological ledger audit, using saved artifacts only."""
import argparse
from fractions import Fraction
import hashlib
import json
import math
from pathlib import Path


def audit(capture, study):
    policy = json.loads((capture / 'session/basket-policy.json').read_text())
    fee_map = {x['market_id']: x for x in policy['screen']['fees']}
    baskets = {x['id']: [x[k] for k in ('lower_market_id', 'upper_market_id', 'range_market_id')]
               for x in policy['screen']['baskets']}
    rows = [json.loads(l) for l in (study / 'trace.jsonl').open()]
    report = rows[-1]
    assert report['type'] == 'basket_execution_complete' and report['orders_sent'] == 0
    delays = {x['scenario']: x['arrival_delay_ms'] * 1_000_000 for x in report['scenarios']}
    capital = report['initial_capital_micro']
    cash = {i: capital for i in delays}
    floor = {i: 0 for i in delays}
    active = {i: None for i in delays}
    attempts = {}
    pending = {}
    last_fill = {}
    rational_checks = 0
    onsets = {(r['basket_id'], r['time_ns']) for r in rows if r['type'] == 'basket_episode_open'}
    for row in rows:
        kind = row['type']
        if not kind.startswith('basket_execution_') or 'scenario' not in row:
            continue
        i = row['scenario']
        key = i, row.get('attempt')
        if kind == 'basket_execution_entry':
            assert active[i] is None
            assert (row['basket_id'], row['time_ns']) in onsets
            active[i] = key
            attempts[key] = {'entry': row, 'held': [0, 0, 0], 'spent': 0, 'received': 0}
        elif kind == 'basket_execution_submit':
            assert active[i] == key and key not in pending
            if key in last_fill:
                assert row['time_ns'] >= last_fill[key] + delays[i]
            assert row['arrival_ns'] == row['time_ns'] + delays[i]
            if row['sell']:
                assert row['quantity_centicontracts'] <= attempts[key]['held'][row['leg']]
            pending[key] = row
        elif kind == 'basket_execution_fill':
            order = pending.pop(key)
            a = attempts[key]
            leg = row['leg']
            fee = fee_map[baskets[a['entry']['basket_id']][leg]]
            assert order['sell'] == row['sell'] and order['leg'] == leg
            assert row['time_ns'] == order['arrival_ns']
            last_fill[key] = row['time_ns']
            accumulated = total = quantity = 0
            for fill in row['levels']:
                q, p = fill['quantity_centicontracts'], fill['price_1e4']
                assert q > 0 and 0 <= p <= 10000
                assert p >= order['limit_price_1e4'] if row['sell'] else p <= order['limit_price_1e4']
                contracts, dollars = Fraction(q, 100), Fraction(p, 10000)
                notional = int(contracts * dollars * 1_000_000)
                trade_fee = math.ceil(Fraction(fee['coefficient_ppm'], 1_000_000) * contracts * dollars * (1-dollars) * 1_000_000)
                quantum = fee['balance_quantum_micro']
                raw = notional-trade_fee if row['sell'] else notional+trade_fee
                aligned = (math.floor if row['sell'] else math.ceil)(Fraction(raw, quantum)) * quantum
                rounding = raw-aligned if row['sell'] else aligned-raw
                accumulated += rounding
                rebate = min(accumulated // quantum, (trade_fee+rounding) // quantum) * quantum
                accumulated -= rebate
                charged = aligned+rebate if row['sell'] else aligned-rebate
                assert charged == fill['cash_micro']
                quantity += q
                total += charged
                rational_checks += 1
            assert quantity == row['quantity_centicontracts'] <= order['quantity_centicontracts']
            assert total == row['cash_micro']
            if row['sell']:
                assert quantity <= a['held'][leg]
                a['held'][leg] -= quantity
                a['received'] += total
                cash[i] += total
            else:
                a['held'][leg] += quantity
                a['spent'] += total
                cash[i] -= total
            assert cash[i] >= 0
        elif kind == 'basket_execution_held':
            a = attempts[key]
            assert a['held'] == [a['entry']['quantity_centicontracts']] * 3
            assert row['time_ns'] >= last_fill[key] + delays[i]
            assert row['debit_micro'] == a['spent']
            payment = min(a['held']) * 20000
            assert row['conditional_floor_micro'] == payment
            assert row['conditional_margin_micro'] == payment-a['spent']
            floor[i] += payment
            active[i] = None
        elif kind == 'basket_execution_exit_complete':
            a = attempts[key]
            assert a['held'] == row['remaining_centicontracts']
            assert row['net_cash_micro'] == a['received']-a['spent']
            assert bool(any(a['held'])) == row['residual']
            if not row['residual']:
                active[i] = None
    for scenario in report['scenarios']:
        i = scenario['scenario']
        assert cash[i] == scenario['cash_micro']
        assert floor[i] == scenario['completed_conditional_floor_micro']
        if scenario['conditional_net_micro'] is not None:
            assert active[i] is None
            assert cash[i]+floor[i]-capital == scenario['conditional_net_micro']
    # The typed callback must not change the existing continuous quote control.
    control = [r for r in rows if r['type'].startswith('basket_') and not r['type'].startswith('basket_execution_')]
    original = [json.loads(l) for l in (capture / 'session/basket-replay.jsonl').open()]
    assert control == original
    return {'rational_per_level_checks': rational_checks, 'attempts_audited': len(attempts),
            'no_overlap_and_ack_timing_verified': True, 'cash_and_holdings_verified': True,
            'continuous_control_equals_original_replay': True,
            'trace_sha256': hashlib.sha256((study / 'trace.jsonl').read_bytes()).hexdigest(),
            'orders_sent': 0, 'fee_policy_is_the_same_declared_hypothesis': True}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('study', type=Path)
    args = parser.parse_args()
    print(json.dumps(audit(args.capture, args.study), indent=2))
