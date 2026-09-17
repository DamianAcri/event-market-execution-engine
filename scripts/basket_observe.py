#!/usr/bin/env python3
"""Prospective basket observation and raw recording; no orders or simulated fills."""
import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

from basket_research import (PreparationError, digest, native_input, prepare,
                             utcnow, write_json)
from capture_readonly import OperatorError, REPO, SETTINGS, credentials
from economic_selection import timestamp
from public_pool import PublicPool

MODULES = ('basket_observe.py', 'basket_research.py', 'basket_families.py',
           'public_pool.py', 'market_catalog.py', 'market_families.py',
           'economic_selection.py', 'capture_readonly.py', 'research_selection.py')
MAX_EPISODE_EVENTS = 100000


class ObservationError(ValueError):
    """Fixed reason codes; never credential file or subprocess contents."""


def source_hashes():
    return {name: digest(Path(__file__).with_name(name)) for name in MODULES}


def read_json(path, maximum=32 * 1024 * 1024):
    if not path.is_file() or path.stat().st_size > maximum:
        raise ObservationError('missing_or_oversized_artifact')
    return json.loads(path.read_bytes())


def directory_size(root):
    return sum(p.stat().st_size for p in root.rglob('*') if p.is_file() and not p.is_symlink())


def check_window(qualification, now, seconds):
    if (qualification.get('model_classification') != 'conditional_common_scalar_observation_only' or
            qualification.get('observation_only') is not True or not qualification.get('baskets') or
            not 3 <= len(qualification.get('markets', [])) <= 64 or
            any(b.get('fee_verified') is not True for b in qualification['baskets'])):
        raise ObservationError('no_qualified_observation_cohort')
    age = (now - timestamp(qualification['metadata_observed_at'])).total_seconds()
    until = min([timestamp(qualification['fee_verified_until'])] +
                [timestamp(m['close_time']) for m in qualification['markets']])
    if not 0 <= age <= 300:
        raise ObservationError('stale_or_future_preparation')
    if (timestamp(qualification['cohort_frozen_at']) > now or
            until <= now + dt.timedelta(seconds=seconds)):
        raise ObservationError('observation_window_expired')
    return until


def observation_policy(qualification, qualification_path, metadata, until, cap):
    return {'schema_version': 1, 'kind': 'conditional_basket_observation',
            'freshness_mode': 'contiguous_shared_stream',
            'qualification_sha256': digest(qualification_path),
            'metadata_sha256': hashlib.sha256(json.dumps(metadata, sort_keys=True,
                separators=(',', ':'), ensure_ascii=False).encode()).hexdigest(),
            'valid_from_unix_ms': int(timestamp(qualification['cohort_frozen_at']).timestamp() * 1000),
            'valid_until_unix_ms': int(until.timestamp() * 1000),
            'max_episode_events': MAX_EPISODE_EVENTS,
            'screen': native_input(qualification, [], 0, cap=cap)}


def monitor(child, root, seconds, max_mib, *, monotonic=time.monotonic,
            size_reader=directory_size, free_reader=lambda p: shutil.disk_usage(p).free,
            progress=print):
    """Bounded process/storage guard; collector handles signal finalization."""
    started, stop_at, stopped, last_progress = monotonic(), None, None, 0
    try:
        while child.poll() is None:
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                elapsed = monotonic() - started
                size = size_reader(root)
                if stopped is None and (size > max_mib * 1024**2 or free_reader(root) < 1024**3):
                    stopped, stop_at = 'storage_limit', monotonic()
                    child.terminate()
                if elapsed > seconds + 120 or (stop_at is not None and monotonic() - stop_at > 120):
                    stopped = 'capture_timeout'
                    child.kill()
                if elapsed - last_progress >= 60:
                    progress(str(int(elapsed)) + ' s; ' + str(size // (1024**2)) + ' MiB registrados.')
                    last_progress = elapsed
    except KeyboardInterrupt:
        stopped = 'user_stop'
        child.terminate()
        try:
            child.wait(timeout=30)
        except subprocess.TimeoutExpired:
            child.kill()
    child.wait()
    return stopped


def observe(root, engine, binary, settings, transport, *, seconds=1800, max_mib=256,
            workers=4, max_baskets=20, max_markets=64, cap=100,
            window_label='prospective-window', prepare_only=False, clock=utcnow,
            prepare_runner=prepare, credential_loader=credentials,
            process_factory=subprocess.Popen, monitor_runner=monitor,
            hash_reader=source_hashes, free_reader=lambda p: shutil.disk_usage(p).free,
            progress=lambda message: None):
    """Prepare, preregister and optionally run one fresh observation window.

    Credentials are loaded only after all public preparation, clock, source,
    capacity and policy checks. Never call this function automatically to start
    a long session: the operator runs the emitted CLI command.
    """
    if (type(seconds) is not int or not 1 <= seconds <= 10800 or
            type(max_mib) is not int or not 32 <= max_mib <= 2048 or
            type(max_markets) is not int or not 3 <= max_markets <= 64 or
            type(workers) is not int or not 1 <= workers <= 8 or
            type(max_baskets) is not int or not 1 <= max_baskets <= 100 or
            type(cap) is not int or not 1 <= cap <= 100 or
            not isinstance(window_label, str) or
            re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_. -]{0,79}', window_label) is None):
        raise ObservationError('invalid_observation_limits_or_label')
    root, engine, binary = Path(root), Path(engine), Path(binary)
    if not engine.is_file() or (not prepare_only and not binary.is_file()):
        raise ObservationError('missing_native_binary')
    if root.exists() and (root.is_symlink() or not root.is_dir() or any(root.iterdir())):
        raise ObservationError('output_directory_not_empty')
    root.mkdir(parents=True, mode=0o700, exist_ok=True)
    initial_sources = hash_reader()
    binary_hash = digest(binary) if binary.is_file() else None
    engine_hash = digest(engine)
    result = {'execution': 'not_started', 'simulated_orders': 0, 'simulated_fills': False}

    def stable():
        return (hash_reader() == initial_sources and digest(engine) == engine_hash and
                (binary_hash is None or digest(binary) == binary_hash))

    try:
        preparation = root / 'preparation'
        prepare_runner(preparation, engine, transport, workers=workers,
                       max_baskets=max_baskets, max_markets=max_markets,
                       horizon_seconds=seconds + 600, cap=cap, clock=clock)
        qualification_path = preparation / 'qualification.json'
        qualification = read_json(qualification_path)
        if not qualification.get('baskets'):
            result.update(reason='no_qualified_observation_cohort', usable=False)
            write_json(root / 'result.json', result)
            return result
        now = clock()
        until = check_window(qualification, now, seconds)
        metadata_path = preparation / 'observation-metadata.json'
        metadata = read_json(metadata_path)
        policy = observation_policy(qualification, qualification_path, metadata, until, cap)
        policy_path = root / 'observation-policy.json'
        write_json(policy_path, policy)
        if metadata.get('constraints') != [] or metadata.get('markets') != policy['screen']['markets']:
            raise ObservationError('observation_metadata_mismatch')
        if not stable():
            raise ObservationError('source_changed_before_capture')
        plan = {'schema_version': 1, 'registered_at': now.isoformat(), 'window_label': window_label,
            'planned_duration_seconds': seconds, 'duration_is_statistical_proof': False,
            'planned_collection_start_not_before': now.isoformat(),
            'latest_permitted_end': until.isoformat(),
            'analysis_role': 'prospective_observation_not_confirmatory_profit_test',
            'confirmation_unit': 'event_expiry_cohort; updates_and_shared_depth_are_not_independent_trials',
            'confirmation_requirement': 'Separate future expiries untouched by model changes; '
                                        'this single window is not economic confirmation.',
            'hypothesis': 'Positive costed three-leg conditional payoff bounds exist in this frozen cohort.',
            'selection_policy': qualification['selection_policy'],
            'cohort_frozen_before_rest_books': True, 'policy_frozen_before_stream': True,
            'no_adaptive_parameter_changes': True, 'max_episode_events': MAX_EPISODE_EVENTS,
            'soft_storage_limit_mib': max_mib, 'storage_check_seconds': 5,
            'storage_limit_can_overshoot': True, 'own_fills_or_profit_measured': False,
            'book_freshness': 'contiguous shared stream; quiet books retained; ages reported',
            'qualification_sha256': digest(qualification_path), 'policy_sha256': digest(policy_path),
            'metadata_sha256': digest(metadata_path), 'preparation_provenance_sha256': digest(preparation / 'provenance.json'),
            'native_engine_sha256': engine_hash, 'collector_sha256': binary_hash,
            'source_modules_sha256': initial_sources}
        write_json(root / 'observation-plan.json', plan)
        progress('Preparacion lista: ' + str(len(qualification['baskets'])) +
                 ' combinaciones, ' + str(len(qualification['markets'])) +
                 ' mercados. Seleccion fijada antes de observar los resultados.')
        frozen_paths = (qualification_path, metadata_path, policy_path,
                        preparation / 'provenance.json', root / 'observation-plan.json')
        frozen_hashes = {str(path): digest(path) for path in frozen_paths}

        def artifacts_stable():
            return all(digest(Path(path)) == expected for path, expected in frozen_hashes.items())

        if prepare_only:
            result.update(reason='prepare_only', usable=False, policy_prepared=True)
            write_json(root / 'result.json', result)
            return result
        if free_reader(root) < 3 * 1024**3:
            raise ObservationError('insufficient_free_space')
        check_window(qualification, clock(), seconds)
        if not stable():
            raise ObservationError('source_changed_before_capture')
        environment = os.environ.copy()
        environment.update(credential_loader(Path(settings)))
        check_window(qualification, clock(), seconds)
        if not stable() or not artifacts_stable():
            raise ObservationError('source_changed_before_capture')
        command = [str(binary.resolve()), str(metadata_path.resolve()), str((root / 'session').resolve()),
                   str(seconds), 'production', '--basket-observe', str(policy_path.resolve()), '--public-trades']
        # Never persist the environment, settings contents, private-key path or
        # collector stderr. Its small final JSON uses a file, avoiding PIPE stalls.
        with (root / 'collector-output.json').open('wb') as stdout:
            progress('Iniciando observacion y grabacion durante ' + str(seconds) +
                     ' segundos. Ninguna orden ni fill simulado; se miden episodios de margen.')
            progress('Limite de almacenamiento: ' + str(max_mib) +
                     ' MiB, con parada suave. Ctrl+C solicita guardar y finalizar.')
            capture_started_at = clock()
            child = process_factory(command, env=environment, stdout=stdout, stderr=subprocess.DEVNULL)
            try:
                result['execution'] = 'live_basket_observation_no_orders_sent'
                result['capture_started_at'] = capture_started_at.isoformat()
                write_json(root / 'result.json', result)
                stopped = monitor_runner(child, root, seconds, max_mib)
            except BaseException:
                child.terminate()
                try:
                    child.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    child.kill(); child.wait()
                raise
        result.update(exit_code=child.returncode, operator_stop=stopped,
                      capture_completed_at=clock().isoformat())
        collector = read_json(root / 'collector-output.json', maximum=1024 * 1024)
        summary = read_json(root / 'session/basket-summary.json')
        unchanged = stable() and artifacts_stable()
        policy_expired = summary.get('policy_expired') is True
        verified = (summary.get('live_replay_equal') is True and
                    summary.get('type') == 'basket_complete' and
                    summary.get('mode') == 'live_conditional_observation_no_orders' and
                    summary.get('incomplete') is False and
                    summary.get('event_budget_exceeded') is False and
                    type(summary.get('policy_expired')) is bool and
                    type(summary.get('orders_sent')) is int and summary['orders_sent'] == 0 and
                    summary.get('simulated_fills') is False)
        result.update(finalized=collector.get('finalized') is True,
                      market_updates=collector.get('market_updates', 0),
                      public_trades=collector.get('public_trades', 0),
                      connections=collector.get('connections', 0),
                      live_replay_equal=summary.get('live_replay_equal') is True,
                      observation_verified=verified, source_unchanged=unchanged,
                      policy_expired=policy_expired,
                      exit_code=child.returncode, operator_stop=stopped,
                      summary='session/basket-summary.json')
        result['usable'] = (result['finalized'] and type(result['market_updates']) is int and
                            result['market_updates'] > 0 and verified and unchanged and
                            not policy_expired and child.returncode == 0 and stopped is None)
        result['planned_window_complete'] = result['usable']
        result['reason'] = ('policy_window_expired_before_observation_finished' if policy_expired else
                            'observation_complete' if result['usable'] else 'observation_incomplete')
        if policy_expired:
            result['explanation'] = ('La vigencia de reglas y comisiones termino antes de finalizar la ventana; '
                'el motor dejo de evaluar oportunidades. Los datos registrados se conservan para analisis parcial.')
        result['artifact_hashes'] = {name: digest(root / name) for name in
            ('observation-plan.json', 'observation-policy.json', 'collector-output.json', 'session/basket-summary.json')}
        write_json(root / 'result.json', result)
        return result
    except Exception as error:
        reason = str(error) if isinstance(error, ObservationError) else type(error).__name__
        result.update(reason=reason, usable=False)
        write_json(root / 'result.json', result)
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--engine', type=Path, default=REPO / 'out/bin/event-engine')
    parser.add_argument('--binary', type=Path, default=REPO / 'out/bin/eme-capture')
    parser.add_argument('--settings', type=Path, default=SETTINGS)
    parser.add_argument('--output', type=Path, default=REPO / 'captures')
    parser.add_argument('--seconds', type=int, default=1800)
    parser.add_argument('--max-mib', type=int, default=256)
    parser.add_argument('--workers', type=int, default=4)
    parser.add_argument('--baskets', type=int, default=20)
    parser.add_argument('--markets', type=int, default=64)
    parser.add_argument('--cap', type=int, default=100)
    parser.add_argument('--window-label', default='prospective-window')
    parser.add_argument('--prepare-only', action='store_true')
    args = parser.parse_args()
    os.umask(0o077)
    root = args.output.resolve() / utcnow().strftime('basket-observe-%Y%m%dT%H%M%S.%fZ')
    print('Preparando observacion sin ordenes en: ' + str(root), flush=True)
    try:
        with PublicPool(workers=args.workers) as transport:
            result = observe(root, args.engine, args.binary, args.settings, transport,
                seconds=args.seconds, max_mib=args.max_mib, workers=args.workers,
                max_baskets=args.baskets, max_markets=args.markets, cap=args.cap,
                window_label=args.window_label, prepare_only=args.prepare_only,
                progress=lambda message: print(message, flush=True))
        print(json.dumps(result, indent=2))
        print('Directorio para analizar: ' + str(root))
        return 0 if result.get('usable') or result.get('reason') in ('prepare_only', 'no_qualified_observation_cohort') else 1
    except (ObservationError, PreparationError, OperatorError, OSError, ValueError,
            KeyError, TypeError, subprocess.TimeoutExpired):
        print('Observacion detenida; consulta result.json. Contenido de configuracion omitido.', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
