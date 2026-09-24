#!/usr/bin/env python3
"""Record a read-only basket window, then automatically run offline passive scenarios."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time

from basket_observe import observe, read_json, ObservationError
from basket_research import digest, write_json, utcnow, PreparationError
from capture_readonly import REPO, SETTINGS, OperatorError
from public_pool import PublicPool


def policy(basket_path):
    # Operational sensitivity assumptions, not fitted estimates or venue promises.
    return {'schema_version': 1, 'kind': 'passive_basket_probe',
            'basket_policy_sha256': digest(basket_path), 'entry_delay_ms': 100,
            'rest_ms': 5000, 'cancel_delay_ms': 100, 'hedge_delays_ms': [1, 10, 100],
            'reconcile_ms': 250, 'max_gap_ms': 15000, 'trade_lookback_ms': 60000,
            'max_spread_1e4': 500, 'minimum_margin_micro': 10000,
            'maker_coefficient_ppm': 70000, 'max_attempts': 1000,
            'assumed_clock_error_ms': 250}


def register(root, basket_path, observation_plan, probe, *, process=subprocess.run):
    """Freeze the policy and executable before credentials or collection start."""
    probe = Path(probe)
    if not probe.is_file():
        raise ObservationError('missing_passive_probe')
    policy_path = root / 'passive-policy.json'
    write_json(policy_path, policy(basket_path))
    checked = process([str(probe), '--validate', str(root / 'preparation/observation-metadata.json'),
                       str(basket_path), str(policy_path)], capture_output=True, timeout=30)
    if checked.returncode or json.loads(checked.stdout).get('valid') is not True:
        raise ObservationError('passive_policy_validation_failed')
    plan = {'schema_version': 1, 'registered_at': observation_plan['registered_at'],
            'mode': 'read_only_recording_then_offline_counterfactual_simulation',
            'orders_sent': 0, 'live_order_simulator': False, 'own_fills_observed': False,
            'policy_sha256': digest(policy_path), 'native_probe_sha256': digest(probe),
            'runner_sha256': digest(Path(__file__)), 'basket_policy_sha256': digest(basket_path),
            'observation_plan_sha256': digest(root / 'observation-plan.json'),
            'maker_fee_verified': False, 'maker_fee_note': '0.07 coefficient is a declared stress hypothesis, not an account-specific fee.',
            'analysis_max_trace_events': 100000, 'analysis_timeout_seconds': 600,
            'clock_error_bound_verified': False,
            'queue_models': ['trades_only_priority', 'unmatched_reductions_ahead_sensitivity'],
            'selection': 'first eligible update; best total costed margin among its dependent baskets; no future trade support',
            'confirmation': 'Keep future whole expiries/days unused for changes; this initial window is exploratory.',
            'assumptions': ['Exact-price non-block public trades only; aggregate queue identity is unknown.',
                'No fills from book reductions; favorable cancellation priority is a sensitivity case.',
                'No overlapping entries within a scenario; completed baskets keep capital locked.',
                'Sequential hedge arrival delays are assumptions, not measured exchange execution latency.',
                'Clock error is assumed bounded by 250 ms; ambiguous trades near activation cannot fill.',
                'Simulation runs after capture; its processing time is not live trading latency.']}
    write_json(root / 'passive-plan.json', plan)
    return policy_path, root / 'passive-plan.json', probe, Path(__file__)


def analyze(root, probe, *, process=subprocess.run, exploratory=False):
    root, probe = Path(root), Path(probe)
    output = root / 'passive.jsonl'
    report_path = root / 'passive-report.json'
    if output.exists() or report_path.exists():
        raise ObservationError('passive_output_already_exists')
    basket_path = root / 'session/basket-policy.json'
    policy_path = root / 'passive-policy.json'
    if not basket_path.is_file():
        raise ObservationError('no_finalized_basket_session')
    if exploratory:
        if policy_path.exists():
            raise ObservationError('refuse_replace_registered_passive_policy')
        write_json(policy_path, policy(basket_path))
    else:
        plan = read_json(root / 'passive-plan.json')
        if (plan['policy_sha256'] != digest(policy_path) or plan['native_probe_sha256'] != digest(probe) or
                plan['basket_policy_sha256'] != digest(basket_path) or plan['runner_sha256'] != digest(Path(__file__)) or
                plan['observation_plan_sha256'] != digest(root / 'observation-plan.json')):
            raise ObservationError('passive_registration_changed')
    started = time.monotonic()
    with output.open('xb') as stream:
        # The executable has no transport or credentials interface. No stderr is persisted.
        run = process([str(probe), str(root / 'session'), str(policy_path)], stdout=stream,
                      stderr=subprocess.DEVNULL, timeout=600)
    if run.returncode:
        raise ObservationError('passive_analysis_failed')
    last = None
    with output.open('rb') as stream:
        for line in stream:
            if len(line) > 1024 * 1024:
                raise ObservationError('passive_output_record_too_large')
            last = json.loads(line)
    if not last or last.get('type') != 'passive_complete' or last.get('orders_sent') != 0:
        raise ObservationError('passive_output_incomplete')
    capture = read_json(root / 'result.json') if (root / 'result.json').is_file() else {}
    last.update(registered_before_capture=not exploratory,
                capture_window_complete=capture.get('planned_window_complete') is True,
                capture_usable=capture.get('usable') is True,
                offline_processing_seconds=time.monotonic() - started,
                processing_time_is_live_latency=False,
                trace_sha256=digest(output), native_probe_sha256=digest(probe))
    write_json(report_path, last)
    return last


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', type=Path, default=REPO / 'out/bin/eme-passive-probe')
    parser.add_argument('--engine', type=Path, default=REPO / 'out/bin/event-engine')
    parser.add_argument('--binary', type=Path, default=REPO / 'out/bin/eme-capture')
    parser.add_argument('--settings', type=Path, default=SETTINGS)
    parser.add_argument('--output', type=Path, default=REPO / 'captures')
    parser.add_argument('--seconds', type=int, default=1800)
    parser.add_argument('--max-mib', type=int, default=256)
    parser.add_argument('--prepare-only', action='store_true')
    parser.add_argument('--analyze-existing', type=Path)
    args = parser.parse_args()
    os.umask(0o077)
    root = args.analyze_existing or args.output.resolve() / utcnow().strftime('passive-%Y%m%dT%H%M%S.%fZ')
    try:
        if not args.probe.is_file():
            raise ObservationError('missing_passive_probe')
        if args.analyze_existing:
            # New hypothesis on old data is always labeled exploratory.
            report = analyze(root, args.probe, exploratory=not (root / 'passive-plan.json').exists())
        else:
            print('Grabacion sin ordenes; simulacion automatica AL TERMINAR. Carpeta: ' + str(root), flush=True)
            print('Politica fijada antes de capturar: dos supuestos de cola y coberturas a 1/10/100 ms por pata.', flush=True)
            with PublicPool(workers=4) as transport:
                result = observe(root, args.engine, args.binary, args.settings, transport,
                    seconds=args.seconds, max_mib=args.max_mib, window_label='passive-entry-exploration',
                    prepare_only=args.prepare_only, progress=lambda message: print(message, flush=True),
                    register_analysis=lambda r, b, p: register(r, b, p, args.probe))
            if args.prepare_only or result.get('reason') == 'no_qualified_observation_cohort':
                print('Preparacion finalizada. No se ha iniciado la captura.' if args.prepare_only else 'No hay una cohorte valida; no se inicia la captura.')
                return 0
            print('Captura terminada. Analizando escenarios con el replay nativo...', flush=True)
            report = analyze(root, args.probe)
        for scenario in report['scenarios']:
            print(str(scenario['scenario']) + ': ' + scenario['queue_model'] + ', retraso/pata=' +
                  str(scenario['hedge_leg_delay_ns'] // 1000000) + ' ms; fills modelados=' +
                  str(scenario['modeled_fill_events']) + '; cestas completas=' + str(scenario['completed_baskets']) +
                  '; margen condicional=$' + format(scenario['conditional_margin_micro'] / 1000000, '.4f'))
        print('No son beneficios realizados. Informe: ' + str(root / 'passive-report.json'))
        print('Datos y trazas para analizar: ' + str(root))
        if not report['capture_window_complete'] or report['continuity_failed']:
            print('Ventana incompleta: resultados parciales, sin validacion de rentabilidad.')
        return 0
    except (ObservationError, PreparationError, OperatorError, OSError, ValueError, KeyError, TypeError, subprocess.TimeoutExpired) as error:
        reason = str(error) if isinstance(error, ObservationError) else type(error).__name__
        print('Prueba detenida: ' + reason + '. Datos conservados en: ' + str(root), file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
