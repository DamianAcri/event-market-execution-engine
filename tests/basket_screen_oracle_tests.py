"""Independent rational oracle for indicative, fully funded three-leg screening.

No HTTP, account data or orders. Fraction arithmetic and exhaustive enumeration
are deliberately separate from the native prefix-cursor implementation. A fill
means one assumed match per occupied price level, never a prediction of execution.
"""
import argparse
from fractions import Fraction
import json
import math
from pathlib import Path
import random
import subprocess
import sys
import tempfile
import unittest

ENGINE = None
MICRO = 1_000_000


def fixture(cap=10000):
    return {
        'schema_version': 1, 'as_of_ms': 10000, 'max_age_ms': 100,
        'max_skew_ms': 30, 'max_total_evaluations': 10000,
        'markets': [{'id': i, 'ticker': 'SYNTHETIC-' + str(i)} for i in (1, 2, 3)],
        'baskets': [{'id': 1, 'key': 'synthetic-range-cover', 'lower_market_id': 1,
                     'upper_market_id': 2, 'range_market_id': 3,
                     'lower_threshold_cents': 10000, 'upper_threshold_cents': 20000,
                     'interval_lower_cents': 10001, 'interval_upper_cents': 20000}],
        'sizing': {'cap_centicontracts': cap, 'step_centicontracts': 100,
                   'available_cash_micro': 1_000_000_000,
                   'minimum_margin_micro': 0, 'max_evaluations': 10000},
        'fees': [{'market_id': i, 'coefficient_ppm': 70000,
                  'balance_quantum_micro': 10000} for i in (1, 2, 3)],
        'books': [
            {'market_id': 1, 'request_time_ms': 9950, 'received_time_ms': 9960,
             'yes_bids': [[1000, 10000]], 'no_bids': [[4000, 10000]]},
            {'market_id': 2, 'request_time_ms': 9950, 'received_time_ms': 9960,
             'yes_bids': [[4000, 10000]], 'no_bids': [[1000, 10000]]},
            {'market_id': 3, 'request_time_ms': 9950, 'received_time_ms': 9960,
             'yes_bids': [[4000, 10000]], 'no_bids': [[1000, 10000]]},
        ],
    }


def fee_terms(quantity, price, coefficient, quantum, accumulated):
    """Dollars first, with rational ceiling at the venue's microdollar precision."""
    contracts = Fraction(quantity, 100)
    dollars = Fraction(price, 10000)
    notional = int(contracts * dollars * MICRO)
    trade_fee = math.ceil(Fraction(coefficient, MICRO) * contracts * dollars
                          * (1 - dollars) * MICRO)
    rounded_debit = math.ceil(Fraction(notional + trade_fee, quantum)) * quantum
    rounding = rounded_debit - notional - trade_fee
    accumulated += rounding
    rebate = min(accumulated // quantum, (trade_fee + rounding) // quantum) * quantum
    return notional, rounded_debit - rebate, accumulated - rebate


def leg_quote(book, policy, outcome, quantity):
    remaining = quantity
    notional = debit = accumulated = 0
    limit = None
    # REST stores bids by ascending price. Buy from the opposite outcome.
    for bid, depth in reversed(book['no_bids' if outcome == 'yes' else 'yes_bids']):
        used = min(remaining, depth)
        if not used:
            continue
        limit = 10000 - bid
        cost, charged, accumulated = fee_terms(
            used, limit, policy['coefficient_ppm'], policy['balance_quantum_micro'], accumulated)
        notional += cost
        debit += charged
        remaining -= used
        if not remaining:
            break
    if remaining:
        return None
    # Reserve every centicontract as a possible separate fill, even when the
    # *order* quantity is whole. This is a deliberately conservative cash bound.
    contracts = Fraction(quantity, 100)
    maximum_fee = math.ceil(contracts * Fraction(policy['coefficient_ppm'], MICRO)
                            * Fraction(1, 4) * MICRO)
    reserve = int(contracts * Fraction(limit, 10000) * MICRO) + maximum_fee
    reserve += quantity * (policy['balance_quantum_micro'] + 1)
    return {'market_id': book['market_id'], 'outcome': outcome,
            'limit_price_1e4': limit, 'notional_micro': notional, 'debit_micro': debit,
            'fees_and_rounding_micro': debit - notional, 'reservation_micro': reserve}


def quantity_quote(data, basket, quantity):
    books = {book['market_id']: book for book in data['books']}
    policies = {fee['market_id']: fee for fee in data['fees']}
    legs = [leg_quote(books[basket[field]], policies[basket[field]], outcome, quantity)
            for field, outcome in (('lower_market_id', 'yes'), ('upper_market_id', 'no'),
                                   ('range_market_id', 'no'))]
    if any(leg is None for leg in legs):
        return None
    floor = int(Fraction(quantity, 100) * 2 * MICRO)
    debit = sum(leg['debit_micro'] for leg in legs)
    return {'quantity_centicontracts': quantity, 'net_margin_micro': floor - debit,
            'payout_floor_micro': floor, 'debit_micro': debit,
            'notional_micro': sum(leg['notional_micro'] for leg in legs),
            'reservation_micro': sum(leg['reservation_micro'] for leg in legs), 'legs': legs}


def enumerate_basket(data, basket):
    """All grid quantities, no pruning or reuse of a native result."""
    sizing = data['sizing']
    quotes = [quantity_quote(data, basket, q)
              for q in range(100, sizing['cap_centicontracts'] + 1, 100)]
    eligible = [quote for quote in quotes if quote is not None
                and quote['reservation_micro'] <= sizing['available_cash_micro']
                and quote['net_margin_micro'] > sizing['minimum_margin_micro']]
    best = max(eligible, key=lambda q: (q['net_margin_micro'], -q['reservation_micro'],
                                       -q['quantity_centicontracts']), default=None)
    diagnostic = quotes[0]
    if diagnostic is not None:
        diagnostic = dict(diagnostic)
        diagnostic.update(gross_margin_micro=diagnostic['payout_floor_micro'] - diagnostic['notional_micro'],
                          funded=diagnostic['reservation_micro'] <= sizing['available_cash_micro'])
    return best, diagnostic


def run_engine(data, engine=None):
    with tempfile.TemporaryDirectory(prefix='eme-basket-oracle-') as directory:
        path = Path(directory) / 'screen.json'
        path.write_text(json.dumps(data))
        result = subprocess.run([str(engine or ENGINE), 'basket', 'screen', str(path)],
                                capture_output=True, text=True, timeout=30)
        if result.returncode:
            raise AssertionError('native screening rejected fixture: ' + result.stderr)
        return json.loads(result.stdout)


def assert_quote_equal(test, actual, expected):
    if expected is None:
        test.assertIsNone(actual)
    else:
        test.assertIsInstance(actual, dict)
        for key, value in expected.items():
            test.assertEqual(actual.get(key), value, key)


class RationalReferenceTests(unittest.TestCase):
    def test_payoff_at_both_thresholds_and_interval_endpoints(self):
        def payout(observation, lower, upper):
            return int(observation > 10000) + int(observation <= 20000) + int(not lower <= observation <= upper)

        for lower, upper in ((10001, 20000), (12000, 15000), (15000, 15000)):
            endpoints = sorted({9999, 10000, 10001, lower - 1, lower, upper, upper + 1, 20000, 20001})
            self.assertEqual(min(payout(value, lower, upper) for value in endpoints), 2)
        # The native boundary rejections below matter economically: either
        # off-by-one permits a state where this three-leg package only pays $1.
        self.assertEqual(payout(10000, 10000, 20000), 1)
        self.assertEqual(payout(20001, 10001, 20001), 1)

    def test_hand_calculated_fee_rounding_and_rebate(self):
        # Two half-contract buys at 40c each incur 8400 micro trade fee; first
        # rounds by 1600, second by 1600, with no whole-cent rebate yet.
        self.assertEqual(fee_terms(50, 4000, 70000, 10000, 0), (200000, 210000, 1600))
        self.assertEqual(fee_terms(50, 4000, 70000, 10000, 9000), (200000, 200000, 600))

    def test_zero_fee_rebate_cannot_make_negative_fee(self):
        value = fee_terms(1, 1, 0, 10000, 30000)
        self.assertEqual(value, (1, 10000, 39999))

    def test_full_funding_fragmentation_bound_is_explicit(self):
        data = fixture(100)
        quote = quantity_quote(data, data['baskets'][0], 100)
        self.assertEqual(quote['notional_micro'], 1800000)
        self.assertEqual(quote['debit_micro'], 1860000)
        self.assertEqual(quote['reservation_micro'], 4852800)


@unittest.skipUnless(ENGINE, 'pass --engine to compare with the compiled CLI')
class NativeOracleTests(unittest.TestCase):
    def check_oracle(self, data):
        actual = run_engine(data)
        self.assertTrue(actual['solver_complete'])
        self.assertFalse(actual['simulated_fills'])
        self.assertEqual(actual['funding_fill_step_centicontracts'], 1)
        rows = {row['basket_id']: row for row in actual['baskets']}
        for basket in data['baskets']:
            best, diagnostic = enumerate_basket(data, basket)
            with self.subTest(basket=basket['id']):
                assert_quote_equal(self, rows[basket['id']]['quote'], best)
                assert_quote_equal(self, rows[basket['id']]['one_contract_diagnostic'], diagnostic)
        return actual

    def test_one_to_one_hundred_contracts_asymmetric_fees(self):
        data = fixture()
        data['fees'][1]['coefficient_ppm'] = 35000
        data['fees'][2]['balance_quantum_micro'] = 100
        self.check_oracle(data)

    def test_fractional_depth_accumulates_to_whole_order(self):
        data = fixture(200)
        for book, side in zip(data['books'], ('no_bids', 'yes_bids', 'yes_bids')):
            book[side] = [[3500, 55], [3800, 46], [4000, 39], [4200, 60]]
        result = self.check_oracle(data)
        self.assertIsNotNone(result['baskets'][0]['quote'])
        self.assertEqual(result['baskets'][0]['quote']['quantity_centicontracts'], 200)

    def test_fee_rounding_can_make_two_contracts_profitable_when_one_is_not(self):
        data = fixture(200)
        for book, side in zip(data['books'], ('no_bids', 'yes_bids', 'yes_bids')):
            book[side] = [[3400, 200]]
        for fee in data['fees']:
            fee['coefficient_ppm'] = 7000
        result = self.check_oracle(data)['baskets'][0]
        self.assertEqual(result['one_contract_diagnostic']['net_margin_micro'], -10000)
        self.assertEqual(result['quote']['net_margin_micro'], 10000)
        self.assertEqual(result['quote']['quantity_centicontracts'], 200)

    def test_more_capital_does_not_create_more_depth(self):
        data = fixture(10000)
        for book, side in zip(data['books'], ('no_bids', 'yes_bids', 'yes_bids')):
            book[side] = [[4000, 250]]
        original = self.check_oracle(data)['baskets'][0]['quote']
        data['sizing']['available_cash_micro'] *= 100
        larger = self.check_oracle(data)['baskets'][0]['quote']
        self.assertEqual(original, larger)
        self.assertEqual(larger['quantity_centicontracts'], 200)

    def test_funding_bound_can_reject_apparently_affordable_quote(self):
        data = fixture(100)
        exact = quantity_quote(data, data['baskets'][0], 100)
        data['sizing']['available_cash_micro'] = exact['reservation_micro'] - 1
        actual = self.check_oracle(data)['baskets'][0]
        self.assertEqual(actual['status'], 'insufficient_cash')
        self.assertGreater(data['sizing']['available_cash_micro'], exact['debit_micro'])
        data['sizing']['available_cash_micro'] += 1
        self.assertIsNotNone(self.check_oracle(data)['baskets'][0]['quote'])

    def test_deterministic_generated_depth_fee_and_funding_cases(self):
        rng = random.Random(928741)
        for index in range(64):
            data = fixture(10000)
            data['sizing']['minimum_margin_micro'] = rng.choice((0, 10000, 100000))
            data['sizing']['available_cash_micro'] = rng.choice((3000000, 20000000, 1000000000))
            for book, side, fee in zip(data['books'], ('no_bids', 'yes_bids', 'yes_bids'), data['fees']):
                levels = rng.randrange(2, 17)
                prices = sorted(rng.sample(range(1500, 7500, 25), levels))
                book[side] = [[price, rng.randrange(1, 2001)] for price in prices]
                fee['coefficient_ppm'] = rng.choice((0, 7000, 35000, 70000, 1000000))
                fee['balance_quantum_micro'] = rng.choice((100, 10000))
            with self.subTest(case=index):
                self.check_oracle(data)

    def test_negative_fees_costs_and_zero_gross_margin_do_not_quote(self):
        data = fixture(100)
        data['books'][0]['no_bids'] = [[3000, 100]]
        data['books'][1]['yes_bids'] = [[3000, 100]]
        data['books'][2]['yes_bids'] = [[4000, 100]]
        row = self.check_oracle(data)['baskets'][0]
        self.assertEqual(row['one_contract_diagnostic']['gross_margin_micro'], 0)
        self.assertLess(row['one_contract_diagnostic']['net_margin_micro'], 0)
        self.assertEqual(row['status'], 'no_positive_margin')

    def test_quote_invariant_to_json_collection_order(self):
        data = fixture()
        expected = run_engine(data)
        for key in ('markets', 'books', 'fees'):
            data[key].reverse()
        self.assertEqual(run_engine(data), expected)

    def test_freshness_and_missing_inputs_fail_closed(self):
        scenarios = (
            ('missing_book', lambda d: d['books'].pop()),
            ('missing_fee', lambda d: d['fees'].pop()),
            ('stale_book', lambda d: d['books'][0].update(request_time_ms=9800)),
            ('nonsynchronous_books', lambda d: d['books'][0].update(request_time_ms=9920)),
            ('no_depth', lambda d: d['books'][0].update(no_bids=[[4000, 99]])),
        )
        for status, mutate in scenarios:
            data = fixture()
            mutate(data)
            row = run_engine(data)['baskets'][0]
            with self.subTest(status=status):
                self.assertEqual(row['status'], status)
                self.assertIsNone(row['quote'])

    def test_quantity_budget_does_not_emit_unproven_optimum(self):
        for key in ('max_evaluations', 'max_total_evaluations'):
            data = fixture()
            (data['sizing'] if key == 'max_evaluations' else data)[key] = 1
            output = run_engine(data)
            self.assertFalse(output['solver_complete'])
            self.assertLessEqual(output['evaluated_quantities'], 1)
            self.assertIsNone(output['baskets'][0]['quote'])
            self.assertEqual(output['baskets'][0]['status'], 'search_budget_exceeded')

    def test_interval_containment_prevents_uncovered_payoff_state(self):
        mutations = [
            lambda d: d['baskets'][0].update(interval_lower_cents=10000),
            lambda d: d['baskets'][0].update(interval_upper_cents=20001),
            lambda d: d['baskets'][0].update(range_market_id=1),
            lambda d: d['books'][0].update(no_bids=[[9000, 1], [8000, 1]]),
            lambda d: d['books'][0].update(no_bids=[[9500, 100]]),
            lambda d: d['sizing'].update(step_centicontracts=1),
        ]
        for index, mutate in enumerate(mutations):
            data = fixture()
            mutate(data)
            with self.subTest(case=index), self.assertRaises(AssertionError):
                run_engine(data)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument('--engine', type=Path)
    args, remaining = parser.parse_known_args()
    ENGINE = args.engine
    # unittest evaluates class decorators at import time. Configure the optional
    # integration suite here, retaining importability by the benchmark driver.
    NativeOracleTests.__unittest_skip__ = ENGINE is None
    unittest.main(argv=[sys.argv[0]] + remaining)
