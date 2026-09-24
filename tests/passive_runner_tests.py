"""Passive preregistration and offline runner boundaries; no network or credentials."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).parents[1] / 'scripts'))
import passive_probe as runner
import basket_observe_tests as fixtures


class RegistrationTests(unittest.TestCase):
    setUp = fixtures.ObserveTests.setUp
    prepare = fixtures.ObserveTests.prepare
    credentials = fixtures.ObserveTests.credentials
    process = fixtures.ObserveTests.process
    run_observer = fixtures.ObserveTests.run_observer

    def test_passive_registration_exists_before_credentials_and_is_frozen(self):
        probe = self.base / 'probe'; probe.write_bytes(b'fake-offline-binary')
        original = self.credentials
        def auth(path):
            self.assertTrue((self.root / 'passive-plan.json').is_file())
            self.assertTrue((self.root / 'passive-policy.json').is_file())
            return original(path)
        def validate(command, **kwargs):
            self.assertEqual(command[1], '--validate')
            self.assertEqual(self.auth_calls, 0)
            return subprocess.CompletedProcess(command, 0, b'{"valid":true}')
        result = self.run_observer(credential_loader=auth,
            register_analysis=lambda r, b, p: runner.register(r, b, p, probe, process=validate))
        self.assertTrue(result['usable'])
        plan = json.loads((self.root / 'passive-plan.json').read_text())
        self.assertFalse(plan['live_order_simulator'])
        self.assertEqual(plan['orders_sent'], 0)

    def test_failed_passive_validation_prevents_credentials_and_capture(self):
        probe = self.base / 'probe'; probe.write_bytes(b'fake-offline-binary')
        with self.assertRaisesRegex(runner.ObservationError, 'passive_policy_validation_failed'):
            self.run_observer(register_analysis=lambda r, b, p: runner.register(r, b, p, probe,
                process=lambda *args, **kwargs: subprocess.CompletedProcess(args, 1, b'')))
        self.assertEqual((self.auth_calls, self.process_calls), (0, 0))


class AnalysisTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='passive-runner-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / 'session').mkdir()
        (self.root / 'preparation').mkdir()
        self.probe = self.root / 'probe'; self.probe.write_bytes(b'fixture-native')
        self.basket = self.root / 'session/basket-policy.json'; self.basket.write_text('{}')
        runner.write_json(self.root / 'observation-plan.json', {'registered_at': 'fixture'})
        runner.register(self.root, self.basket, {'registered_at': 'fixture'}, self.probe,
            process=lambda *a, **kw: subprocess.CompletedProcess(a, 0, b'{"valid":true}'))

    def process(self, command, **kwargs):
        self.assertEqual(command, [str(self.probe), str(self.root / 'session'), str(self.root / 'passive-policy.json')])
        self.assertNotIn('env', kwargs)
        row = {'type': 'passive_complete', 'orders_sent': 0, 'scenarios': [], 'realized_pnl_micro': None}
        kwargs['stdout'].write(json.dumps(row).encode() + b'\n')
        return subprocess.CompletedProcess(command, 0)

    def test_analysis_checks_registration_and_reports_incomplete_capture(self):
        result = runner.analyze(self.root, self.probe, process=self.process)
        self.assertTrue(result['registered_before_capture'])
        self.assertFalse(result['capture_window_complete'])
        self.assertFalse(result['processing_time_is_live_latency'])
        self.assertTrue((self.root / 'passive-report.json').is_file())
        with self.assertRaisesRegex(runner.ObservationError, 'already_exists'):
            runner.analyze(self.root, self.probe, process=self.process)

    def test_changed_binary_policy_and_plan_are_rejected(self):
        for filename in ('probe', 'passive-policy.json', 'observation-plan.json'):
            with self.subTest(filename=filename):
                path = self.root / filename; original = path.read_bytes(); path.write_bytes(original + b' ')
                with self.assertRaisesRegex(runner.ObservationError, 'registration_changed'):
                    runner.analyze(self.root, self.probe, process=self.process)
                path.write_bytes(original)

    def test_missing_completion_cannot_be_a_successful_report(self):
        def partial(command, **kwargs):
            kwargs['stdout'].write(b'{"type":"passive_start"}\n')
            return subprocess.CompletedProcess(command, 0)
        with self.assertRaisesRegex(runner.ObservationError, 'incomplete'):
            runner.analyze(self.root, self.probe, process=partial)
        self.assertFalse((self.root / 'passive-report.json').exists())


if __name__ == '__main__':
    unittest.main()
