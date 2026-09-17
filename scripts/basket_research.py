"""Prepare and screen a frozen BTC basket cohort using public data only.

No account settings, environment secrets, order endpoints or authenticated
transport are used. This is a bounded REST preflight, not a continuous trading
simulation. The native screen prices a conditional payoff model, not own fills.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import datetime as dt
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time
from urllib.parse import urlencode

from basket_families import BTC, SERIES, FamilyError, qualify_baskets
from economic_selection import (SelectionError, fetch_books, millis,
                                paginated_event_fees, timestamp)
from market_catalog import CatalogClient, CatalogError, _market_page
from public_pool import PublicPool


class PreparationError(ValueError):
    pass


def utcnow():
    return dt.datetime.now(dt.timezone.utc)


def digest(path):
    value = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(chunk)
    return value.hexdigest()


def write_json(path, value):
    encoded = (json.dumps(value, sort_keys=True, indent=2, allow_nan=False) + '\n').encode()
    if len(encoded) > 32 * 1024 * 1024:
        raise PreparationError('research_artifact_size_limit')
    path.write_bytes(encoded)


def acquire_series(client, ticker):
    detail = client.get_json('series/' + ticker, 'series')
    if not isinstance(detail.get('series'), dict) or detail['series'].get('ticker') != ticker:
        raise PreparationError('series_identity_mismatch')
    markets, seen_tickers, seen_cursors, cursor = [], set(), set(), ''
    for page in range(9):
        query = {'series_ticker': ticker, 'status': 'open', 'mve_filter': 'exclude', 'limit': 1000}
        if cursor:
            query['cursor'] = cursor
        raw = client.get_json('markets?' + urlencode(query), 'markets-' + str(page))
        items, cursor = _market_page(raw, 1000, seen_tickers, seen_cursors)
        if any(not m['event_ticker'].startswith(ticker + '-') or
               not m['ticker'].startswith(m['event_ticker'] + '-') for m in items):
            raise PreparationError('series_market_identity_mismatch')
        markets.extend(items)
        if len(markets) > 8192:
            raise PreparationError('market_input_budget_exceeded')
        if not cursor:
            fees = client.get_json('series/fee_changes?' + urlencode({
                'series_ticker': ticker, 'show_historical': 'true'}), 'series-fees')
            return {'catalog': {'markets': markets, 'cursor': ''}, 'series': detail['series'], 'fees': fees}
    raise PreparationError('incomplete_series_page_budget')


def acquire_event(client, event):
    detail = client.get_json('events/' + event + '?with_nested_markets=false', 'event')
    if not isinstance(detail.get('event'), dict) or detail['event'].get('event_ticker') != event:
        raise PreparationError('event_identity_mismatch')
    return {'event': detail['event'], 'fees': paginated_event_fees(client, event, 'event-fees')}


def native_input(qualification, books, as_of_ms, *, cap=100, capital_micro=1_000_000_000,
                 max_age_ms=30000, max_skew_ms=2000):
    markets = [{'id': index + 1, 'ticker': market['ticker']}
               for index, market in enumerate(qualification['markets'])]
    identifiers = {m['ticker']: m['id'] for m in markets}
    baskets, fee_map = [], {}
    for index, basket in enumerate(qualification['baskets']):
        if (basket.get('model_classification') != 'conditional_common_scalar_observation_only' or
                basket.get('observation_only') is not True or basket.get('fee_verified') is not True):
            raise PreparationError('conditional_qualification_required')
        row = {'id': index + 1, 'key': basket['stablekey']}
        for role in ('lower', 'upper', 'range'):
            row[role + '_market_id'] = identifiers[basket[role + '_ticker']]
        for field in ('lower_threshold_cents', 'upper_threshold_cents', 'interval_lower_cents', 'interval_upper_cents'):
            row[field] = basket[field]
        for leg in basket['legs']:
            if leg['ticker'] in fee_map and fee_map[leg['ticker']] != leg['coefficient_ppm']:
                raise PreparationError('conflicting_market_fee')
            fee_map[leg['ticker']] = leg['coefficient_ppm']
        baskets.append(row)
    return {'schema_version': 1, 'markets': markets, 'baskets': baskets, 'books': books,
            'as_of_ms': as_of_ms, 'max_age_ms': max_age_ms, 'max_skew_ms': max_skew_ms,
            'max_total_evaluations': max(1, len(baskets) * cap),
            'sizing': {'cap_centicontracts': cap * 100, 'step_centicontracts': 100,
                       'available_cash_micro': capital_micro, 'minimum_margin_micro': 0,
                       'max_evaluations': cap},
            'fees': [{'market_id': m['id'], 'coefficient_ppm': fee_map[m['ticker']],
                      'balance_quantum_micro': 10000} for m in markets]}


def invoke_screen(engine, input_path, output_path):
    result = subprocess.run([str(Path(engine).resolve()), 'basket', 'screen', str(input_path)],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30, check=False)
    if result.returncode or len(result.stdout) > 32 * 1024 * 1024:
        raise PreparationError('native_basket_screen_failed')
    try:
        output = json.loads(result.stdout)
    except (ValueError, UnicodeError):
        raise PreparationError('invalid_native_screen_output') from None
    if (output.get('kind') != 'conditional_payoff_screen' or
            output.get('production_certificate') is not False or output.get('simulated_fills') is not False):
        raise PreparationError('unexpected_native_screen_mode')
    write_json(output_path, output)
    return output


def prepare(root, engine, transport, *, workers=4, max_baskets=20, max_markets=64,
            horizon_seconds=600, cap=100, clock=utcnow, client_factory=CatalogClient,
            screen_runner=invoke_screen):
    """One fresh preflight. Independent tasks have isolated archive ownership.

    A task returns its own CatalogClient and provenance. All cursor chains and
    each client are single-owner; the coordinator freezes the cohort before it
    requests any book or sees a costed result.
    """
    if (type(workers) is not int or not 1 <= workers <= 8 or
            type(max_baskets) is not int or not 1 <= max_baskets <= 100 or
            type(max_markets) is not int or not 3 <= max_markets <= 100 or
            type(cap) is not int or not 1 <= cap <= 100 or
            type(horizon_seconds) is not int or not 1 <= horizon_seconds <= 86400):
        raise PreparationError('invalid_preparation_limits')
    root = Path(root)
    if root.exists() and (root.is_symlink() or not root.is_dir() or any(root.iterdir())):
        raise PreparationError('output_directory_not_empty')
    root.mkdir(parents=True, exist_ok=True)
    began, started = clock(), time.monotonic()
    module_names = ('basket_research.py', 'basket_families.py', 'public_pool.py',
                    'market_catalog.py', 'market_families.py', 'economic_selection.py')
    code_hashes = {name: digest(Path(__file__).with_name(name)) for name in module_names}
    engine_hash = digest(engine)
    clients = []
    write_json(root / 'status.json', {'state': 'preparing', 'execution': 'public_data_only',
                                    'started_at': began.isoformat()})

    def task(name, callback, argument):
        client = client_factory(root / 'public' / name, public_get=transport, utcnow=clock)
        return client, callback(client, argument)

    def collect(executor, tasks):
        submitted = [(key, executor.submit(task, name, callback, argument))
                     for key, name, callback, argument in tasks]
        values, failure = {}, None
        for key, future in submitted:
            try:
                client, value = future.result()
                clients.append(client)
                values[key] = value
            except Exception as error:
                if failure is None:
                    failure = error
        if failure is not None:
            raise failure
        return values

    try:
        with ThreadPoolExecutor(max_workers=workers) as executor:
            series_results = collect(executor, [(ticker, 'series-' + ticker, acquire_series, ticker)
                                                for ticker in SERIES])
            catalogs = {ticker: item['catalog'] for ticker, item in series_results.items()}
            event_names = sorted({m['event_ticker'] for c in catalogs.values() for m in c['markets']})
            if len(event_names) > 128 or sum(len(c['markets']) for c in catalogs.values()) > 8192:
                raise PreparationError('catalog_input_budget_exceeded')
            event_results = collect(executor, [(event, 'event-' + str(i), acquire_event, event)
                                               for i, event in enumerate(event_names)])
        term_start = clock()
        response = transport(BTC['terms_url'])
        if response.status != 200 or not response.body.startswith(b'%PDF-'):
            raise PreparationError('btc_terms_download_failed')
        (root / 'BTC.pdf').write_bytes(response.body)
        term_hash = hashlib.sha256(response.body).hexdigest()
        now = clock()
        run_end = now + dt.timedelta(seconds=horizon_seconds)
        observed = min(timestamp(request['requested_at']) for c in clients for request in c.requests)
        qualification = qualify_baskets(
            catalogs, {t: value['series'] for t, value in series_results.items()},
            {event: value['event'] for event, value in event_results.items()},
            {ticker: term_hash for ticker in SERIES}, now, run_end,
            {t: value['fees'] for t, value in series_results.items()},
            {event: value['fees'] for event, value in event_results.items()},
            metadata_observed_at=observed, max_baskets=max_baskets, max_markets=max_markets)
        qualification['cohort_frozen_at'] = now.isoformat()
        qualification['fee_verified_from'] = began.isoformat()
        qualification['fee_verified_until'] = run_end.isoformat()
        # Preserve the preparation start in fee checks: a change which took
        # effect during acquisition must not evade the future-window check.
        for result in series_results.values():
            for change in result['fees'].get('series_fee_change_arr', []):
                if began < timestamp(change['scheduled_ts']) <= now:
                    raise PreparationError('fee_changed_during_preparation')
        for result in event_results.values():
            for change in result['fees'].get('event_fee_changes', []):
                if began < timestamp(change['scheduled_ts']) <= now:
                    raise PreparationError('fee_changed_during_preparation')
        write_json(root / 'qualification.json', qualification)
        # Market-only metadata is intentionally not a trade-policy certificate.
        params = native_input(qualification, [], int(now.timestamp() * 1000), cap=cap)
        metadata = {'schema_version': 1, 'metadata_version': int(now.strftime('%Y%m%d%H%M%S')),
                    'venue': 'kalshi', 'markets': params['markets'], 'constraints': []}
        write_json(root / 'observation-metadata.json', metadata)
        output = None
        if qualification['baskets']:
            book_client = client_factory(root / 'public' / 'books', public_get=transport, utcnow=clock)
            clients.append(book_client)
            books = fetch_books(book_client, metadata, 'frozen-cohort')
            as_of = clock()
            if as_of >= run_end or any(timestamp(m['close_time']) <= as_of for m in qualification['markets']):
                raise PreparationError('qualified_window_expired')
            params = native_input(qualification, books, int(as_of.timestamp() * 1000), cap=cap)
            write_json(root / 'screen-input.json', params)
            output = screen_runner(engine, root / 'screen-input.json', root / 'screen-output.json')
        if engine_hash != digest(engine) or code_hashes != {
                name: digest(Path(__file__).with_name(name)) for name in module_names}:
            raise PreparationError('code_changed_during_preparation')
        provenance = {'started_at': began.isoformat(), 'completed_at': clock().isoformat(),
            'elapsed_seconds': time.monotonic() - started, 'execution': 'public_data_only',
            'cohort_frozen_before_books': True, 'global_catalog_scan_required': False,
            'workers': workers, 'native_engine_sha256': engine_hash,
            'terms': {'url': BTC['terms_url'], 'sha256': term_hash,
                      'requested_at': term_start.isoformat()},
            'modules': code_hashes, 'code_unchanged_during_preparation': True,
            'archives': [{'path': str(c.root.relative_to(root)),
                          'manifest_sha256': digest(c.manifest_path), 'requests': len(c.requests),
                          'raw_bytes': c.raw_bytes, 'stored_bytes': c.stored_bytes} for c in clients],
            'transport_metrics': getattr(transport, 'metrics', None),
            'artifacts': {p.name: digest(p) for p in root.iterdir() if p.is_file() and p.name != 'status.json'}}
        write_json(root / 'provenance.json', provenance)
        result = {'state': 'complete', 'execution': 'public_data_only', 'model': qualification['model_classification'],
                  'counts': qualification['counts'], 'screen': None if output is None else {
                      k: output[k] for k in ('positive_quotes', 'solver_complete', 'evaluated_quantities', 'status_counts')},
                  'continuous_observation': False, 'simulated_orders': 0,
                  'elapsed_seconds': provenance['elapsed_seconds']}
        write_json(root / 'status.json', result)
        return result
    except Exception as error:
        code = str(error) if isinstance(error, (PreparationError, FamilyError, CatalogError, SelectionError)) else type(error).__name__
        write_json(root / 'status.json', {'state': 'failed', 'execution': 'public_data_only', 'error': code})
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True, help='New, empty research directory')
    parser.add_argument('--engine', type=Path, required=True, help='Built event-engine binary')
    parser.add_argument('--workers', type=int, default=4)
    parser.add_argument('--baskets', type=int, default=20)
    parser.add_argument('--markets', type=int, default=64)
    parser.add_argument('--cap', type=int, default=100, help='Maximum whole contracts per leg (1–100)')
    parser.add_argument('--horizon-seconds', type=int, default=600,
                        help='Fee/closing eligibility horizon; does not start a timed capture')
    args = parser.parse_args()
    if not args.engine.is_file():
        parser.error('--engine must be a built executable')
    try:
        with PublicPool(workers=args.workers) as transport:
            result = prepare(args.output, args.engine, transport, workers=args.workers,
                max_baskets=args.baskets, max_markets=args.markets, cap=args.cap,
                horizon_seconds=args.horizon_seconds)
        print(json.dumps(result, indent=2))
        print('Informe: ' + str(args.output.resolve() / 'status.json'))
        return 0
    except (PreparationError, FamilyError, CatalogError, SelectionError, OSError,
            ValueError, subprocess.TimeoutExpired) as error:
        print('Preparacion publica detenida: ' + str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
