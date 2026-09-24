import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).parents[1] / 'scripts'))
import basket_execution_study as runner


class RunnerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.capture = self.base / 'capture'
        (self.capture / 'session').mkdir(parents=True)
        for name in ('basket-policy.json', 'manifest.json'):
            (self.capture / 'session' / name).write_text('{}')
        (self.capture / 'result.json').write_text('{"planned_window_complete":false}')
        self.binary = self.base / 'binary'
        self.binary.write_bytes(b'fixture-binary')
        self.out = self.base / 'study'

    def process(self, command, **kwargs):
        self.assertEqual(command[1], str((self.capture / 'session').resolve()))
        self.assertTrue((self.out / 'plan.json').is_file())
        self.assertEqual(json.loads((self.out / 'policy.json').read_text())['arrival_and_response_ms'], [1, 10, 50, 100, 250])
        self.assertNotIn('env', kwargs)
        kwargs['stdout'].write(b'{"type":"basket_execution_complete","orders_sent":0,"scenarios":[]}\n')
        return subprocess.CompletedProcess(command, 0)

    def test_fixed_policy_before_processing_and_incomplete_window_retained(self):
        with patch.object(runner.subprocess, 'run', side_effect=self.process):
            report = runner.run(self.capture, self.binary, self.out)
        self.assertFalse(report['capture_window_complete'])
        self.assertFalse(report['registered_before_capture'])
        self.assertTrue((self.out / 'report.json').is_file())

    def test_output_directory_is_never_replaced(self):
        self.out.mkdir()
        with patch.object(runner.subprocess, 'run') as process:
            with self.assertRaises(FileExistsError):
                runner.run(self.capture, self.binary, self.out)
            process.assert_not_called()

    def test_partial_trace_cannot_pass(self):
        def partial(command, **kwargs):
            kwargs['stdout'].write(b'{"type":"basket_execution_start"}\n')
        with patch.object(runner.subprocess, 'run', side_effect=partial):
            with self.assertRaisesRegex(ValueError, 'incomplete'):
                runner.run(self.capture, self.binary, self.out)
        self.assertFalse((self.out / 'report.json').exists())

    def test_mutated_input_cannot_pass(self):
        def change(command, **kwargs):
            self.process(command, **kwargs)
            (self.capture / 'session/basket-policy.json').write_text('{"changed":true}')
        with patch.object(runner.subprocess, 'run', side_effect=change):
            with self.assertRaisesRegex(ValueError, 'inputs_changed'):
                runner.run(self.capture, self.binary, self.out)
        self.assertFalse((self.out / 'report.json').exists())


if __name__ == '__main__':
    unittest.main()
