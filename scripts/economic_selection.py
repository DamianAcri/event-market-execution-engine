"""Broad discovery -> reviewed semantics -> native cost screen -> frozen watchlist.

Public REST observations rank data subscriptions, never submit orders or assert
fills. The lexicographic watchlist policy is explicit, not a calibrated forecast
of expected profit or a joint capital allocator.
"""
from collections import Counter
import datetime as dt
from decimal import Decimal, InvalidOperation
import hashlib
import gzip
import itertools
import json
from pathlib import Path
import re
import subprocess
from urllib.parse import urlencode

from market_catalog import CatalogClient, CatalogError, acquire_catalog
from market_families import REVIEWED_FAMILIES, FamilyError, qualify_catalog, verify_group_fees


class SelectionError(ValueError):
    pass


def utcnow():
    return dt.datetime.now(dt.timezone.utc)


def timestamp(value):
    if not isinstance(value, str):
        raise SelectionError('invalid_timestamp')
    # Python 3.9 accepts only 3/6 fractional digits; public RFC3339 timestamps
    # legitimately vary. Normalize to microseconds without rounding into future.
    value = re.sub(r'\.(\d{1,9})(?=Z$|[+-]\d{2}:\d{2}$)',
                   lambda m: '.' + (m[1] + '000000')[:6], value)
    try:
        parsed = dt.datetime.fromisoformat(value.replace('Z', '+00:00'))
    except ValueError:
        raise SelectionError('invalid_timestamp') from None
    if parsed.tzinfo is None:
        raise SelectionError('timestamp_without_timezone')
    return parsed


def millis(value):
    return int(timestamp(value).timestamp() * 1000)


def units(value, scale):
    if not isinstance(value, str):
        raise SelectionError('fixed_point_string_required')
    try:
        number = Decimal(value) * scale
        if not number.is_finite() or number != number.to_integral_value() or number < 0:
            raise SelectionError('invalid_fixed_point')
        return int(number)
    except InvalidOperation:
        raise SelectionError('invalid_fixed_point') from None


def write_json(path, value):
    content = (json.dumps(value, indent=2, sort_keys=True) + '\n').encode('utf-8')
    stored = sum(p.stat().st_size for p in path.parent.iterdir() if p.is_file() and p != path)
    if stored + len(content) > 256 * 1024 * 1024:
        raise SelectionError('preparation_artifact_budget_exceeded')
    path.write_bytes(content)


def market_series(market, series):
    # Series tickers can contain hyphens; find an exact catalogue prefix.
    event = market['event_ticker']
    while '-' in event:
        event = event.rsplit('-', 1)[0]
        if event in series:
            return event
    return '__unknown_series__'


def public_activity(market):
    try:
        return units(market.get('volume_24h_fp', '0.00'), 100)
    except SelectionError:
        return 0  # Only a tie-breaker; never invent liquidity or an execution.


def metadata_for(groups, tickers, version):
    markets, constraints, fee_map, originals = [], [], {}, {}
    for group in sorted(groups, key=lambda g: (g['series_ticker'], g['event_ticker'])):
        if not group['fee_verified']:
            raise SelectionError('fee_verification_required')
        members = []
        for market in group['markets']:
            ticker = market['ticker']
            if tickers is not None and ticker not in tickers:
                continue
            if ticker in originals:
                raise SelectionError('duplicate_qualified_market')
            entry = {'id': len(markets) + 1, 'ticker': ticker}
            markets.append(entry); members.append(entry); originals[ticker] = market
            fee_map[ticker] = group['coefficient_ppm']
        for low, high in itertools.combinations(members, 2):
            constraints.append({'id': len(constraints) + 1, 'semantic_version': 1,
                'key': group['event_ticker'].lower() + '.' + str(high['id']) + '-implies-' + str(low['id']),
                'provenance': 'Reviewed ' + group['family'] + '; terms SHA256 ' + group['terms_sha256'] +
                              '; common-rule SHA256 ' + group['rule_fingerprint'] + '. Within-event thresholds only.',
                'relationship': {'type': 'implication', 'antecedent': high['id'], 'consequent': low['id']}})
            if len(constraints) > 200000:
                raise SelectionError('relationship_budget_exceeded')
    return ({'schema_version': 1, 'metadata_version': version, 'venue': 'kalshi',
             'markets': markets, 'constraints': constraints}, originals, fee_map)


def book_pool(groups, budget):
    """Only used if qualified universe exceeds acquisition budget; report exclusions.

    Priority is indicative gross quote proximity, then both legs' past-day volume.
    This catalogue prefilter does not estimate fees, depth, or expected profit.
    """
    all_tickers = {m['ticker'] for g in groups for m in g['markets']}
    if len(all_tickers) <= budget:
        return all_tickers
    pairs = []
    for group in groups:
        for low, high in itertools.combinations(group['markets'], 2):
            if len(pairs) >= 200000:
                raise SelectionError('relationship_budget_exceeded')
            try:
                low_ask, high_ask = units(low['yes_ask_dollars'], 10000), units(high['no_ask_dollars'], 10000)
                if not 0 < low_ask <= 10000 or not 0 < high_ask <= 10000:
                    raise SelectionError('catalog_quote_unavailable')
                gross = 10000 - low_ask - high_ask
            except (KeyError, SelectionError):
                gross = -20000
            pairs.append((-gross, -min(public_activity(low), public_activity(high)),
                          low['ticker'], high['ticker']))
    chosen = set()
    for _, _, a, b in sorted(pairs):
        if len(chosen | {a, b}) <= budget:
            chosen.update((a, b))
    return chosen


def fetch_books(client, metadata, name):
    books = []
    for offset in range(0, len(metadata['markets']), 100):
        batch = metadata['markets'][offset:offset + 100]
        query = urlencode([('tickers', m['ticker']) for m in batch])
        payload = client.get_json('markets/orderbooks?' + query, name + '-' + str(offset // 100))
        request = client.requests[-1]
        expected = {m['ticker']: m['id'] for m in batch}
        seen = set()
        if not isinstance(payload.get('orderbooks'), list):
            raise SelectionError('invalid_orderbooks_response')
        for row in payload['orderbooks']:
            if not isinstance(row, dict) or not isinstance(row.get('orderbook_fp'), dict):
                raise SelectionError('invalid_orderbook_side')
            ticker = row.get('ticker')
            if ticker not in expected or ticker in seen:
                raise SelectionError('unexpected_or_duplicate_book')
            seen.add(ticker)
            normalized = {'market_id': expected[ticker], 'request_time_ms': millis(request['requested_at']),
                          'received_time_ms': millis(request['completed_at'])}
            for public_side, native_side in [('yes_dollars', 'yes_bids'), ('no_dollars', 'no_bids')]:
                if public_side not in row['orderbook_fp']:
                    raise SelectionError('invalid_orderbook_side')
                raw = row['orderbook_fp'].get(public_side)
                # REST may encode an empty side as null. Nonempty levels stay exact.
                if raw is None:
                    raw = []
                if not isinstance(raw, list):
                    raise SelectionError('invalid_orderbook_side')
                levels = []
                for level in raw:
                    if not isinstance(level, list) or len(level) != 2:
                        raise SelectionError('invalid_orderbook_level')
                    price, quantity = units(level[0], 10000), units(level[1], 100)
                    if price > 10000 or quantity <= 0:
                        raise SelectionError('invalid_orderbook_level')
                    levels.append([price, quantity])
                levels.sort()
                if len({p for p, _ in levels}) != len(levels):
                    raise SelectionError('duplicate_book_price')
                normalized[native_side] = levels
            books.append(normalized)
        if seen != set(expected):
            raise SelectionError('missing_requested_book')
    return books


def native_screen(root, engine, metadata, fees, books, name, as_of_ms,
                  max_age_ms=30000, max_skew_ms=2000, total_evaluations=1000000):
    """Batch only constraint metadata; retain all books and original IDs per call."""
    rows, complete, evaluated = [], True, 0
    for offset in range(0, len(metadata['constraints']), 2048):
        constraints = metadata['constraints'][offset:offset + 2048]
        if evaluated >= total_evaluations:
            complete = False
            rows.extend({'constraint_id': c['id'], 'status': 'screening_budget_exceeded',
                         'quote': None, 'one_contract_diagnostic': None} for c in constraints)
            continue
        needed = {c['relationship'][role] for c in constraints for role in ('antecedent', 'consequent')}
        batch_markets = [m for m in metadata['markets'] if m['id'] in needed]
        batch = dict(metadata, markets=batch_markets, constraints=constraints)
        params = {'schema_version': 1, 'as_of_ms': as_of_ms, 'max_age_ms': max_age_ms,
                  'max_skew_ms': max_skew_ms, 'max_total_evaluations': total_evaluations - evaluated,
                  'sizing': {'cap_centicontracts': 10000, 'step_centicontracts': 100,
                             'available_cash_micro': 1000000000, 'minimum_margin_micro': 0,
                             'max_evaluations': 100000},
                  'fees': [{'market_id': m['id'], 'coefficient_ppm': fees[m['ticker']],
                            'balance_quantum_micro': 10000} for m in batch_markets],
                  'books': [book for book in books if book['market_id'] in needed]}
        stem = name + '-' + str(offset // 2048)
        meta_path, input_path = root / (stem + '-metadata.json'), root / (stem + '-input.json')
        write_json(meta_path, batch); write_json(input_path, params)
        process = subprocess.run([str(engine), 'market', 'screen', str(meta_path), str(input_path)],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60, check=False)
        if process.returncode:
            raise SelectionError('native_market_screen_failed')
        result = json.loads(process.stdout)
        if result.get('kind') != 'indicative_rest_screen':
            raise SelectionError('invalid_native_screen_result')
        write_json(root / (stem + '-output.json'), result)
        if (len(result['constraints']) != len(constraints) or
                {r['constraint_id'] for r in result['constraints']} != {c['id'] for c in constraints}):
            raise SelectionError('native_screen_constraint_mismatch')
        rows.extend(result['constraints']); evaluated += result['evaluated_quantities']
        complete = complete and result['solver_complete']
    return {'rows': rows, 'solver_complete': complete, 'evaluated_quantities': evaluated}


def candidates(metadata, originals, results):
    by_id = {m['id']: m['ticker'] for m in metadata['markets']}
    relations = {c['id']: c['relationship'] for c in metadata['constraints']}
    ranked = []
    for row in results['rows']:
        quote, diagnostic = row.get('quote'), row.get('one_contract_diagnostic')
        if row['status'] not in ('optimal', 'no_positive_margin'):
            continue
        if not quote and (not diagnostic or not diagnostic['funded']):
            continue
        relation = relations[row['constraint_id']]
        legs = [by_id[relation[k]] for k in ('consequent', 'antecedent')]
        margin = quote['net_margin_micro'] if quote else diagnostic['net_margin_micro']
        ranked.append({'tickers': legs, 'constraint_id': row['constraint_id'],
                       'selection_class': 'positive_indicative_margin' if quote else 'active_near_margin',
                       'net_margin_micro': margin, 'quote': quote, 'one_contract_diagnostic': diagnostic,
                       'minimum_volume_24h_centicontracts': min(public_activity(originals[t]) for t in legs),
                       'close_time': min(originals[t]['close_time'] for t in legs)})
    return sorted(ranked, key=lambda r: (r['quote'] is None, -r['net_margin_micro'],
                                        -r['minimum_volume_24h_centicontracts'], r['close_time'], r['tickers']))


def pair_budget(ranked, budget):
    chosen = set()
    for candidate in ranked:
        pair = set(candidate['tickers'])
        if len(chosen | pair) <= budget:
            chosen.update(pair)
    return chosen


def fetch_activity(client, tickers, as_of, name='activity', lookback_seconds=900):
    """Bounded recent non-block trade presence; capped counts are lower bounds."""
    result = {}
    end = int(as_of.timestamp()); start = end - lookback_seconds
    for index, ticker in enumerate(sorted(tickers)):
        query = urlencode({'ticker': ticker, 'min_ts': start, 'max_ts': end,
                           'is_block_trade': 'false', 'limit': 1000})
        data = client.get_json('markets/trades?' + query, name + '-' + str(index))
        if not isinstance(data.get('trades'), list) or not isinstance(data.get('cursor'), str):
            raise SelectionError('invalid_trade_activity_response')
        seen, count, volume, unknown = set(), 0, 0, 0
        latest = None
        for trade in data['trades']:
            if trade.get('ticker') != ticker or not isinstance(trade.get('trade_id'), str) or not trade['trade_id']:
                raise SelectionError('trade_activity_identity_mismatch')
            if trade['trade_id'] in seen:
                raise SelectionError('duplicate_activity_trade')
            seen.add(trade['trade_id'])
            when = timestamp(trade['created_time'])
            if not start <= when.timestamp() <= end + 1:
                raise SelectionError('trade_activity_time_mismatch')
            if trade.get('is_block_trade') is not False:
                unknown += 1
                continue
            quantity = units(trade['count_fp'], 100)
            if quantity <= 0:
                raise SelectionError('invalid_trade_activity_quantity')
            count += 1; volume += quantity
            latest = max(latest, when) if latest else when
        result[ticker] = {'recent_nonblock_trades': count, 'recent_volume_centicontracts': volume,
                          'latest_trade_at': latest.isoformat() if latest else None,
                          'window_start': start, 'window_end': end, 'window_seconds': lookback_seconds,
                          'complete': not data['cursor'], 'count_is_lower_bound': bool(data['cursor']),
                          'unknown_or_block_excluded': unknown}
    return result


def choose_watchlist(ranked, activity, budget):
    chosen, accepted, declined = set(), [], []
    for candidate in ranked:
        pair = set(candidate['tickers'])
        missing = pair - set(activity)
        if missing:
            declined.append(dict(candidate, exclusion='activity_budget'))
            continue
        # An indicative positive quote can justify observing a quiet pair; it is
        # still never an execution. Otherwise both books need recent traded flow.
        if not candidate['quote'] and any(activity[t]['recent_nonblock_trades'] == 0 for t in pair):
            declined.append(dict(candidate, exclusion='no_recent_two_leg_trade_activity'))
            continue
        if len(chosen | pair) > budget:
            declined.append(dict(candidate, exclusion='subscription_budget'))
            continue
        chosen.update(pair); accepted.append(candidate)
    return chosen, accepted, declined


def paginated_event_fees(client, event, name):
    rows, seen, cursor = [], set(), ''
    for page in range(20):
        query = {'event_ticker': event, 'limit': 1000}
        if cursor:
            query['cursor'] = cursor
        payload = client.get_json('events/fee_changes?' + urlencode(query), name + '-' + str(page))
        if not isinstance(payload.get('event_fee_changes'), list) or not isinstance(payload.get('cursor'), str):
            raise SelectionError('invalid_event_fee_schedule')
        rows.extend(payload['event_fee_changes']); cursor = payload['cursor']
        if not cursor:
            return {'event_fee_changes': rows, 'cursor': ''}
        if cursor in seen:
            raise SelectionError('event_fee_cursor_loop')
        seen.add(cursor)
    raise SelectionError('event_fee_page_budget')


def refresh_reviewed_catalog(client, grouped, series):
    """Broad scans take minutes; refresh supported universes before qualification."""
    for ticker in REVIEWED_FAMILIES:
        detail = client.get_json('series/' + ticker, 'fresh-series-' + ticker)
        if not isinstance(detail.get('series'), dict) or detail['series'].get('ticker') != ticker:
            raise SelectionError('series_refresh_identity_mismatch')
        series[ticker] = detail['series']
        markets, seen, cursors, cursor = [], set(), set(), ''
        for page in range(20):
            query = {'series_ticker': ticker, 'status': 'open', 'mve_filter': 'exclude', 'limit': 1000}
            if cursor:
                query['cursor'] = cursor
            data = client.get_json('markets?' + urlencode(query), 'fresh-markets-' + ticker + '-' + str(page))
            if not isinstance(data.get('markets'), list) or not isinstance(data.get('cursor'), str):
                raise SelectionError('invalid_series_market_refresh')
            for market in data['markets']:
                name = market.get('ticker')
                if name in seen or market_series(market, series) != ticker:
                    raise SelectionError('series_market_refresh_identity_mismatch')
                seen.add(name); markets.append(market)
            cursor = data['cursor']
            if not cursor:
                grouped[ticker] = {'markets': markets, 'cursor': ''}
                break
            if cursor in cursors:
                raise SelectionError('series_market_refresh_cursor_loop')
            cursors.add(cursor)
        else:
            raise SelectionError('series_market_refresh_page_budget')


def research_frontier(grouped, series):
    """Activity ranks future rule reviews, never certifies trading eligibility."""
    rows = [{'series_ticker': ticker, 'title': series.get(ticker, {}).get('title'),
             'open_markets': len(data['markets']),
             'volume_24h_centicontracts': sum(public_activity(m) for m in data['markets']),
             'reason': 'unreviewed_family'}
            for ticker, data in grouped.items() if ticker not in REVIEWED_FAMILIES]
    return sorted(rows, key=lambda r: (-r['volume_24h_centicontracts'], r['series_ticker']))


def prepare_economic(root, seconds, engine, get_terms, *, market_budget=64,
                     book_budget=1024, activity_budget=128, max_pages=500, client=None,
                     catalog=None, clock=utcnow):
    root = Path(root)
    if not 2 <= market_budget <= 64 or not market_budget <= activity_budget <= 512 or not activity_budget <= book_budget <= 2048:
        raise SelectionError('invalid_selection_budgets')
    client = client or CatalogClient(root / 'public')
    catalog = catalog or acquire_catalog(client.root, client=client, max_market_pages=max_pages,
                                         compact_markets=True)
    if not catalog['coverage']['complete']:
        raise SelectionError('complete_catalog_required')
    series = {s['ticker']: s for s in catalog['series']}
    grouped, hashes, extra_sources = {}, {}, {}
    for market in catalog['markets']:
        grouped.setdefault(market_series(market, series), {'markets': [], 'cursor': ''})['markets'].append(market)
    refresh_reviewed_catalog(client, grouped, series)
    frontier = research_frontier(grouped, series)
    write_json(root / 'research-frontier.json', frontier)
    for ticker, family in REVIEWED_FAMILIES.items():
        if ticker not in grouped:
            continue
        raw = get_terms(family['terms_url'])
        filename = ticker + '-terms.pdf'; (root / filename).write_bytes(raw)
        hashes[ticker] = hashlib.sha256(raw).hexdigest()
        extra_sources[filename] = {'url': family['terms_url'], 'sha256': hashes[ticker]}
    now = clock(); run_end = now + dt.timedelta(seconds=seconds + 600)
    qualified = qualify_catalog(grouped, series, hashes, run_end, now)
    groups, fee_exclusions, schedules, fee_contexts = [], [], {}, {}
    for index, group in enumerate(qualified['groups']):
        ticker, event = group['series_ticker'], group['event_ticker']
        if ticker not in schedules:
            schedules[ticker] = client.get_json('series/fee_changes?' + urlencode({'series_ticker': ticker, 'show_historical': 'false'}), 'series-fees-' + ticker)
        detail = client.get_json('events/' + event, 'event-' + str(index))
        event_fees = paginated_event_fees(client, event, 'event-fees-' + str(index))
        fee_contexts[event] = (detail['event'], event_fees)
        try:
            groups.append(verify_group_fees(group, series[ticker], detail['event'], now, run_end, schedules[ticker], event_fees))
        except FamilyError as error:
            fee_exclusions.append({'event_ticker': event, 'reason': str(error)})
    qualification = {'semantics': qualified, 'fee_exclusions': fee_exclusions, 'fee_verified_groups': groups}
    (root / 'qualification.json.gz').write_bytes(gzip.compress(json.dumps(qualification, sort_keys=True).encode(), mtime=0))
    pool = book_pool(groups, book_budget)
    version = int(now.strftime('%Y%m%d%H%M%S'))
    metadata, originals, fees = metadata_for(groups, pool, version)
    books = fetch_books(client, metadata, 'initial-books')
    first = native_screen(root, engine, metadata, fees, books, 'initial', int(clock().timestamp() * 1000))
    ranked = candidates(metadata, originals, first)
    activity_pool = pair_budget(ranked, activity_budget)
    activity = fetch_activity(client, activity_pool, clock())
    write_json(root / 'activity.json', activity)
    provisional, _, initial_declined = choose_watchlist(ranked, activity, activity_budget)
    write_json(root / 'initial-selection-input.json', {'ranked_pairs': ranked, 'activity': activity,
                                                     'activity_budget': activity_budget, 'declined': initial_declined})
    final_meta, final_originals, final_fees = metadata_for(groups, provisional, version)
    final_results = {'rows': [], 'solver_complete': True, 'evaluated_quantities': 0}
    if final_meta['constraints']:
        final_books = fetch_books(client, final_meta, 'refreshed-books')
        final_results = native_screen(root, engine, final_meta, final_fees, final_books, 'refreshed', int(clock().timestamp() * 1000))
    final_ranked = candidates(final_meta, final_originals, final_results)
    chosen, accepted, declined = choose_watchlist(final_ranked, activity, market_budget)
    selected_meta, selected_originals, selected_fees = metadata_for(groups, chosen, version)
    freeze = clock()
    verified_until = freeze + dt.timedelta(seconds=seconds + 600)
    if any(timestamp(m['close_time']) <= verified_until for m in selected_originals.values()):
        raise SelectionError('selection_window_expired_during_preparation')
    # Recheck the full preparation-to-run horizon: a scheduled change that has
    # become effective during preparation must not escape a check starting now.
    for group in groups:
        if not any(m['ticker'] in chosen for m in group['markets']):
            continue
        event, event_fees = fee_contexts[group['event_ticker']]
        try:
            verify_group_fees(group, series[group['series_ticker']], event, now, verified_until,
                              schedules[group['series_ticker']], event_fees)
        except FamilyError:
            raise SelectionError('fee_window_changed_during_preparation') from None
    reason_counts = Counter(r['reason'] for r in qualified['exclusions'])
    report = {'schema_version': 1, 'profile': 'economic_v1', 'selection_frozen_at': freeze.isoformat(),
        'selection_method': 'Fee/depth-aware native IOC sizing, positive indicative margins first; otherwise nearest one-contract net margin with recent trades on both legs; deterministic greedy pair-union subscription budget. Not expected profit or joint capital allocation.',
        'catalog': catalog['coverage'], 'supported_families': sorted(REVIEWED_FAMILIES),
        'reviewed_catalogs_refreshed': True, 'fee_verified_until': verified_until.isoformat(),
        'unsupported_research_frontier_top10': frontier[:10],
        'qualification_file': 'qualification.json.gz',
        'preparation_artifact_budget_mib': 256,
        'qualified_groups': len(groups), 'qualified_markets': sum(len(g['markets']) for g in groups),
        'semantic_exclusion_counts': dict(sorted(reason_counts.items())), 'fee_exclusions': fee_exclusions,
        'book_budget': book_budget, 'books_examined': len(books),
        'excluded_by_book_budget': sum(len(g['markets']) for g in groups) - len(pool),
        'activity_budget': activity_budget, 'activity_markets_examined': len(activity),
        'initial_pair_exclusion_counts': dict(Counter(r['exclusion'] for r in initial_declined)),
        'market_budget': market_budget, 'selected_markets': len(chosen),
        'certified_relationships': len(selected_meta['constraints']), 'observation_only_markets': 0,
        'initial_screen_statuses': dict(Counter(r['status'] for r in first['rows'])),
        'refreshed_screen_statuses': dict(Counter(r['status'] for r in final_results['rows'])),
        'initial_solver_complete': first['solver_complete'], 'refreshed_solver_complete': final_results['solver_complete'],
        'positive_indicative_pairs_selected': sum(c['quote'] is not None for c in accepted),
        'decision': 'capture_active_watchlist' if chosen else 'do_not_start_no_qualified_active_pairs',
        'scope_limit': 'Only reviewed BTC/ETH threshold families are executable. Other families stay in qualification exclusions. REST snapshots are not synchronous fills; watchlist is frozen for the run and budgets limit discovery depth.',
        'fees_by_ticker': selected_fees, 'selected_pair_reasons': accepted,
        'pair_exclusion_counts': dict(Counter(r['exclusion'] for r in declined))}
    write_json(root / 'selection-input.json', {'ranked_pairs': final_ranked, 'activity': activity, 'market_budget': market_budget})
    write_json(root / 'selection-decision.json', report)
    client.save()
    sources = dict(extra_sources)
    sources.update({'public/' + filename: value for filename, value in client.sources.items()})
    return selected_meta, [selected_originals[m['ticker']] for m in selected_meta['markets']], report, sources
