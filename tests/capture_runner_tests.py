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
import io
import hashlib
from contextlib import redirect_stdout

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

    def test_verified_economic_fee_map_is_complete_and_exact(self):
        metadata, _ = runner.select_metadata(self.public, self.series, self.now, 7200)
        mapping = {m['ticker']: 35000 if m['id'] % 2 else 140000 for m in metadata['markets']}
        policy = runner.paper_policy(metadata, mapping)
        self.assertEqual([fee['coefficient_ppm'] for fee in policy['fees']], [35000, 140000] * 4)
        self.assertIn('event overrides', policy['fee_provenance'])
        self.assertEqual(policy['lifecycle']['settlements'], [])
        malformed = [dict(mapping, unrelated=70000), dict(list(mapping.items())[1:])]
        for value in (True, -1, 1000001, 70000.0, '70000'):
            changed = dict(mapping)
            changed[next(iter(mapping))] = value
            malformed.append(changed)
        for value in malformed:
            with self.subTest(mapping=value):
                with self.assertRaises(runner.OperatorError):
                    runner.paper_policy(metadata, value)
        self.assertTrue(all(f['coefficient_ppm'] == 70000 for f in runner.paper_policy(metadata)['fees']))

    def economic_result(self, root, *, no_run=False, fee_expired=False, close_expired=False):
        now = dt.datetime.now(dt.timezone.utc)
        metadata = {'schema_version': 1, 'metadata_version': 1, 'venue': 'kalshi',
                    'markets': [{'id': 1, 'ticker': 'A'}, {'id': 2, 'ticker': 'B'}],
                    'constraints': [{'relationship': {'type': 'implication', 'antecedent': 2, 'consequent': 1}}]}
        selected = [{'ticker': ticker, 'close_time': (now + dt.timedelta(seconds=30 if close_expired else 7200)).isoformat()}
                    for ticker in ('A', 'B')]
        if no_run:
            metadata['markets'], metadata['constraints'], selected = [], [], []
        report = {'selection_method': 'Synthetic native cost/depth screen',
                  'selection_frozen_at': now.isoformat(), 'observation_only_markets': 0,
                  'catalog': {'market_count': 500}, 'qualified_markets': 20,
                  'books_examined': 20, 'activity_markets_examined': 4,
                  'positive_indicative_pairs_selected': 0,
                  'selected_pair_reasons': [] if no_run else [{'selection_class': 'active_near_margin'}],
                  'selected_markets': len(selected), 'certified_relationships': len(metadata['constraints']),
                  'fees_by_ticker': {} if no_run else {'A': 35000, 'B': 140000},
                  'fee_verified_until': (now + dt.timedelta(seconds=-1 if fee_expired else 1000)).isoformat(),
                  'decision': 'no_run' if no_run else 'capture_active_watchlist'}
        (root / 'public').mkdir()
        (root / 'public/catalog-manifest.json').write_text('{"fixture":true}\n')
        return metadata, selected, report, {'fixture.json': {'sha256': 'fixture'}}

    def test_economic_prepare_is_public_only_and_hashes_complete_provenance(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            engine = folder / 'event-engine'; engine.write_bytes(b'synthetic native screen engine')
            argv = ['capture', '--profile', 'economic', '--paper', '--prepare-only', '--seconds', '60',
                    '--engine', str(engine), '--output', str(folder / 'capture-output')]
            with patch.object(sys, 'argv', argv), patch.object(runner, 'prepare_economic',
                    side_effect=lambda root, *a, **kw: self.economic_result(root)) as prepare, \
                    patch.object(runner, 'credentials') as credentials, \
                    patch.object(runner, 'archive_markets') as legacy_listing, \
                    patch.object(runner.subprocess, 'Popen') as process, redirect_stdout(io.StringIO()) as output:
                self.assertEqual(runner.main(), 0)
            credentials.assert_not_called(); process.assert_not_called(); legacy_listing.assert_not_called()
            self.assertEqual(prepare.call_args.kwargs, {'market_budget': 64, 'book_budget': 1024,
                                                        'activity_budget': 128, 'max_pages': 500})
            root = next((folder / 'capture-output').iterdir())
            provenance = json.loads((root / 'provenance.json').read_text())
            self.assertTrue(provenance['public_trades'])
            self.assertEqual(provenance['execution'], 'not_started')
            self.assertEqual(provenance['sources_base'], '.')
            self.assertEqual(provenance['native_engine_sha256'], hashlib.sha256(engine.read_bytes()).hexdigest())
            self.assertEqual(provenance['catalog_manifest_sha256'],
                             hashlib.sha256((root / 'public/catalog-manifest.json').read_bytes()).hexdigest())
            self.assertEqual(set(provenance['selector_modules_sha256']),
                             {'research_selection.py', 'economic_selection.py', 'market_catalog.py', 'market_families.py'})
            policy = json.loads((root / 'paper-policy.json').read_text())
            self.assertEqual([f['coefficient_ppm'] for f in policy['fees']], [35000, 140000])
            self.assertIn('Catalogo: 500 mercados abiertos; 20 contratos', output.getvalue())
            self.assertIn('0 con margen indicativo positivo; 1 para observar actividad', output.getvalue())

    def test_economic_zero_choices_writes_decision_without_authentication(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            engine = folder / 'engine'; engine.write_text('synthetic')
            binary = folder / 'capture'; binary.write_text('synthetic')
            argv = ['capture', '--profile', 'economic', '--paper', '--engine', str(engine),
                    '--binary', str(binary), '--output', str(folder / 'output')]
            output = io.StringIO()
            with patch.object(sys, 'argv', argv), patch.object(runner, 'prepare_economic',
                    side_effect=lambda root, *a, **kw: self.economic_result(root, no_run=True)), \
                    patch.object(runner, 'credentials') as credentials, \
                    patch.object(runner.subprocess, 'Popen') as process, redirect_stdout(output):
                self.assertEqual(runner.main(), 0)
            credentials.assert_not_called(); process.assert_not_called()
            root = next((folder / 'output').iterdir())
            self.assertEqual(json.loads((root / 'metadata.json').read_text())['markets'], [])
            self.assertEqual(json.loads((root / 'coverage.json').read_text())['decision'], 'no_run')
            self.assertEqual(json.loads((root / 'result.json').read_text())['execution'], 'not_started')
            self.assertIn('No se inicia la captura', output.getvalue())

    def test_economic_requires_native_engine_even_when_only_preparing(self):
        with tempfile.TemporaryDirectory() as temporary:
            argv = ['capture', '--profile', 'economic', '--prepare-only', '--engine', str(Path(temporary) / 'absent')]
            with patch.object(sys, 'argv', argv), patch.object(runner, 'prepare_economic') as prepare, \
                    patch.object(runner, 'credentials') as credentials:
                with self.assertRaisesRegex(runner.OperatorError, 'Falta event-engine'):
                    runner.main()
            prepare.assert_not_called(); credentials.assert_not_called()

    def test_economic_native_timeout_has_fixed_message_without_authentication(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            engine = folder / 'engine'; engine.write_text('synthetic')
            argv = ['capture', '--profile', 'economic', '--prepare-only', '--engine', str(engine),
                    '--output', str(folder / 'output')]
            with patch.object(sys, 'argv', argv), patch.object(runner, 'prepare_economic',
                    side_effect=runner.subprocess.TimeoutExpired('arbitrary command details omitted', 60)), \
                    patch.object(runner, 'credentials') as credentials, redirect_stdout(io.StringIO()):
                with self.assertRaisesRegex(runner.OperatorError, '^La evaluacion nativa ha superado su limite de tiempo\\.'):
                    runner.main()
            credentials.assert_not_called()

    def test_economic_budget_validation_occurs_before_public_requests(self):
        invalid = [('--market-budget', '1'), ('--market-budget', '65'), ('--activity-budget', '63'),
                   ('--activity-budget', '513'), ('--book-budget', '127'), ('--book-budget', '2049'),
                   ('--discovery-max-pages', '0'), ('--discovery-max-pages', '1001')]
        for option, value in invalid:
            with self.subTest(option=option, value=value), patch.object(sys, 'argv',
                    ['capture', '--profile', 'economic', '--prepare-only', option, value]), \
                    patch.object(runner, 'prepare_economic') as prepare:
                with self.assertRaisesRegex(runner.OperatorError, 'Presupuestos'):
                    runner.main()
                prepare.assert_not_called()

    def test_economic_rechecks_fee_and_close_horizon_before_credentials(self):
        for kind, reason in [('fee_expired', 'ventana de comisiones'), ('close_expired', 'margen de cierre')]:
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as temporary:
                folder = Path(temporary)
                engine = folder / 'engine'; engine.write_text('synthetic')
                binary = folder / 'capture'; binary.write_text('synthetic')
                argv = ['capture', '--profile', 'economic', '--seconds', '60', '--engine', str(engine),
                        '--binary', str(binary), '--output', str(folder / 'output')]
                with patch.object(sys, 'argv', argv), patch.object(runner, 'prepare_economic',
                        side_effect=lambda root, *a, **kw: self.economic_result(root, **{kind: True})), \
                        patch.object(runner.shutil, 'disk_usage') as disk, \
                        patch.object(runner, 'credentials') as credentials, \
                        patch.object(runner.subprocess, 'Popen') as process, redirect_stdout(io.StringIO()):
                    disk.return_value.free = 4 * 1024**3
                    with self.assertRaisesRegex(runner.OperatorError, reason):
                        runner.main()
                credentials.assert_not_called(); process.assert_not_called()

    def test_economic_capture_passes_public_trades_and_verified_policy_to_readonly_binary(self):
        class SyntheticChild:
            returncode = 0
            stdout = io.BytesIO(json.dumps({'finalized': True, 'market_updates': 4,
                                           'connections': 1, 'reason': 'completed', 'public_trades': 2}).encode())

            def poll(self):
                return 0

            def wait(self):
                return 0

        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            engine = folder / 'engine'; engine.write_text('synthetic')
            binary = folder / 'capture'; binary.write_text('synthetic')
            argv = ['capture', '--profile', 'economic', '--paper', '--seconds', '60', '--engine', str(engine),
                    '--binary', str(binary), '--output', str(folder / 'output')]

            def spawn_fixture(command, **kwargs):
                session = Path(command[2]); session.mkdir()
                (session / 'paper-summary.json').write_text(json.dumps({
                    'live_replay_equal': True, 'attempts': 0,
                    'lifecycle': {'orders': 0, 'simulated_net_pnl_micro_usd': 0}}))
                return SyntheticChild()

            with patch.object(sys, 'argv', argv), patch.object(runner, 'prepare_economic',
                    side_effect=lambda root, *a, **kw: self.economic_result(root)), \
                    patch.object(runner.shutil, 'disk_usage') as disk, \
                    patch.object(runner, 'credentials', return_value={}) as credentials, \
                    patch.object(runner.subprocess, 'Popen', side_effect=spawn_fixture) as process, \
                    redirect_stdout(io.StringIO()):
                disk.return_value.free = 4 * 1024**3
                self.assertEqual(runner.main(), 0)
            credentials.assert_called_once()
            command = process.call_args.args[0]
            self.assertEqual(command[0], str(binary.resolve()))
            self.assertEqual(command[3:5], ['60', 'production'])
            self.assertEqual(Path(command[5]).name, 'paper-policy.json')
            self.assertEqual(command[6], '--public-trades')


if __name__ == '__main__':
    unittest.main()
