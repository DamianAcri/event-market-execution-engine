#!/usr/bin/env python3
"""Bounded market-data recording; no order endpoints or account operations.

Python 3.9+, curl, and the optional compiled eme-capture target are required.
Certified relationships are BTC thresholds; optional NFL coverage is observation only.
"""
import argparse
import datetime as dt
from decimal import Decimal
import hashlib
import itertools
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import time
from urllib.parse import urlencode

from research_selection import NFL_TERMS, select_research

REPO = Path(__file__).resolve().parents[1]
API = 'https://external-api.kalshi.com/trade-api/v2/'
TERMS = 'https://assets.kalshi.com/contract_terms/BTC.pdf'
TERMS_SHA256 = 'e7d857369971e75e9db14c5e2d91c29b94eb9a06e83e2acd9777991c4f2a0e2f'
SETTINGS = Path.home() / '.config/event-market-execution-engine/.env.local'
RULE = re.compile(
    r"If the simple average of the sixty seconds of CF Benchmarks' Bitcoin Real-Time Index "
    r"\(BRTI\) before (?P<time>[0-9]{1,2} (?:AM|PM) (?:EDT|EST)) is above "
    r"(?P<strike>[0-9]+(?:\.[0-9]+)?) at (?P=time) on "
    r"[A-Z][a-z]{2} [0-9]{1,2}, [0-9]{4}, then the market resolves to Yes\."
)


class OperatorError(Exception):
    """A fixed operator-facing message, never credential/parser input."""


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')


def public_get(url):
    result = subprocess.run(
        ['curl', '--fail', '--silent', '--show-error', '--max-time', '30', url],
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, check=False)
    if result.returncode or len(result.stdout) > 16 * 1024 * 1024:
        raise OperatorError('No se pudo obtener la fuente publica; no se ha iniciado la captura.')
    return result.stdout


def archive_markets(root, ticker, sources, max_pages=20):
    """Complete bounded public listing; repeated cursors/duplicates fail closed."""
    markets, cursor, seen_cursors, seen_tickers = [], '', set(), set()
    for page in range(max_pages):
        query = {'series_ticker': ticker, 'status': 'open', 'limit': 1000}
        if cursor:
            query['cursor'] = cursor
        url = API + 'markets?' + urlencode(query)
        filename = ticker + '-markets-page-' + str(page + 1) + '.json'
        content = public_get(url)
        (root / filename).write_bytes(content)
        sources[filename] = {'url': url, 'sha256': hashlib.sha256(content).hexdigest()}
        data = json.loads(content)
        for market in data['markets']:
            if market['ticker'] in seen_tickers:
                raise OperatorError('Listado cambiante o duplicado; repite la preparacion.')
            seen_tickers.add(market['ticker'])
            markets.append(market)
        cursor = data.get('cursor', '')
        if not cursor:
            return {'markets': markets, 'cursor': ''}
        if not isinstance(cursor, str) or cursor in seen_cursors:
            raise OperatorError('Paginacion publica repetida o no valida.')
        seen_cursors.add(cursor)
    raise OperatorError('La lista supera el limite de paginas; no se inicia una cobertura incompleta.')


def select_metadata(public, series, now, seconds, count=8):
    """Same event, same reviewed rule, different greater-than thresholds only."""
    if (public.get('cursor') or series.get('ticker') != 'KXBTCD' or
            series.get('contract_terms_url') != TERMS or
            series.get('fee_type') != 'quadratic' or series.get('fee_multiplier') != 1):
        raise OperatorError('La cobertura o las condiciones han cambiado; hace falta revisar la seleccion.')
    deadline = now + dt.timedelta(seconds=seconds + 600)
    groups = {}
    for market in public['markets']:
        closes = dt.datetime.fromisoformat(market['close_time'].replace('Z', '+00:00'))
        if (market['status'] == 'active' and market.get('strike_type') == 'greater'
                and closes > deadline):
            groups.setdefault(market['event_ticker'], []).append(market)
    events = sorted(groups, key=lambda event: (groups[event][0]['close_time'], event))
    event = next((event for event in events if len(groups[event]) >= count), None)
    if event is None:
        raise OperatorError('No hay suficientes mercados abiertos para esa duracion.')
    candidates = groups[event]
    candidates.sort(key=lambda market: (
        abs((Decimal(market['yes_bid_dollars']) + Decimal(market['yes_ask_dollars'])) / 2 - Decimal('.5')),
        market['ticker']))
    chosen = sorted(candidates[:count], key=lambda market: Decimal(str(market['floor_strike'])))
    templates = set()
    for market in chosen:
        match = RULE.fullmatch(market['rules_primary'])
        if (not match or Decimal(match['strike']) != Decimal(str(market['floor_strike'])) or
                market.get('cap_strike') is not None or market.get('functional_strike') or
                market.get('custom_strike') or market.get('is_provisional') or
                Decimal(market['notional_value_dollars']) != 1 or
                market.get('fee_waiver_expiration_time')):
            raise OperatorError('Un contrato no coincide con las reglas revisadas; captura cancelada.')
        templates.add(re.sub(r' is above [0-9.]+ at ', ' is above <STRIKE> at ', market['rules_primary']))
    for field in ('close_time', 'expiration_time', 'rules_secondary'):
        if len({market[field] for market in chosen}) != 1:
            raise OperatorError('Los contratos tienen condiciones distintas; captura cancelada.')
    if len(templates) != 1 or len({market['ticker'] for market in chosen}) != count or len({market['floor_strike'] for market in chosen}) != count:
        raise OperatorError('La relacion entre contratos no es valida; captura cancelada.')
    markets = [{'id': index + 1, 'ticker': market['ticker']} for index, market in enumerate(chosen)]
    constraints = []
    for low, high in itertools.combinations(markets, 2):
        constraints.append({
            'id': len(constraints) + 1, 'semantic_version': 1,
            'key': event.lower() + '.' + str(high['id']) + '-implies-' + str(low['id']),
            'provenance': 'Reviewed BTC threshold template 2026-09-16; same BRTI minute, event and rules. '
                          'Higher above-threshold implies lower; absent data resolves both NO. '
                          'Archived public-markets.json and BTC.pdf; no cross-event relationships.',
            'relationship': {'type': 'implication', 'antecedent': high['id'], 'consequent': low['id']}})
    metadata = {'schema_version': 1, 'metadata_version': int(now.strftime('%Y%m%d%H%M%S')),
                'venue': 'kalshi', 'markets': markets, 'constraints': constraints}
    return metadata, chosen


def paper_policy(metadata):
    """Frozen starting scenario, not fitted parameters or measured exchange fills."""
    certified = {value for constraint in metadata['constraints']
                 for key, value in constraint['relationship'].items() if key in ('antecedent', 'consequent', 'left', 'right')}
    return {
        'schema_version': 4, 'strategy': 'residual_exit_v4',
        'fee_provenance': 'General taker 0.07; selected series quadratic multiplier 1 is checked. '
                          'Cent-aligned account scenario; actual account tier not queried. '
                          'https://docs.kalshi.com/getting_started/fee_rounding',
        'capital_micro_usd': 1000000000, 'operating_cost_micro_usd': 0,
        'quantity_cap_centicontracts': 10000, 'quantity_step_centicontracts': 100,
        'min_margin_micro_usd': 0, 'max_book_age_ns': 10000000000,
        'leg_latency_ns': [100000000, 100000000], 'reject_legs': [False, False],
        'available_liquidity_bps': 10000, 'max_sizing_evaluations': 100000,
        'fees': [{'market_id': market['id'], 'coefficient_ppm': 70000,
                  'balance_quantum_micro': 10000} for market in metadata['markets'] if market['id'] in certified],
        'lifecycle': {'execution_policy': 'parallel_hold', 'first_leg': 0,
                      'response_latency_ns': [100000000, 100000000],
                      'completion_timeout_ns': 5000000000, 'completion_loss_limit_micro_usd': 0,
                      'maximum_completion_orders': 2, 'settlements': []},
        'residual_exit': {'mode': 'reduce_once', 'arrival_latency_ns': 100000000,
                          'response_latency_ns': 100000000, 'timeout_ns': 5000000000,
                          'minimum_price_1e4': 1, 'reject': False, 'available_liquidity_bps': 10000}}


def credentials(path):
    allowed = {'EME_KALSHI_KEY_ID', 'EME_KALSHI_PRIVATE_KEY_PATH'}
    values = {}
    for line in path.read_text(encoding='utf-8').splitlines():
        parts = shlex.split(line, comments=True)
        if parts and parts[0] == 'export':
            parts = parts[1:]
        if not parts:
            continue
        if len(parts) != 1 or '=' not in parts[0]:
            raise OperatorError('Formato de configuracion no valido; contenido omitido.')
        name, value = parts[0].split('=', 1)
        if name not in allowed or name in values:
            raise OperatorError('Campo de configuracion inesperado o repetido; contenido omitido.')
        values[name] = value
    if set(values) != allowed or not all(values.values()):
        raise OperatorError('Falta completar la configuracion local; contenido omitido.')
    if not re.fullmatch(r'[A-Za-z0-9-]{1,128}', values['EME_KALSHI_KEY_ID']):
        raise OperatorError('Identificador no valido; contenido omitido.')
    if not Path(values['EME_KALSHI_PRIVATE_KEY_PATH']).is_file():
        raise OperatorError('No existe el archivo de clave indicado; contenido omitido.')
    return values


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seconds', type=int, default=7200)
    parser.add_argument('--binary', type=Path, default=REPO / 'out/bin/eme-capture')
    parser.add_argument('--settings', type=Path, default=SETTINGS)
    parser.add_argument('--output', type=Path, default=REPO / 'captures')
    parser.add_argument('--paper', action='store_true', help='Live local IOC simulation plus recording; never submits orders')
    parser.add_argument('--profile', choices=['baseline', 'research'], default='baseline')
    parser.add_argument('--public-trades', action='store_true', help='Record public trades; implied by research profile')
    parser.add_argument('--btc-events', type=int, default=2)
    parser.add_argument('--btc-per-event', type=int, default=16)
    parser.add_argument('--nfl-markets', type=int, default=16)
    parser.add_argument('--max-mib', type=int, default=1024, help='Soft storage stop, checked every 5 seconds; can overshoot')
    parser.add_argument('--prepare-only', action='store_true', help='Public metadata only; no credential access or WS connection')
    args = parser.parse_args()
    if not 1 <= args.seconds <= 10800:
        raise OperatorError('La duracion debe estar entre 1 y 10800 segundos.')
    if not 32 <= args.max_mib <= 2048:
        raise OperatorError('El limite de almacenamiento debe estar entre 32 y 2048 MiB.')
    if args.profile == 'research' and (not 1 <= args.btc_events <= 4 or not 2 <= args.btc_per_event <= 32 or
            args.nfl_markets not in [0, *range(4, 33)] or args.btc_events * args.btc_per_event + args.nfl_markets > 64):
        raise OperatorError('Presupuesto de investigacion no valido; maximo 64 mercados.')
    args.public_trades = args.public_trades or args.profile == 'research'
    if not args.prepare_only and not args.binary.is_file():
        raise OperatorError('Falta el binario eme-capture; consulta READONLY_CAPTURE.md.')
    os.umask(0o077)
    now = dt.datetime.now(dt.timezone.utc)
    root = args.output.resolve() / now.strftime(args.profile + '-%Y%m%dT%H%M%S.%fZ')
    root.mkdir(parents=True, mode=0o700)
    print('Preparando captura en: ' + str(root), flush=True)
    sources = {}
    public = archive_markets(root, 'KXBTCD', sources)
    write_json(root / 'public-markets.json', public)
    for filename, url in [('public-series.json', API + 'series/KXBTCD'), ('BTC.pdf', TERMS)]:
        content = public_get(url)
        (root / filename).write_bytes(content)
        sources[filename] = {'url': url, 'sha256': hashlib.sha256(content).hexdigest()}
    if sources['BTC.pdf']['sha256'] != TERMS_SHA256:
        raise OperatorError('Ha cambiado el documento de reglas de BTC; hace falta revisarlo antes de capturar.')
    series = json.loads((root / 'public-series.json').read_text())['series']
    if args.profile == 'research':
        nfl = {}
        if args.nfl_markets:
            for ticker, (url, expected_hash) in NFL_TERMS.items():
                series_content = public_get(API + 'series/' + ticker)
                filename = ticker + '-series.json'
                (root / filename).write_bytes(series_content)
                sources[filename] = {'url': API + 'series/' + ticker, 'sha256': hashlib.sha256(series_content).hexdigest()}
                sport = json.loads(series_content)['series']
                if sport['ticker'] != ticker or sport['contract_terms_url'] != url:
                    raise OperatorError('Han cambiado las condiciones NFL; revisar antes de observar.')
                terms = public_get(url)
                filename = ticker + '-terms.pdf'
                (root / filename).write_bytes(terms)
                sources[filename] = {'url': url, 'sha256': hashlib.sha256(terms).hexdigest()}
                if sources[filename]['sha256'] != expected_hash:
                    raise OperatorError('Han cambiado las reglas NFL; revisar antes de observar.')
                nfl[ticker] = archive_markets(root, ticker, sources)
        selected_at = dt.datetime.now(dt.timezone.utc)
        metadata, selected, coverage = select_research(public, series, selected_at, args.seconds,
            select_metadata, OperatorError, args.btc_events, args.btc_per_event, args.nfl_markets, nfl)
    else:
        selected_at = dt.datetime.now(dt.timezone.utc)
        metadata, selected = select_metadata(public, series, selected_at, args.seconds)
        coverage = {'profile': 'baseline', 'selected_markets': len(selected),
                    'certified_relationships': len(metadata['constraints']), 'observation_only_markets': 0,
                    'selection_method': 'Eight thresholds nearest public midpoint 0.5 in the earliest eligible BTC event; frozen before capture. Not an optimal-market claim.'}
    write_json(root / 'coverage.json', coverage)
    write_json(root / 'metadata.json', metadata)
    write_json(root / 'selection.json', selected)
    if args.paper:
        write_json(root / 'paper-policy.json', paper_policy(metadata))
    write_json(root / 'provenance.json', {
        'preparation_started_at': now.isoformat(), 'selection_frozen_at': selected_at.isoformat(), 'duration_seconds': args.seconds, 'sources': sources,
        'selection': coverage['selection_method'], 'profile': args.profile,
        'public_trades': args.public_trades, 'soft_storage_limit_mib': args.max_mib,
        'selection_sha256': hashlib.sha256((root / 'selection.json').read_bytes()).hexdigest(),
        'coverage_sha256': hashlib.sha256((root / 'coverage.json').read_bytes()).hexdigest(),
        'selector_sha256': hashlib.sha256(Path(__file__).with_name('research_selection.py').read_bytes()).hexdigest(),
        'execution': 'live_paper_no_orders_sent' if args.paper else 'market_data_only',
        'policy_sha256': hashlib.sha256((root / 'paper-policy.json').read_bytes()).hexdigest() if args.paper else None, 'runner_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        'binary_sha256': hashlib.sha256(args.binary.read_bytes()).hexdigest() if args.binary.is_file() else None})
    print('Seleccion: ' + str(len(selected)) + ' mercados, ' + str(len(metadata['constraints'])) +
          ' relaciones BTC verificadas; ' + str(coverage['observation_only_markets']) + ' mercados solo de observacion.', flush=True)
    if args.public_trades:
        print('Se registran libros y operaciones publicas. Las operaciones publicas no son ejecuciones nuestras.', flush=True)
    if args.prepare_only:
        print('Metadatos preparados. No se ha abierto ninguna conexion autenticada.')
        return 0
    if shutil.disk_usage(root).free < 3 * 1024**3:
        raise OperatorError('Se necesitan al menos 3 GiB libres para empezar.')
    deadline = dt.datetime.now(dt.timezone.utc) + dt.timedelta(seconds=args.seconds + 600)
    if any(dt.datetime.fromisoformat(m['close_time'].replace('Z', '+00:00')) <= deadline for m in selected):
        raise OperatorError('La preparacion ha consumido el margen de cierre; repite la seleccion.')
    environment = os.environ.copy()
    environment.update(credentials(args.settings))
    command = [str(args.binary.resolve()), str(root / 'metadata.json'), str(root / 'session'), str(args.seconds), 'production']
    if args.paper:
        command.append(str(root / 'paper-policy.json'))
    if args.public_trades:
        command.append('--public-trades')
    print(('Grabando y simulando en vivo' if args.paper else 'Grabando solo datos') + ' durante ' + str(args.seconds) + ' segundos. Ctrl+C detiene y finaliza.', flush=True)
    if args.paper:
        print('Capital ficticio: 1000 USD; maximo 100 contratos por intento; retrasos supuestos de 100 ms. Ninguna orden se envia.', flush=True)
    child = subprocess.Popen(command, env=environment, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    started, stopped, last_progress = time.monotonic(), None, 0
    stop_time = None
    try:
        while child.poll() is None:
            try:
                child.wait(timeout=5)
            except subprocess.TimeoutExpired:
                elapsed = int(time.monotonic() - started)
                size = sum(path.stat().st_size for path in (root / 'session').glob('*') if path.is_file()) if (root / 'session').exists() else 0
                if stopped is None and (size > args.max_mib * 1024**2 or shutil.disk_usage(root).free < 1024**3):
                    stopped = 'storage_limit'
                    stop_time = time.monotonic()
                    child.terminate()
                if elapsed > args.seconds + 120 or (stop_time is not None and time.monotonic() - stop_time > 120):
                    stopped = 'capture_timeout'
                    child.kill()
                if elapsed - last_progress >= 60:
                    print(str(elapsed) + ' s; ' + str(size // (1024 * 1024)) + ' MiB guardados.', flush=True)
                    last_progress = elapsed
    except KeyboardInterrupt:
        stopped = 'user_stop'
        child.terminate()
        try:
            child.wait(timeout=120)
        except subprocess.TimeoutExpired:
            child.kill()
    child.wait()
    output = child.stdout.read()
    try:
        raw = json.loads(output)
        result = {key: raw[key] for key in ('finalized', 'market_updates', 'connections', 'reason')}
        result['public_trades'] = raw.get('public_trades', 0)
    except (ValueError, KeyError):
        result = {'finalized': False, 'reason': 'collector_failed'}
    if args.paper:
        summary = root / 'session/paper-summary.json'
        result['paper_verified'] = False
        if summary.is_file():
            paper = json.loads(summary.read_text())
            result['paper_verified'] = paper.get('live_replay_equal') is True
            result['simulated_attempts'] = paper['attempts']
            result['simulated_orders'] = paper['lifecycle']['orders']
            result['simulated_net_pnl_micro_usd'] = paper['lifecycle']['simulated_net_pnl_micro_usd']
            result['summary'] = str(summary)
    result.update({'exit_code': child.returncode, 'operator_stop': stopped, 'directory': str(root)})
    write_json(root / 'result.json', result)
    usable = result['finalized'] and result.get('market_updates', 0) > 0 and (not args.paper or result.get('paper_verified') is True)
    print(('Datos guardados para analizar: ' if usable else 'Captura incompleta; conservar para diagnostico: ') + str(root))
    print(json.dumps(result, indent=2))
    return 0 if usable and not child.returncode and stopped is None else 1


if __name__ == '__main__':
    try:
        sys.exit(main())
    except OperatorError as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
    except (OSError, KeyError, TypeError, ValueError, ArithmeticError):
        print('No se pudo preparar la captura; revisa los archivos locales. Contenido omitido.', file=sys.stderr)
        sys.exit(1)
