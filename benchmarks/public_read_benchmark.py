#!/usr/bin/env python3
"""Compare archived public metadata GET plans; never read credentials or orders.

Network execution is explicit. Six requests are derived from an existing
successful basket-preparation archive, then repeated twice with serial curl,
one persistent worker and four persistent workers. This measures small-plan
preparation latency, not order latency, market coverage, or economic returns.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import datetime as dt
import hashlib
import json
from pathlib import Path
import platform
import statistics
import subprocess
import sys
import time
from urllib.parse import parse_qs, urlsplit

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / 'scripts'))
from market_catalog import API, CatalogClient, CatalogError, public_get
from public_pool import PublicPool


def sha(value):
    return hashlib.sha256(value).hexdigest()


def json_bytes(value):
    return (json.dumps(value, sort_keys=True, indent=2, allow_nan=False) + '\n').encode()


def save(path, value):
    Path(path).write_bytes(json_bytes(value))


def request_plan(source):
    """Only select six metadata endpoints already observed in the source run."""
    source = Path(source)
    status = json.loads((source / 'status.json').read_bytes())
    if status.get('state') != 'complete' or status.get('execution') != 'public_data_only':
        raise ValueError('source_preparation_not_complete')
    choices = {}
    for manifest in sorted((source / 'public').rglob('catalog-manifest.json')):
        raw = manifest.read_bytes()
        data = json.loads(raw)
        for request in data['requests']:
            url = request.get('url', '')
            if request.get('http_status') != 200 or not url.startswith(API):
                continue
            relative = url[len(API):]
            parsed = urlsplit(relative)
            query = parse_qs(parsed.query)
            category = None
            for series in ('KXBTC', 'KXBTCD'):
                if parsed.path == 'series/' + series and not query:
                    category = 'series-' + series
                elif (parsed.path == 'series/fee_changes' and query.get('series_ticker') == [series]
                      and query.get('show_historical') == ['true']):
                    category = 'fees-' + series
                elif (parsed.path.startswith('events/' + series + '-') and
                      query.get('with_nested_markets') == ['false']):
                    category = 'event-' + series
            if category is not None and category not in choices:
                choices[category] = {'name': category, 'relative': relative, 'url': url,
                                     'source_manifest': str(manifest.resolve()),
                                     'source_manifest_sha256': sha(raw)}
    order = ['series-KXBTC', 'series-KXBTCD', 'fees-KXBTC', 'fees-KXBTCD', 'event-KXBTC', 'event-KXBTCD']
    if any(name not in choices for name in order):
        raise ValueError('source_missing_required_six_request_plan')
    return [choices[name] for name in order]


def economic_fields(row, payload):
    """A conservative parity check, not a claim of atomic REST snapshots."""
    if row['name'].startswith('series-'):
        data = payload.get('series', {})
        if not isinstance(data, dict) or data.get('ticker') != row['name'][len('series-'):]:
            raise ValueError('unexpected_series_identity')
        fields = ('ticker', 'fee_type', 'fee_multiplier', 'settlement_sources', 'contract_terms_url')
    elif row['name'].startswith('event-'):
        data = payload.get('event', {})
        expected = urlsplit(row['relative']).path[len('events/'):]
        if not isinstance(data, dict) or data.get('event_ticker') != expected:
            raise ValueError('unexpected_event_identity')
        fields = ('event_ticker', 'series_ticker', 'mutually_exclusive', 'strike_date',
                  'settlement_sources', 'fee_type', 'fee_multiplier')
    else:
        if not isinstance(payload.get('series_fee_change_arr'), list):
            raise ValueError('incomplete_fee_schedule')
        return {'series_fee_change_arr': payload.get('series_fee_change_arr')}
    return {key: data.get(key) for key in fields}


class PacedCurl:
    """The existing curl transport with the same global minimum start spacing."""
    def __init__(self):
        self.next_start = 0.0
        self.metrics = []

    def __call__(self, url):
        PublicPool._target(url)
        time.sleep(max(0.0, self.next_start - time.monotonic()))
        start = time.monotonic()
        self.next_start = start + 0.2
        metric = {'url': url, 'transport_attempt': 1, 'reused_connection': False,
                  'started_monotonic': start}
        try:
            response = public_get(url)
            metric.update(http_status=response.status, response_bytes=len(response.body))
            return response
        finally:
            metric['elapsed_seconds'] = time.monotonic() - start
            self.metrics.append(metric)

    def close(self):
        pass


def run_mode(name, repetition, plan, root):
    mode_dir = root / ('repetition-{}-{}'.format(repetition, name))
    mode_dir.mkdir()
    workers = {'curl_serial': 1, 'pooled_1': 1, 'pooled_4': 4}[name]
    start = time.monotonic()
    transport = PacedCurl() if name == 'curl_serial' else PublicPool(workers=workers, interval=0.2)

    def fetch(index, row):
        # Separate archives give every worker single ownership of mutable
        # manifests; only the transport's global limiter is shared.
        client = CatalogClient(mode_dir / ('request-{:02d}'.format(index)), public_get=transport,
                               max_retries=0, max_archive_bytes=32 * 1024 * 1024,
                               max_decoded_bytes=32 * 1024 * 1024)
        payload = client.get_json(row['relative'], 'response')
        source = next(iter(client.sources.values()))
        return {'name': row['name'], 'response_sha256': source['sha256'],
                'semantic_sha256': sha(json_bytes(economic_fields(row, payload))),
                'decoded_bytes': client.raw_bytes, 'stored_bytes': client.stored_bytes,
                'archive_manifest': str(client.manifest_path.resolve()),
                'archive_manifest_sha256': sha(client.manifest_path.read_bytes()),
                'logical_elapsed_seconds': client.requests[-1]['elapsed_seconds']}

    try:
        if name == 'curl_serial':
            rows = [fetch(index, row) for index, row in enumerate(plan)]
        else:
            with ThreadPoolExecutor(max_workers=workers) as executor:
                futures = [executor.submit(fetch, index, row) for index, row in enumerate(plan)]
                rows = [future.result() for future in futures]
    finally:
        transport.close()
    elapsed = time.monotonic() - start
    metrics = list(transport.metrics)
    save(mode_dir / 'transport-metrics.json', metrics)
    return {'mode': name, 'repetition': repetition, 'workers': workers,
            'elapsed_seconds': elapsed, 'logical_requests': len(plan),
            'transport_attempts': len(metrics),
            'extra_transport_attempts': len(metrics) - len(plan),
            'reused_connection_attempts': sum(bool(m.get('reused_connection')) for m in metrics),
            'decoded_bytes': sum(row['decoded_bytes'] for row in rows),
            'stored_bytes': sum(row['stored_bytes'] for row in rows),
            'transport_completed_response_bytes': sum(m.get('response_bytes', 0) for m in metrics),
            'transport_decoded_bytes': getattr(transport, 'decoded_bytes', sum(m.get('response_bytes', 0) for m in metrics)),
            'extra_completed_response_bytes': sum(m.get('response_bytes', 0) for m in metrics) - sum(row['decoded_bytes'] for row in rows),
            'transport_metrics': str((mode_dir / 'transport-metrics.json').resolve()),
            'transport_metrics_sha256': sha((mode_dir / 'transport-metrics.json').read_bytes()),
            'responses': rows}


def run(source, archive, output):
    archive, output = Path(archive), Path(output)
    if archive.exists() or output.exists():
        raise ValueError('benchmark_output_already_exists')
    plan = request_plan(source)
    archive.mkdir(parents=True)
    output.parent.mkdir(parents=True, exist_ok=True)
    paths = [Path(__file__), REPO / 'scripts/market_catalog.py', REPO / 'scripts/public_pool.py']
    hashes = {str(path.relative_to(REPO)): sha(path.read_bytes()) for path in paths}
    save(archive / 'request-plan.json', plan)
    # Reversing order reduces one obvious warm-up/time confound. Two repeats
    # remain a descriptive check and cannot establish statistical superiority.
    orders = [('curl_serial', 'pooled_1', 'pooled_4'), ('pooled_4', 'pooled_1', 'curl_serial')]
    runs = []
    began = dt.datetime.now(dt.timezone.utc).isoformat()
    try:
        for repetition, order in enumerate(orders, 1):
            for mode in order:
                runs.append(run_mode(mode, repetition, plan, archive))
                save(archive / 'completed-runs.json', runs)
        if hashes != {str(path.relative_to(REPO)): sha(path.read_bytes()) for path in paths}:
            raise ValueError('benchmark_sources_changed_during_run')
        parity = {row['name']: {
            'distinct_response_hashes': len({response['response_sha256'] for run in runs
                                             for response in run['responses'] if response['name'] == row['name']}),
            'distinct_semantic_hashes': len({response['semantic_sha256'] for run in runs
                                             for response in run['responses'] if response['name'] == row['name']})}
            for row in plan}
        same_semantics = all(p['distinct_semantic_hashes'] == 1 for p in parity.values())
        summary = {mode: {'elapsed_seconds': [r['elapsed_seconds'] for r in runs if r['mode'] == mode],
                          'mean_seconds': statistics.mean(r['elapsed_seconds'] for r in runs if r['mode'] == mode)}
                   for mode in orders[0]}
        report = {'schema': 'public-read-benchmark-v1', 'state': 'complete',
                  'started_at': began, 'completed_at': dt.datetime.now(dt.timezone.utc).isoformat(),
                  'execution': 'public_get_only', 'logical_requests': 36,
                  'transport_attempts': sum(r['transport_attempts'] for r in runs),
                  'minimum_start_interval_seconds': 0.2, 'request_plan': plan,
                  'request_plan_sha256': sha((archive / 'request-plan.json').read_bytes()),
                  'source_hashes': hashes, 'python': sys.version.split()[0], 'machine': platform.machine(),
                  'curl': subprocess.run(['curl', '--version'], capture_output=True, text=True, check=True).stdout.splitlines()[0],
                  'timing_scope': 'transport creation, GETs, parsing, gzip archives, manifests, and transport shutdown',
                  'same_request_plan': True, 'semantic_fields_unchanged': same_semantics,
                  'response_parity': parity, 'summary': summary, 'runs': runs,
                  'limitations': ['Two repetitions only; changing internet/server load and DNS/TLS/cache state are uncontrolled.',
                                  'Only six metadata endpoints; not a complete catalog or order-book workload.',
                                  'Responses are live snapshots, not guaranteed identical or atomic.',
                                  'Semantic parity checks series/event identity, fees, sources and timing; raw hash differences remain reported.',
                                  'No trading, order latency, strategy performance or revenue is measured.']}
        save(output, report)
        return report
    except Exception as error:
        save(archive / 'failure.json', {'state': 'failed', 'error': type(error).__name__,
                                      'completed_runs': len(runs), 'started_at': began})
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True, help='Completed public basket-preparation directory')
    parser.add_argument('--archive', type=Path, required=True, help='New raw public benchmark archive directory')
    parser.add_argument('--output', type=Path, required=True, help='New compact benchmark JSON report')
    parser.add_argument('--network', action='store_true', help='Explicitly execute the 36 logical public GET requests')
    args = parser.parse_args()
    if not args.network:
        print(json.dumps(request_plan(args.source), indent=2))
        return 0
    try:
        report = run(args.source, args.archive, args.output)
    except (OSError, ValueError, CatalogError) as error:
        print('Public benchmark failed: ' + str(error), file=sys.stderr)
        return 1
    print(json.dumps({'summary': report['summary'], 'semantic_fields_unchanged': report['semantic_fields_unchanged'],
                      'transport_attempts': report['transport_attempts']}, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
