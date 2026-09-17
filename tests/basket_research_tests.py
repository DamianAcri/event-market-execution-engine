"""Frozen-cohort preparation checks, archived fake public replies, no network."""
import copy
import datetime as dt
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch
from urllib.parse import parse_qs, urlsplit

sys.path.insert(0, str(Path(__file__).parents[1] / 'scripts'))
import basket_research as research
from basket_families_tests import fixture
from market_catalog import CatalogClient, PublicResponse, read_archived_response

NOW = dt.datetime(2026, 9, 17, 12, tzinfo=dt.timezone.utc)
PDF = b'%PDF-synthetic-tested-rule-bytes'


class PreparationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / 'run'
        self.engine = Path(self.temp.name) / 'engine'
        self.engine.write_text('not executed by these preparation tests')
        self.catalogs, self.series, self.events, self.sc, self.ec = fixture()
        self.calls, self.inputs = [], []
        self.lock = threading.Lock()
        self.clock_value = NOW
        self.addCleanup(patch.stopall)
        patch.dict(research.BTC, {'terms_sha256': hashlib.sha256(PDF).hexdigest()}).start()

    def clock(self):
        with self.lock:
            value = self.clock_value
            self.clock_value += dt.timedelta(milliseconds=1)
            return value

    def transport(self, url):
        with self.lock: self.calls.append(url)
        parsed = urlsplit(url)
        query = parse_qs(parsed.query)
        path = parsed.path.removeprefix('/trade-api/v2/')
        if parsed.netloc == 'assets.kalshi.com':
            return PublicResponse(PDF)
        if path == 'markets':
            value = self.catalogs[query['series_ticker'][0]]
        elif path == 'series/fee_changes':
            self.assertEqual(query['show_historical'], ['true'])
            value = self.sc[query['series_ticker'][0]]
        elif path.startswith('series/'):
            value = {'series': self.series[path.split('/')[1]]}
        elif path == 'events/fee_changes':
            value = self.ec[query['event_ticker'][0]]
        elif path.startswith('events/'):
            value = {'event': self.events[path.split('/')[1]]}
        elif path == 'markets/orderbooks':
            self.assertTrue((self.root / 'qualification.json').exists())
            value = {'orderbooks': [{'ticker': ticker, 'orderbook_fp': {
                'yes_dollars': [['0.2500', '10.00']], 'no_dollars': [['0.2500', '10.00']]}}
                for ticker in query['tickers']]}
        else:
            raise AssertionError('unexpected endpoint ' + path)
        return PublicResponse(json.dumps(value).encode())

    def client(self, *args, **kwargs):
        return CatalogClient(*args, **kwargs, sleep=lambda _: None)

    def screen(self, engine, input_path, output_path):
        self.assertEqual(engine, self.engine)
        params = json.loads(input_path.read_text())
        self.inputs.append(params)
        self.assertEqual(len({b['market_id'] for b in params['books']}), len(params['markets']))
        self.assertEqual(params['sizing']['cap_centicontracts'], 10000)
        result = {'kind': 'conditional_payoff_screen', 'production_certificate': False,
                  'simulated_fills': False, 'positive_quotes': 0, 'solver_complete': True,
                  'evaluated_quantities': 200, 'status_counts': {'no_positive_margin': 2}}
        research.write_json(output_path, result)
        return result

    def prepare(self, **kwargs):
        return research.prepare(self.root, self.engine, self.transport, clock=self.clock,
            client_factory=self.client, screen_runner=self.screen, **kwargs)

    def test_public_preparation_freezes_before_books_and_archives_each_task(self):
        result = self.prepare()
        self.assertEqual(result['counts']['selected_baskets'], 2)
        self.assertEqual(result['simulated_orders'], 0)
        self.assertFalse(result['continuous_observation'])
        provenance = json.loads((self.root / 'provenance.json').read_text())
        self.assertTrue(provenance['cohort_frozen_before_books'])
        self.assertFalse(provenance['global_catalog_scan_required'])
        self.assertEqual(len(provenance['archives']), 5)
        for item in provenance['archives']:
            folder = self.root / item['path']
            manifest = json.loads((folder / 'catalog-manifest.json').read_text())
            self.assertEqual(research.digest(folder / 'catalog-manifest.json'), item['manifest_sha256'])
            for name, source in manifest['sources'].items():
                read_archived_response(folder, name, source)
        metadata = json.loads((self.root / 'observation-metadata.json').read_text())
        self.assertEqual(metadata['constraints'], [])
        self.assertEqual(len(metadata['markets']), 5)
        book_calls = [url for url in self.calls if 'orderbooks?' in url]
        self.assertEqual(len(book_calls), 1)
        self.assertFalse(any('/portfolio/' in url or '/account/' in url for url in self.calls))

    def test_parallel_and_serial_qualification_have_same_cohort(self):
        self.prepare(workers=1)
        first = json.loads((self.root / 'qualification.json').read_text())
        self.root = Path(self.temp.name) / 'second'
        self.prepare(workers=4)
        second = json.loads((self.root / 'qualification.json').read_text())
        self.assertEqual(first['baskets'], second['baskets'])
        self.assertEqual(first['selection_policy'], second['selection_policy'])

    def test_unreviewed_terms_stop_before_book_or_engine(self):
        patch.dict(research.BTC, {'terms_sha256': '0' * 64}).start()
        result = self.prepare()
        self.assertEqual(result['counts']['selected_baskets'], 0)
        self.assertIsNone(result['screen'])
        self.assertFalse(self.inputs)
        self.assertFalse(any('orderbooks?' in url for url in self.calls))

    def test_unknown_rule_never_becomes_a_basket(self):
        for market in self.catalogs['KXBTC']['markets']:
            market['rules_primary'] = 'unreviewed settlement'
        result = self.prepare()
        self.assertEqual(result['counts']['selected_baskets'], 0)
        self.assertFalse(self.inputs)

    def test_existing_output_is_not_overwritten(self):
        self.root.mkdir(); (self.root / 'keep').write_text('keep')
        with self.assertRaisesRegex(research.PreparationError, 'output_directory_not_empty'):
            self.prepare()
        self.assertEqual((self.root / 'keep').read_text(), 'keep')
        self.assertFalse(self.calls)

    def test_duplicate_or_incomplete_catalog_does_not_screen(self):
        self.catalogs['KXBTC']['markets'].append(copy.deepcopy(self.catalogs['KXBTC']['markets'][0]))
        with self.assertRaisesRegex(Exception, 'duplicate_markets_ticker'):
            self.prepare()
        self.assertFalse(self.inputs)
        self.assertEqual(json.loads((self.root / 'status.json').read_text())['state'], 'failed')

    def test_conflicting_leg_fees_cannot_enter_native_input(self):
        self.prepare()
        qualified = json.loads((self.root / 'qualification.json').read_text())
        shared = set(leg['ticker'] for leg in qualified['baskets'][0]['legs']) & set(
            leg['ticker'] for leg in qualified['baskets'][1]['legs'])
        self.assertTrue(shared)
        for leg in qualified['baskets'][1]['legs']:
            if leg['ticker'] in shared: leg['coefficient_ppm'] = 0
        with self.assertRaisesRegex(research.PreparationError, 'conflicting_market_fee'):
            research.native_input(qualified, [], 1)

    def test_fee_change_during_preparation_is_not_hidden_in_the_past(self):
        self.sc['KXBTC']['series_fee_change_arr'] = [{'series_ticker': 'KXBTC',
            'scheduled_ts': (NOW + dt.timedelta(milliseconds=5)).isoformat()}]
        with self.assertRaisesRegex(research.PreparationError, 'fee_changed_during_preparation'):
            self.prepare()
        self.assertFalse(self.inputs)

    def test_unsupported_limits_fail_before_network(self):
        for kwargs in ({'workers': 0}, {'cap': 101}, {'max_markets': 101}, {'horizon_seconds': 0}):
            with self.subTest(kwargs=kwargs), self.assertRaisesRegex(research.PreparationError, 'invalid_preparation_limits'):
                self.prepare(**kwargs)
        self.assertFalse(self.calls)

    def test_binary_change_during_preparation_invalidates_provenance(self):
        original = self.screen
        def changing_screen(*args):
            result = original(*args)
            self.engine.write_text('changed binary')
            return result
        self.screen = changing_screen
        with self.assertRaisesRegex(research.PreparationError, 'code_changed_during_preparation'):
            self.prepare()
        self.assertEqual(json.loads((self.root / 'status.json').read_text())['state'], 'failed')


if __name__ == '__main__':
    unittest.main()
