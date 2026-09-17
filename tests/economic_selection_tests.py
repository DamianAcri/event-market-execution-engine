"""Offline policy tests; optional --engine exercises the actual C++ cost solver."""
import argparse
import copy
import datetime as dt
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
from urllib.parse import parse_qs, urlsplit

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import economic_selection as selection

UTC = dt.timezone.utc
NOW = dt.datetime(2026, 9, 17, 10, 0, tzinfo=UTC)
ENGINE = None


def group(series='KXBTCD', event='KXBTCD-EVENT', count=3):
    markets = [{'ticker': event + '-T' + str(100 + i), 'event_ticker': event,
                'floor_strike': 100 + i, 'volume_24h_fp': '50.00',
                'yes_ask_dollars': '0.5000', 'no_ask_dollars': '0.5000',
                'close_time': '2026-09-18T10:00:00Z'} for i in range(count)]
    return {'series_ticker': series, 'event_ticker': event, 'family': series.lower(),
            'fee_verified': True, 'coefficient_ppm': 70000, 'markets': markets,
            'terms_sha256': 'a' * 64, 'rule_fingerprint': 'b' * 64}


def metadata(groups=None):
    return selection.metadata_for(groups or [group()], None, 1)


def public_book(ticker, yes='0.5000', no='0.5000'):
    return {'ticker': ticker, 'orderbook_fp': {
        'yes_dollars': [[yes, '10.00']], 'no_dollars': [[no, '10.00']]}}


def trade(ticker, identity='trade-1', **overrides):
    result = {'ticker': ticker, 'trade_id': identity, 'created_time': '2026-09-17T09:59:00Z',
              'count_fp': '2.50', 'is_block_trade': False}
    result.update(overrides)
    return result


class Client:
    """Public read-only response fixture; no credentials and no network."""
    def __init__(self, root, responder):
        self.root = Path(root) / 'public'; self.root.mkdir(exist_ok=True)
        self.responder = responder
        self.requests, self.calls, self.sources = [], [], {}
        self.saved = 0

    def get_json(self, path, name):
        self.calls.append((path, name))
        self.requests.append({'requested_at': '2026-09-17T09:59:59.900Z',
                              'completed_at': '2026-09-17T10:00:00.000Z'})
        return self.responder(path, name)

    def save(self):
        self.saved += 1


class SelectionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='eme-selection-test-')
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def client(self, payload):
        return Client(self.root, lambda _path, _name: copy.deepcopy(payload))

    def test_exact_decimal_boundaries(self):
        self.assertEqual(selection.units('0.1234', 10000), 1234)
        self.assertEqual(selection.units('123.45', 100), 12345)
        for value in (0.1, True, None, 'nan', 'Infinity', '-0.01', '0.00001'):
            with self.subTest(value=value), self.assertRaises(selection.SelectionError):
                selection.units(value, 10000)

    def test_rfc3339_trade_timestamps_with_variable_fractional_digits(self):
        for digits, expected_microseconds in [('1', 100000), ('12345', 123450), ('123456789', 123456)]:
            with self.subTest(digits=digits):
                parsed = selection.timestamp('2026-09-17T10:00:00.' + digits + 'Z')
                self.assertEqual(parsed, NOW + dt.timedelta(microseconds=expected_microseconds))
                self.assertEqual(selection.timestamp('2026-09-17T12:00:00.' + digits + '+02:00'), parsed)
        self.assertEqual(selection.timestamp('2026-09-17T10:00:00.999999999Z').microsecond, 999999)
        self.assertLess(selection.timestamp('2026-09-17T10:00:00.999999999Z'), NOW + dt.timedelta(seconds=1))
        for value in ('invalid', '2026-02-30T10:00:00Z', '2026-09-17T10:00:00', '2026-09-17', None, 0):
            with self.subTest(value=value), self.assertRaises(selection.SelectionError):
                selection.timestamp(value)

    def test_no_cross_event_or_asset_relations_and_stable_group_order(self):
        groups = [group(), group(event='KXBTCD-OTHER'), group('KXETHD', 'KXETHD-EVENT')]
        result, originals, _ = metadata(groups)
        self.assertEqual(len(result['markets']), 9)
        self.assertEqual(len(result['constraints']), 9)
        by_id = {m['id']: originals[m['ticker']] for m in result['markets']}
        for relation in result['constraints']:
            high = by_id[relation['relationship']['antecedent']]
            low = by_id[relation['relationship']['consequent']]
            self.assertEqual(high['event_ticker'], low['event_ticker'])
            self.assertGreater(high['floor_strike'], low['floor_strike'])
        self.assertEqual(result, metadata(list(reversed(groups)))[0])
        chosen = {groups[0]['markets'][0]['ticker'], groups[0]['markets'][2]['ticker']}
        filtered, _, _ = selection.metadata_for(groups, chosen, 1)
        self.assertEqual(len(filtered['markets']), 2)
        self.assertEqual(len(filtered['constraints']), 1)

    def test_unverified_fees_cannot_generate_native_metadata(self):
        unverified = group(); unverified['fee_verified'] = False
        with self.assertRaisesRegex(selection.SelectionError, 'fee_verification_required'):
            metadata([unverified])

    def test_catalogue_zero_asks_do_not_crowd_out_observed_quotes(self):
        empty = group(event='KXBTCD-AAA', count=2)
        available = group(event='KXBTCD-ZZZ', count=2)
        for market in empty['markets']:
            market['yes_ask_dollars'] = '0.0000'
            market['no_ask_dollars'] = '0.0000'
        self.assertEqual(selection.book_pool([empty, available], 2),
                         {m['ticker'] for m in available['markets']})

    def test_book_normalization_and_null_sides(self):
        meta, _, _ = metadata([group(count=2)])
        rows = [public_book(m['ticker']) for m in meta['markets']]
        rows[0]['orderbook_fp']['yes_dollars'] = [['0.6000', '2.50'], ['0.2000', '1.00']]
        rows[1]['orderbook_fp']['no_dollars'] = None
        client = self.client({'orderbooks': rows})
        actual = selection.fetch_books(client, meta, 'books')
        self.assertEqual(actual[0]['yes_bids'], [[2000, 100], [6000, 250]])
        self.assertEqual(actual[1]['no_bids'], [])
        self.assertEqual(actual[0]['received_time_ms'] - actual[0]['request_time_ms'], 100)
        query = parse_qs(urlsplit(client.calls[0][0]).query)
        self.assertEqual(query['tickers'], [m['ticker'] for m in meta['markets']])

    def test_book_validation_fails_closed(self):
        meta, _, _ = metadata([group(count=2)])
        base = [public_book(m['ticker']) for m in meta['markets']]
        cases = []
        changed = copy.deepcopy(base); changed[0]['ticker'] = 'UNREQUESTED'; cases.append(changed)
        changed = copy.deepcopy(base); changed[0]['ticker'] = changed[1]['ticker']; cases.append(changed)
        cases.append(base[:1])
        for level in ([0.5, '1.00'], ['1.0001', '1.00'], ['0.5000', '0.00'],
                      ['0.5000', '-1.00'], ['0.5000', '0.001'], ['0.50']):
            changed = copy.deepcopy(base); changed[0]['orderbook_fp']['yes_dollars'] = [level]; cases.append(changed)
        changed = copy.deepcopy(base); changed[0]['orderbook_fp']['yes_dollars'] *= 2; cases.append(changed)
        for rows in cases:
            with self.subTest(rows=rows), self.assertRaises(selection.SelectionError):
                selection.fetch_books(self.client({'orderbooks': rows}), meta, 'books')

    def test_missing_book_side_is_not_silently_empty(self):
        meta, _, _ = metadata([group(count=2)])
        rows = [public_book(m['ticker']) for m in meta['markets']]
        del rows[0]['orderbook_fp']['yes_dollars']
        with self.assertRaises(selection.SelectionError):
            selection.fetch_books(self.client({'orderbooks': rows}), meta, 'books')

    def test_pair_union_subscription_budget_is_deterministic(self):
        ranked = [{'tickers': ['A', 'B']}, {'tickers': ['B', 'C']},
                  {'tickers': ['C', 'D']}, {'tickers': ['A', 'B']}]
        self.assertEqual(selection.pair_budget(ranked, 3), {'A', 'B', 'C'})
        self.assertEqual(selection.pair_budget(ranked, 2), {'A', 'B'})
        self.assertEqual(selection.pair_budget(ranked, 1), set())
        self.assertEqual(selection.pair_budget(ranked, 4), {'A', 'B', 'C', 'D'})

    def test_activity_budget_and_no_recent_two_leg_flow(self):
        def candidate(a, b, positive=False):
            return {'tickers': [a, b], 'quote': {'net_margin_micro': 100} if positive else None}
        rows = [candidate('A', 'B'), candidate('B', 'C'), candidate('C', 'D'), candidate('E', 'F', True)]
        activity = {key: {'recent_nonblock_trades': count} for key, count in [('A', 1), ('B', 1), ('C', 0), ('E', 0), ('F', 0)]}
        chosen, accepted, declined = selection.choose_watchlist(rows, activity, 4)
        self.assertEqual(chosen, {'A', 'B', 'E', 'F'})
        self.assertEqual(len(accepted), 2)
        self.assertEqual([r['exclusion'] for r in declined], ['no_recent_two_leg_trade_activity', 'activity_budget'])
        chosen, _, declined = selection.choose_watchlist(rows, activity, 3)
        self.assertEqual(chosen, {'A', 'B'})
        self.assertEqual(declined[-1]['exclusion'], 'subscription_budget')
        quiet = {k: {'recent_nonblock_trades': 0} for k in ('A', 'B')}
        self.assertEqual(selection.choose_watchlist(rows[:1], quiet, 64)[0], set())

    def test_activity_only_counts_verified_nonblock_trades_and_lower_bounds(self):
        client = self.client({'trades': [trade('A'), trade('A', 'unknown', is_block_trade=None),
                                          trade('A', 'block', is_block_trade=True)], 'cursor': 'next-page'})
        result = selection.fetch_activity(client, {'A'}, NOW)['A']
        self.assertEqual(result['recent_nonblock_trades'], 1)
        self.assertEqual(result['recent_volume_centicontracts'], 250)
        self.assertEqual(result['unknown_or_block_excluded'], 2)
        self.assertFalse(result['complete']); self.assertTrue(result['count_is_lower_bound'])
        self.assertEqual(parse_qs(urlsplit(client.calls[0][0]).query)['is_block_trade'], ['false'])
        for trades in ([trade('B')], [trade('A'), trade('A')], [trade('A', count_fp='0.00')],
                       [trade('A', created_time='2026-09-16T10:00:00Z')]):
            with self.subTest(trades=trades), self.assertRaises(selection.SelectionError):
                selection.fetch_activity(self.client({'trades': trades, 'cursor': ''}), {'A'}, NOW)

    def test_recent_activity_acquisition_is_sorted_and_bounded_by_pool(self):
        def respond(path, _name):
            ticker = parse_qs(urlsplit(path).query)['ticker'][0]
            return {'trades': [trade(ticker)], 'cursor': ''}
        client = Client(self.root, respond)
        result = selection.fetch_activity(client, {'Z', 'A'}, NOW)
        self.assertEqual(list(result), ['A', 'Z'])
        self.assertEqual(len(client.calls), 2)

    def test_candidate_priority_uses_native_costs_and_stable_ties(self):
        meta, originals, _ = metadata()
        base = {'status': 'no_positive_margin', 'quote': None,
                'one_contract_diagnostic': {'net_margin_micro': -40000, 'funded': True}}
        rows = [dict(base, constraint_id=i) for i in (3, 2, 1)]
        rows[0]['quote'] = {'net_margin_micro': 10}
        rows[0]['status'] = 'optimal'
        result = selection.candidates(meta, originals, {'rows': rows})
        self.assertEqual(result[0]['constraint_id'], 3)
        self.assertEqual(result[0]['selection_class'], 'positive_indicative_margin')
        self.assertEqual([r['constraint_id'] for r in result[1:]], [1, 2])
        excluded = dict(base, constraint_id=1, one_contract_diagnostic={'funded': False, 'net_margin_micro': -1})
        self.assertEqual(selection.candidates(meta, originals, {'rows': [excluded]}), [])
        for state in ('stale_book', 'nonsynchronous_books', 'missing_book', 'search_budget_exceeded'):
            self.assertEqual(selection.candidates(meta, originals, {'rows': [dict(base, constraint_id=1, status=state)]}), [])

    def test_fee_pagination_rejects_cursor_loops(self):
        client = self.client({'event_fee_changes': [], 'cursor': 'repeat'})
        with self.assertRaisesRegex(selection.SelectionError, 'event_fee_cursor_loop'):
            selection.paginated_event_fees(client, 'EVENT', 'fees')
        self.assertEqual(len(client.calls), 2)

    def test_native_duplicate_rows_cannot_pass_set_equality(self):
        meta, _, fees = metadata([group(count=2)])
        result = {'kind': 'indicative_rest_screen', 'constraints': [{'constraint_id': 1}, {'constraint_id': 1}],
                  'evaluated_quantities': 0, 'solver_complete': True}
        fake = subprocess.CompletedProcess([], 0, json.dumps(result).encode(), b'')
        with patch.object(selection.subprocess, 'run', return_value=fake), \
             self.assertRaisesRegex(selection.SelectionError, 'native_screen_constraint_mismatch'):
            selection.native_screen(self.root, '/unused/mock-engine', meta, fees, [], 'duplicate', 0)

    def test_normalized_artifact_budget_fails_before_another_write(self):
        # Sparse allocation tests logical byte accounting without allocating a
        # quarter-gigabyte payload or touching any user capture directory.
        with (self.root / 'already-stored.bin').open('wb') as stored:
            stored.truncate(256 * 1024 * 1024)
        target = self.root / 'next.json'
        with self.assertRaisesRegex(selection.SelectionError, 'preparation_artifact_budget_exceeded'):
            selection.write_json(target, {'new': 'payload'})
        self.assertFalse(target.exists())

    def test_replacing_artifact_counts_new_size_instead_of_double_counting(self):
        target = self.root / 'replace.json'
        with target.open('wb') as stored:
            stored.truncate(256 * 1024 * 1024)
        selection.write_json(target, {'replacement': True})
        self.assertEqual(json.loads(target.read_text()), {'replacement': True})

    def test_zero_qualified_groups_returns_explicit_no_run_without_native_work(self):
        eligible = group(count=2)
        catalog = {'series': [{'ticker': 'KXBTCD'}], 'markets': eligible['markets'],
                   'coverage': {'complete': True}}
        client = Client(self.root, lambda *_: self.fail('No economic requests expected without qualified groups'))
        with patch.object(selection, 'refresh_reviewed_catalog'), \
             patch.object(selection, 'qualify_catalog', return_value={'groups': [], 'exclusions': [
                 {'reason': 'terms_hash_mismatch'}]}), \
             patch.object(selection.subprocess, 'run') as native:
            chosen, originals, report, _ = selection.prepare_economic(
                self.root, 7200, '/unused/engine', lambda _: b'synthetic terms', market_budget=2,
                book_budget=2, activity_budget=2, client=client, catalog=catalog, clock=lambda: NOW)
        self.assertEqual(chosen['markets'], [])
        self.assertEqual(originals, [])
        self.assertEqual(report['decision'], 'do_not_start_no_qualified_active_pairs')
        self.assertEqual(report['semantic_exclusion_counts'], {'terms_hash_mismatch': 1})
        native.assert_not_called()

    def test_actual_native_screen_drives_signed_costs_and_archives_inputs(self):
        if ENGINE is None:
            self.skipTest('optional --engine path required')
        meta, _, fees = metadata([group(count=2)])
        client = self.client({'orderbooks': [public_book(m['ticker']) for m in meta['markets']]})
        books = selection.fetch_books(client, meta, 'books')
        original_run = subprocess.run
        with patch.object(selection.subprocess, 'run', wraps=original_run) as invoked:
            result = selection.native_screen(self.root, ENGINE, meta, fees, books, 'native', int(NOW.timestamp() * 1000))
        self.assertEqual(invoked.call_count, 1)
        self.assertEqual(invoked.call_args[0][0][1:3], ['market', 'screen'])
        self.assertEqual(result['rows'][0]['status'], 'no_positive_margin')
        self.assertEqual(result['rows'][0]['one_contract_diagnostic']['net_margin_micro'], -40000)
        self.assertIsNone(result['rows'][0]['quote'])
        self.assertTrue((self.root / 'native-0-metadata.json').is_file())
        stored = json.loads((self.root / 'native-0-input.json').read_text())
        self.assertEqual(stored['fees'][0]['coefficient_ppm'], 70000)
        self.assertEqual(stored['sizing']['available_cash_micro'], 1000000000)
        output = json.loads((self.root / 'native-0-output.json').read_text())
        self.assertFalse(output['simulated_fills'])

    def test_native_chunks_remove_unreferenced_markets_fees_and_books(self):
        if ENGINE is None:
            self.skipTest('optional --engine path required')
        meta, _, fees = metadata([group(count=2)])
        client = self.client({'orderbooks': [public_book(m['ticker']) for m in meta['markets']]})
        books = selection.fetch_books(client, meta, 'books')
        # No fee and an intentionally malformed unused book: neither belongs in
        # this chunk. Referenced IDs must remain unchanged, including sparse IDs.
        meta['markets'].append({'id': 999, 'ticker': 'UNREFERENCED'})
        books.append({'market_id': 999, 'yes_bids': 'malformed-unused-book'})
        result = selection.native_screen(self.root, ENGINE, meta, fees, books, 'subset', int(NOW.timestamp() * 1000))
        self.assertEqual(len(result['rows']), 1)
        archived_meta = json.loads((self.root / 'subset-0-metadata.json').read_text())
        archived_input = json.loads((self.root / 'subset-0-input.json').read_text())
        self.assertEqual([m['id'] for m in archived_meta['markets']], [1, 2])
        self.assertEqual([m['market_id'] for m in archived_input['books']], [1, 2])
        self.assertEqual([m['market_id'] for m in archived_input['fees']], [1, 2])

    def test_native_large_graph_preserves_every_relation_across_chunks(self):
        if ENGINE is None:
            self.skipTest('optional --engine path required')
        meta, _, fees = metadata([group(count=65)])
        self.assertEqual(len(meta['constraints']), 2080)
        client = self.client({'orderbooks': [public_book(m['ticker']) for m in meta['markets']]})
        books = selection.fetch_books(client, meta, 'books')
        result = selection.native_screen(self.root, ENGINE, meta, fees, books, 'chunks', int(NOW.timestamp() * 1000))
        self.assertEqual([r['constraint_id'] for r in result['rows']], list(range(1, 2081)))
        self.assertTrue(result['solver_complete'])
        self.assertTrue(all(r['status'] == 'no_positive_margin' for r in result['rows']))
        second = json.loads((self.root / 'chunks-1-metadata.json').read_text())
        needed = {c['relationship'][role] for c in meta['constraints'][2048:]
                  for role in ('antecedent', 'consequent')}
        self.assertEqual({m['id'] for m in second['markets']}, needed)
        self.assertLess(len(second['markets']), len(meta['markets']))

        # Force exhaustion in the first chunk. The next chunk is reported as
        # unexamined rather than restarting the same global evaluation budget.
        books[0]['no_bids'] = [[6000, 1000]]
        books[1]['yes_bids'] = [[7000, 1000]]
        limited = selection.native_screen(self.root, ENGINE, meta, fees, books, 'limited',
                                          int(NOW.timestamp() * 1000), total_evaluations=1)
        self.assertEqual(limited['evaluated_quantities'], 1)
        self.assertFalse(limited['solver_complete'])
        self.assertEqual(len(limited['rows']), 2080)
        self.assertTrue(all(r['status'] == 'screening_budget_exceeded' for r in limited['rows'][2048:]))
        self.assertFalse((self.root / 'limited-1-output.json').exists())

    def test_prepare_refuses_long_capture_when_no_active_eligible_pairs(self):
        if ENGINE is None:
            self.skipTest('optional --engine path required')
        eligible = group(count=2)
        def respond(path, _name):
            split = urlsplit(path)
            if split.path == 'markets/orderbooks':
                return {'orderbooks': [public_book(t) for t in parse_qs(split.query)['tickers']]}
            if split.path == 'markets/trades':
                return {'trades': [], 'cursor': ''}
            if split.path == 'series/fee_changes':
                return {'series_fee_change_arr': []}
            if split.path == 'events/fee_changes':
                return {'event_fee_changes': [], 'cursor': ''}
            if split.path.startswith('events/'):
                return {'event': {}}
            raise AssertionError('Unexpected request ' + path)
        client = Client(self.root, respond)
        catalog = {'series': [{'ticker': 'KXBTCD'}], 'markets': eligible['markets'],
                   'coverage': {'complete': True}}
        qualified = {'groups': [eligible], 'exclusions': []}
        with patch.object(selection, 'qualify_catalog', return_value=qualified), \
             patch.object(selection, 'refresh_reviewed_catalog'), \
             patch.object(selection, 'verify_group_fees', side_effect=lambda g, *_: g):
            chosen, originals, report, _ = selection.prepare_economic(
                self.root, 7200, ENGINE, lambda _: b'synthetic terms fixture',
                market_budget=2, book_budget=2, activity_budget=2,
                client=client, catalog=catalog, clock=lambda: NOW)
        self.assertEqual(chosen['markets'], [])
        self.assertEqual(originals, [])
        self.assertEqual(report['decision'], 'do_not_start_no_qualified_active_pairs')
        self.assertEqual(report['selected_markets'], 0)
        self.assertEqual(report['initial_screen_statuses'], {'no_positive_margin': 1})
        self.assertFalse(any(name.startswith('refreshed-books') for _, name in client.calls))
        self.assertTrue((self.root / 'selection-decision.json').is_file())
        self.assertGreater(client.saved, 0)

    def test_announced_fee_change_entering_run_horizon_during_preparation_rejects(self):
        if ENGINE is None:
            self.skipTest('optional --engine path required')
        eligible = group(count=2)
        source = {'name': 'CF Benchmarks', 'url': 'https://www.cfbenchmarks.com/data/indices/BRTI'}
        series = {'ticker': 'KXBTCD', 'fee_type': 'quadratic', 'fee_multiplier': 1,
                  'settlement_sources': [source]}
        event = {'series_ticker': 'KXBTCD', 'event_ticker': eligible['event_ticker'],
                 'mutually_exclusive': False, 'settlement_sources': [source]}
        change_at = NOW + dt.timedelta(seconds=7200 + 600 + 15)

        def respond(path, _name):
            split = urlsplit(path)
            if split.path == 'markets/orderbooks':
                return {'orderbooks': [public_book(t, '0.3000', '0.6000') if t.endswith('T100')
                                      else public_book(t, '0.7000', '0.2000')
                                      for t in parse_qs(split.query)['tickers']]}
            if split.path == 'markets/trades':
                return {'trades': [], 'cursor': ''}
            if split.path == 'series/fee_changes':
                return {'series_fee_change_arr': [{'series_ticker': 'KXBTCD',
                                                   'scheduled_ts': change_at.isoformat()}]}
            if split.path == 'events/fee_changes':
                return {'event_fee_changes': [], 'cursor': ''}
            if split.path.startswith('events/'):
                return {'event': event}
            raise AssertionError('Unexpected request ' + path)

        client = Client(self.root, respond)
        catalog = {'series': [series], 'markets': eligible['markets'], 'coverage': {'complete': True}}
        times = iter([NOW, NOW + dt.timedelta(seconds=10), NOW + dt.timedelta(seconds=10),
                      NOW + dt.timedelta(seconds=20), NOW + dt.timedelta(seconds=30)])
        with patch.object(selection, 'refresh_reviewed_catalog'), \
             patch.object(selection, 'qualify_catalog', return_value={'groups': [eligible], 'exclusions': []}), \
             self.assertRaisesRegex(selection.SelectionError, 'fee_window_changed_during_preparation'):
            selection.prepare_economic(self.root, 7200, ENGINE, lambda _: b'synthetic terms',
                market_budget=2, book_budget=2, activity_budget=2, client=client,
                catalog=catalog, clock=lambda: next(times))
        self.assertTrue((self.root / 'refreshed-0-output.json').exists())
        self.assertFalse((self.root / 'selection-decision.json').exists())


if __name__ == '__main__':
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument('--engine', type=Path)
    args, remaining = parser.parse_known_args()
    ENGINE = args.engine
    unittest.main(argv=[sys.argv[0]] + remaining)
