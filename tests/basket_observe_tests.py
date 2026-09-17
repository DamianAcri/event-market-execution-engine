"""Prospective runner tests with public fixtures and fake collector; no secrets/network."""
import datetime as dt
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).parents[1] / 'scripts'))
import basket_observe as observe
import basket_families
from basket_families_tests import fixture


def complete_summary():
    return {'type': 'basket_complete', 'mode': 'live_conditional_observation_no_orders',
            'live_replay_equal': True, 'incomplete': False, 'event_budget_exceeded': False,
            'orders_sent': 0, 'simulated_fills': False, 'policy_expired': False}


class FinishedChild:
    returncode = 0

    def poll(self):
        return self.returncode

    def wait(self, timeout=None):
        return self.returncode

    def terminate(self):
        raise AssertionError('No termination expected for completed fixture')


class ObserveTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='basket-observe-test-')
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.root = self.base / 'run'
        self.engine = self.base / 'event-engine'; self.engine.write_bytes(b'fixture-engine')
        self.binary = self.base / 'eme-capture'; self.binary.write_bytes(b'fixture-capture')
        self.now = dt.datetime(2026, 9, 17, 12, tzinfo=dt.timezone.utc)
        self.auth_calls, self.process_calls = 0, 0
        self.summary = complete_summary()
        self.mutate = lambda q, m: None
        self.prepared_horizon = None

    def prepare(self, root, engine, transport, **kwargs):
        self.prepared_horizon = kwargs['horizon_seconds']
        catalogs, series, events, sc, ec = fixture()
        end = self.now + dt.timedelta(seconds=kwargs['horizon_seconds'])
        q = basket_families.qualify_baskets(catalogs, series, events,
            {s: basket_families.BTC['terms_sha256'] for s in basket_families.SERIES},
            self.now, end, sc, ec, metadata_observed_at=self.now,
            max_baskets=kwargs['max_baskets'], max_markets=kwargs['max_markets'])
        q.update(cohort_frozen_at=self.now.isoformat(), fee_verified_from=self.now.isoformat(),
                 fee_verified_until=end.isoformat())
        params = observe.native_input(q, [], 0)
        metadata = {'schema_version': 1, 'metadata_version': 20260917120000,
                    'venue': 'kalshi', 'markets': params['markets'], 'constraints': []}
        self.mutate(q, metadata)
        root.mkdir(parents=True)
        observe.write_json(root / 'qualification.json', q)
        observe.write_json(root / 'observation-metadata.json', metadata)
        observe.write_json(root / 'provenance.json', {'fixture': True})

    def credentials(self, path):
        self.auth_calls += 1
        self.assertTrue((self.root / 'observation-plan.json').is_file())
        return {'EME_KALSHI_KEY_ID': 'fixture-private-id',
                'EME_KALSHI_PRIVATE_KEY_PATH': 'fixture-private-key-path'}

    def process(self, command, *, env, stdout, stderr):
        self.process_calls += 1
        self.assertEqual(command[4:], ['production', '--basket-observe',
            str((self.root / 'observation-policy.json').resolve()), '--public-trades'])
        self.assertNotIn('--paper', command)
        self.assertEqual(env['EME_KALSHI_KEY_ID'], 'fixture-private-id')
        self.assertEqual(stderr, subprocess.DEVNULL)
        session = Path(command[2]); session.mkdir()
        observe.write_json(session / 'basket-summary.json', self.summary)
        stdout.write(json.dumps({'finalized': True, 'market_updates': 30,
                               'public_trades': 2, 'connections': 1}).encode())
        return FinishedChild()

    def run_observer(self, **kwargs):
        return observe.observe(self.root, self.engine, self.binary,
            self.base / 'never-read-settings', object(),
            prepare_runner=kwargs.pop('prepare_runner', self.prepare),
            credential_loader=kwargs.pop('credential_loader', self.credentials),
            process_factory=self.process, clock=lambda: self.now,
            free_reader=lambda p: 4 * 1024**3,
            **kwargs)

    def test_valid_observation_freezes_plan_then_uses_only_observer_collector_mode(self):
        result = self.run_observer()
        self.assertTrue(result['usable'])
        self.assertEqual((self.auth_calls, self.process_calls), (1, 1))
        self.assertEqual(self.prepared_horizon, 2400)
        plan = json.loads((self.root / 'observation-plan.json').read_text())
        self.assertFalse(plan['duration_is_statistical_proof'])
        self.assertFalse(plan['own_fills_or_profit_measured'])
        self.assertIn('future expiries', plan['confirmation_requirement'])
        for path in self.root.rglob('*.json'):
            content = path.read_text()
            self.assertNotIn('fixture-private-id', content)
            self.assertNotIn('fixture-private-key-path', content)

    def test_policy_binds_canonical_metadata_qualification_window_and_mode(self):
        self.run_observer(prepare_only=True)
        policy = json.loads((self.root / 'observation-policy.json').read_text())
        metadata = json.loads((self.root / 'preparation/observation-metadata.json').read_text())
        canonical = json.dumps(metadata, sort_keys=True, separators=(',', ':'), ensure_ascii=False).encode()
        self.assertEqual(policy['metadata_sha256'], hashlib.sha256(canonical).hexdigest())
        self.assertEqual(policy['qualification_sha256'], observe.digest(self.root / 'preparation/qualification.json'))
        self.assertEqual(policy['valid_from_unix_ms'], int(self.now.timestamp() * 1000))
        self.assertGreater(policy['valid_until_unix_ms'], policy['valid_from_unix_ms'] + 1800000)
        self.assertEqual(policy['screen']['books'], [])
        self.assertEqual(policy['freshness_mode'], 'contiguous_shared_stream')

    def test_prepare_only_never_reads_credentials_or_requires_collector(self):
        self.binary.unlink()
        result = self.run_observer(prepare_only=True)
        self.assertEqual(result['reason'], 'prepare_only')
        self.assertEqual((self.auth_calls, self.process_calls), (0, 0))

    def test_empty_cohort_never_reads_credentials(self):
        self.mutate = lambda q, m: q.update(baskets=[], markets=[])
        result = self.run_observer()
        self.assertEqual(result['reason'], 'no_qualified_observation_cohort')
        self.assertEqual((self.auth_calls, self.process_calls), (0, 0))

    def test_unverified_fees_never_read_credentials(self):
        self.mutate = lambda q, m: q['baskets'][0].update(fee_verified=False)
        with self.assertRaisesRegex(observe.ObservationError, 'no_qualified_observation_cohort'):
            self.run_observer()
        self.assertEqual((self.auth_calls, self.process_calls), (0, 0))

    def test_expired_fee_or_stale_metadata_stops_before_credentials(self):
        self.mutate = lambda q, m: q.update(fee_verified_until=self.now.isoformat())
        with self.assertRaisesRegex(observe.ObservationError, 'observation_window_expired'):
            self.run_observer()
        self.assertEqual((self.auth_calls, self.process_calls), (0, 0))

    def test_changed_sources_stop_before_credentials(self):
        count = [0]
        def hashes():
            count[0] += 1
            return {'fixture': 'before' if count[0] == 1 else 'changed'}
        with self.assertRaisesRegex(observe.ObservationError, 'source_changed_before_capture'):
            self.run_observer(hash_reader=hashes)
        self.assertEqual((self.auth_calls, self.process_calls), (0, 0))

    def test_changed_policy_during_credential_load_never_starts_collector(self):
        def loader(path):
            values = self.credentials(path)
            (self.root / 'observation-policy.json').write_text('{}')
            return values
        with self.assertRaisesRegex(observe.ObservationError, 'source_changed_before_capture'):
            self.run_observer(credential_loader=loader)
        self.assertEqual((self.auth_calls, self.process_calls), (1, 0))

    def test_expiry_during_credential_load_never_starts_collector(self):
        def loader(path):
            values = self.credentials(path)
            self.now += dt.timedelta(seconds=301)
            return values
        with self.assertRaisesRegex(observe.ObservationError, 'stale_or_future_preparation'):
            self.run_observer(credential_loader=loader)
        self.assertEqual(self.process_calls, 0)

    def test_mismatched_metadata_never_reads_credentials(self):
        self.mutate = lambda q, m: m.update(constraints=[{'not': 'observation-only'}])
        with self.assertRaisesRegex(observe.ObservationError, 'observation_metadata_mismatch'):
            self.run_observer()
        self.assertEqual(self.auth_calls, 0)

    def test_truncated_or_nonmatching_summary_is_not_usable(self):
        self.summary['event_budget_exceeded'] = True
        result = self.run_observer()
        self.assertFalse(result['usable'])
        self.assertFalse(result['observation_verified'])
        self.assertTrue(result['live_replay_equal'])

    def test_successful_replay_boolean_without_completion_flags_is_not_usable(self):
        self.summary = {'live_replay_equal': True}
        self.assertFalse(self.run_observer()['usable'])

    def test_early_policy_expiry_preserves_replay_but_marks_planned_window_incomplete(self):
        self.summary['policy_expired'] = True
        result = self.run_observer()
        self.assertFalse(result['usable'])
        self.assertFalse(result['planned_window_complete'])
        self.assertTrue(result['live_replay_equal'])
        self.assertTrue(result['observation_verified'])
        self.assertTrue(result['policy_expired'])
        self.assertEqual(result['reason'], 'policy_window_expired_before_observation_finished')
        self.assertIn('analisis parcial', result['explanation'])

    def test_summary_without_policy_validity_is_not_verified(self):
        del self.summary['policy_expired']
        result = self.run_observer()
        self.assertFalse(result['usable'])
        self.assertFalse(result['observation_verified'])

    def test_source_change_after_collector_is_preserved_as_incomplete_result(self):
        def finish(child, root, seconds, max_mib):
            self.binary.write_bytes(b'changed-collector')
            return None
        result = self.run_observer(monitor_runner=finish)
        self.assertFalse(result['usable'])
        self.assertFalse(result['source_unchanged'])

    def test_invalid_feed_budget_and_duration_fail_before_preparation(self):
        for limits in ({'seconds': 10801}, {'max_markets': 65}, {'cap': 101}, {'workers': 9}):
            with self.subTest(limits=limits), self.assertRaisesRegex(observe.ObservationError, 'invalid_observation_limits'):
                self.run_observer(**limits)
        self.assertFalse(self.root.exists())

    def test_storage_guard_terminates_and_reports_soft_limit(self):
        class Waiting:
            returncode = None
            def poll(self): return self.returncode
            def wait(self, timeout=None):
                if self.returncode is None: raise subprocess.TimeoutExpired('fixture', timeout)
                return self.returncode
            def terminate(self): self.returncode = 0
            def kill(self): self.returncode = -9
        ticks = iter((0, 5, 5, 5))
        result = observe.monitor(Waiting(), self.root, 1800, 32,
            monotonic=lambda: next(ticks), size_reader=lambda p: 33 * 1024**2,
            free_reader=lambda p: 4 * 1024**3, progress=lambda s: None)
        self.assertEqual(result, 'storage_limit')


if __name__ == '__main__':
    unittest.main()
