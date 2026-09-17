"""Selection/credential parsing checks; synthetic input only, no network."""
import copy
import datetime as dt
import importlib.util
from pathlib import Path
import tempfile
import sys
import unittest
from unittest.mock import patch
import json

sys.path.insert(0, str(Path(__file__).parents[1] / 'scripts'))
spec = importlib.util.spec_from_file_location('capture_readonly', Path(__file__).parents[1] / 'scripts/capture_readonly.py')
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class RunnerTests(unittest.TestCase):
    def setUp(self):
        self.now = dt.datetime(2026, 9, 16, 12, tzinfo=dt.timezone.utc)
        self.series = {'ticker': 'KXBTCD', 'contract_terms_url': runner.TERMS,
                       'fee_type': 'quadratic', 'fee_multiplier': 1}
        self.public = {'cursor': '', 'markets': [{
            'ticker': 'KXBTCD-EXAMPLE-' + str(i), 'event_ticker': 'KXBTCD-EXAMPLE',
            'close_time': '2026-09-16T21:00:00Z', 'expiration_time': '2026-09-23T21:00:00Z',
            'status': 'active', 'strike_type': 'greater', 'floor_strike': 70000 + i,
            'yes_bid_dollars': '0.40', 'yes_ask_dollars': '0.50', 'notional_value_dollars': '1.0000',
            'rules_primary': "If the simple average of the sixty seconds of CF Benchmarks' Bitcoin Real-Time Index (BRTI) before 5 PM EDT is above " + str(70000 + i) + " at 5 PM EDT on Sep 16, 2026, then the market resolves to Yes.",
            'rules_secondary': 'Same resolution conditions.'} for i in range(8)]}

    def test_payoff_for_every_threshold_interval(self):
        metadata, selected = runner.select_metadata(self.public, self.series, self.now, 7200)
        self.assertEqual(len(metadata['constraints']), 28)
        # Enumerate outcomes by underlying value, independently of generated IDs.
        for underlying in [69999, *[70000 + i + .5 for i in range(8)], 70009]:
            outcomes = {index + 1: underlying > market['floor_strike'] for index, market in enumerate(selected)}
            for constraint in metadata['constraints']:
                relation = constraint['relationship']
                self.assertFalse(outcomes[relation['antecedent']] and not outcomes[relation['consequent']])

    def test_mismatched_rules_rejected(self):
        for field, value in [('rules_primary', 'Different settlement rule'),
                             ('rules_secondary', 'Different exception'),
                             ('expiration_time', '2026-09-24T21:00:00Z'),
                             ('floor_strike', 42), ('notional_value_dollars', '0.50')]:
            with self.subTest(field=field):
                public = copy.deepcopy(self.public)
                public['markets'][0][field] = value
                with self.assertRaises(runner.OperatorError):
                    runner.select_metadata(public, self.series, self.now, 7200)

    def test_closed_window_and_incomplete_listing_rejected(self):
        with self.assertRaises(runner.OperatorError):
            runner.select_metadata(self.public, self.series, self.now + dt.timedelta(hours=8), 7200)
        self.public['cursor'] = 'more'
        with self.assertRaises(runner.OperatorError):
            runner.select_metadata(self.public, self.series, self.now, 7200)

    def research(self, public=None, nfl=None):
        return runner.select_research(public or self.public, self.series, self.now, 7200,
                                      runner.select_metadata, runner.OperatorError,
                                      btc_events=1, btc_per_event=4, nfl_budget=4, nfl_sources=nfl)

    def test_research_coverage_is_deterministic_and_retains_tails(self):
        metadata, selected, report = self.research()
        self.assertEqual([m['floor_strike'] for m in selected], [70000, 70002, 70004, 70007])
        shuffled = copy.deepcopy(self.public)
        shuffled['markets'].reverse()
        self.assertEqual(self.research(shuffled), (metadata, selected, report))
        self.assertEqual(report['groups'][0]['excluded_by_budget'], 4)
        self.assertEqual(len(metadata['constraints']), 6)

    def test_research_does_not_generate_cross_event_relations(self):
        public = copy.deepcopy(self.public)
        extra = copy.deepcopy(public['markets'])
        for m in extra:
            m['ticker'] += '-SECOND'
            m['event_ticker'] += '-SECOND'
        public['markets'] += extra
        metadata, chosen, _ = runner.select_research(public, self.series, self.now, 7200,
            runner.select_metadata, runner.OperatorError, btc_events=2, btc_per_event=4, nfl_budget=0)
        by_id = {m['id']: chosen[m['id'] - 1]['event_ticker'] for m in metadata['markets']}
        self.assertEqual(len(metadata['constraints']), 12)
        for c in metadata['constraints']:
            r = c['relationship']
            self.assertEqual(by_id[r['antecedent']], by_id[r['consequent']])

    def test_nfl_is_observation_only_without_fees_or_constraints(self):
        nfl = {}
        for series in runner.NFL_TERMS:
            nfl[series] = {'markets': [dict(m, ticker=series + '-GROUP-' + str(i),
                                          event_ticker=series + '-GROUP')
                                      for i, m in enumerate(self.public['markets'][:2])]}
        metadata, chosen, report = self.research(nfl=nfl)
        self.assertEqual(len(chosen), 8)
        self.assertEqual(report['observation_only_markets'], 4)
        self.assertEqual(len(metadata['constraints']), 6)
        self.assertEqual({fee['market_id'] for fee in runner.paper_policy(metadata)['fees']}, {1, 2, 3, 4})
        for c in metadata['constraints']:
            self.assertLessEqual(c['relationship']['antecedent'], 4)
            self.assertLessEqual(c['relationship']['consequent'], 4)

    def test_research_budgets_and_short_universe_fail_explicitly(self):
        with self.assertRaises(runner.OperatorError):
            runner.select_research(self.public, self.series, self.now, 7200,
                runner.select_metadata, runner.OperatorError, btc_events=4, btc_per_event=32)
        with self.assertRaises(runner.OperatorError):
            runner.select_research(self.public, self.series, self.now, 7200,
                runner.select_metadata, runner.OperatorError, btc_events=2)

    def test_pagination_archives_pages_and_rejects_duplicates_and_loops(self):
        responses = [{'markets': [{'ticker': 'A'}], 'cursor': 'a b'},
                     {'markets': [{'ticker': 'B'}], 'cursor': ''}]
        with tempfile.TemporaryDirectory() as temporary:
            root, sources = Path(temporary), {}
            with patch.object(runner, 'public_get', side_effect=[json.dumps(r).encode() for r in responses]) as get:
                result = runner.archive_markets(root, 'KXBTCD', sources)
                self.assertIn('cursor=a+b', get.call_args_list[1].args[0])
            self.assertEqual([m['ticker'] for m in result['markets']], ['A', 'B'])
            self.assertEqual(len(sources), 2)
            for malformed in [responses[:1] * 2, [responses[0], {'markets': [], 'cursor': 'a b'}]]:
                with patch.object(runner, 'public_get', side_effect=[json.dumps(r).encode() for r in malformed]):
                    with self.assertRaises(runner.OperatorError):
                        runner.archive_markets(root, 'KXBTCD', {})
            with patch.object(runner, 'public_get', return_value=json.dumps(responses[0]).encode()):
                with self.assertRaises(runner.OperatorError):
                    runner.archive_markets(root, 'KXBTCD', {}, max_pages=1)

    def test_credentials_are_data_not_shell(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            key = root / 'key.pem'
            key.write_text('synthetic fixture, not a private key')
            env = root / 'settings'
            env.write_text('export EME_KALSHI_KEY_ID="fixture"\nexport EME_KALSHI_PRIVATE_KEY_PATH="' + str(key) + '"\n')
            self.assertEqual(runner.credentials(env)['EME_KALSHI_KEY_ID'], 'fixture')
            env.write_text('EME_KALSHI_KEY_ID="$(touch should-not-exist)"\nEME_KALSHI_PRIVATE_KEY_PATH="' + str(key) + '"\n')
            with self.assertRaises(runner.OperatorError):
                runner.credentials(env)
            self.assertFalse((root / 'should-not-exist').exists())

    def test_paper_policy_is_frozen_without_future_labels(self):
        metadata, _ = runner.select_metadata(self.public, self.series, self.now, 7200)
        policy = runner.paper_policy(metadata)
        self.assertEqual(policy['lifecycle']['settlements'], [])
        self.assertEqual({fee['market_id'] for fee in policy['fees']}, set(range(1, 9)))
        self.assertEqual(policy['leg_latency_ns'], [100000000, 100000000])
        self.assertEqual(policy['capital_micro_usd'], 1000000000)
        self.assertEqual(policy, runner.paper_policy(metadata))


if __name__ == '__main__':
    unittest.main()
