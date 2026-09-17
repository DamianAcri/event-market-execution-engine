"""Bounded BTC range/threshold qualification for observation, never execution.

The conditional model is A=1[x>a], B=1[x>b], I=1[l<=x<=u]. With
a<l<=u<=b, YES(A)+NO(B)+NO(I)>=2, also when all YES settle NO.
General exchange review/modification powers are outside this model. No input
can upgrade its observation-only classification to unconditional arbitrage.
All HTTP and raw-byte archival belong to the caller; this module is pure.
"""
from bisect import bisect_left
from collections import Counter, defaultdict, deque
import datetime as dt
import hashlib
import json
import re

from market_families import (FamilyError, REVIEWED_FAMILIES, REVIEWED_SECONDARY,
                             _decimal, _market_semantics, _source_matches, _time,
                             resolve_taker_fee)

BTC = REVIEWED_FAMILIES['KXBTCD']
SERIES = ('KXBTC', 'KXBTCD')
MODEL_CLASSIFICATION = 'conditional_common_scalar_observation_only'
MAX_INPUT_MARKETS = 8192
MAX_INPUT_EVENTS = 128
_RANGE_RULE = re.compile(
    r"If the simple average of the sixty seconds of CF Benchmarks' Bitcoin "
    r'Real-Time Index \(BRTI\) before '
    r'(?P<time>(?P<hour>[0-9]{1,2}) (?P<meridiem>AM|PM) (?P<zone>EDT|EST)) '
    r'is between (?P<lower>[0-9]+(?:\.[0-9]+)?)-(?P<upper>[0-9]+(?:\.[0-9]+)?) '
    r'at (?P=time) on (?P<month>[A-Z][a-z]{2}) (?P<day>[0-9]{1,2}), '
    r'(?P<year>[0-9]{4}), then the market resolves to Yes\.')


def _digest(value):
    """Canonical parsed-source hash; distinct from caller's raw response hash."""
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':'),
                                     allow_nan=False).encode()).hexdigest()


def _cents(value):
    number = _decimal(value)
    cents = number * 100
    if not 0 <= number <= 100000000 or cents != cents.to_integral_value():
        raise FamilyError('unsupported_strike_precision')
    return int(cents)


def _common_semantics(market):
    # Equality is temporal, not an accident of spelling Z versus +00:00.
    times = tuple(None if market.get(k) is None else _time(market[k]).isoformat()
                  for k in ('close_time', 'expiration_time', 'expected_expiration_time',
                            'latest_expiration_time', 'occurrence_datetime'))
    return (times, market['rules_secondary'], market.get('settlement_timer_seconds'),
            market.get('can_close_early'), BTC['terms_sha256'], BTC['source_url'])


def _semantics(market, ticker, now, run_end):
    if ticker == 'KXBTCD':
        strike, _ = _market_semantics(market, ticker, run_end, now)
        return _cents(strike), None, _common_semantics(market)
    # Reuse the reviewed threshold validation for all shared contract fields.
    # Transformation is only allowed after an exact range grammar match; the
    # original range bounds/rules remain in the output and provenance hashes.
    if market.get('strike_type') != 'between':
        raise FamilyError('unsupported_range_payoff')
    primary = market.get('rules_primary')
    match = _RANGE_RULE.fullmatch(primary) if isinstance(primary, str) else None
    if match is None or market.get('rules_secondary') != REVIEWED_SECONDARY:
        raise FamilyError('unreviewed_rule_text')
    lower, upper = _cents(market.get('floor_strike')), _cents(market.get('cap_strike'))
    if (lower != _cents(match['lower']) or upper != _cents(match['upper']) or lower > upper):
        raise FamilyError('range_rule_mismatch')
    event = market.get('event_ticker')
    identity = market.get('ticker')
    if (not isinstance(event, str) or not event.startswith('KXBTC-') or
            not isinstance(identity, str) or not identity.startswith(event + '-')):
        raise FamilyError('market_event_identity_mismatch')
    translated = dict(market, strike_type='greater', cap_strike=None,
                      event_ticker='KXBTCD-' + event[len('KXBTC-'):],
                      ticker='KXBTCD-' + identity[len('KXBTC-'):],
                      rules_primary=primary[:match.start('lower')].replace('is between ', 'is above ') +
                      match['lower'] + primary[match.end('upper'):])
    _market_semantics(translated, 'KXBTCD', run_end, now)
    return lower, upper, _common_semantics(market)


def _verify_event(series, event, ticker, close, now, run_end, series_changes, event_changes):
    if (not isinstance(event, dict) or event.get('series_ticker') != ticker or
            event.get('mutually_exclusive') is not (ticker == 'KXBTC')):
        raise FamilyError('event_payoff_metadata_mismatch')
    if not _source_matches(event.get('settlement_sources'), BTC):
        raise FamilyError('event_settlement_source_mismatch')
    if _time(event.get('strike_date')) != _time(close):
        raise FamilyError('event_reference_time_mismatch')
    coefficient = resolve_taker_fee(series, event)
    if (not isinstance(series_changes, dict) or
            not isinstance(series_changes.get('series_fee_change_arr'), list) or
            not isinstance(event_changes, dict) or
            not isinstance(event_changes.get('event_fee_changes'), list) or
            event_changes.get('cursor') != ''):
        raise FamilyError('incomplete_fee_schedule')
    for scope, changes in (('series', series_changes['series_fee_change_arr']),
                           ('event', event_changes['event_fee_changes'])):
        for change in changes:
            if (not isinstance(change, dict) or change.get('series_ticker') != ticker or
                    (scope == 'event' and change.get('event_ticker') != event['event_ticker'])):
                raise FamilyError('fee_schedule_identity_mismatch')
            if now < _time(change.get('scheduled_ts')) <= run_end:
                raise FamilyError('scheduled_fee_change_during_observation')
    return coefficient


def _volume(market):
    number = _decimal(market.get('volume_24h_fp'))
    if number < 0 or number * 100 != (number * 100).to_integral_value():
        raise FamilyError('invalid_preobservation_volume')
    return int(number * 100)


def qualify_baskets(catalogs, series_by_ticker, events_by_ticker, terms_hashes,
                    now, run_end, series_fee_changes_by_ticker,
                    event_fee_changes_by_ticker, *, metadata_observed_at,
                    rulebook_evidence=None, max_baskets=20, max_markets=64,
                    max_metadata_age_seconds=300):
    """Return a frozen, fee-verified, conditional observation cohort.

    Catalogs are complete {'markets': [...], 'cursor': ''} per series. Events
    and series are their full unwrapped API records. Fee maps contain complete
    API responses. metadata_observed_at is the oldest acquisition start, not
    a market's updated_time; callers archive request/response timestamps.
    The nearest enclosing thresholds are selected in O(n log n), then cohorts
    rotate across expiries with descending bottleneck 24h volume within each.
    Ranking never reads quotes, later trades, profits, or settled outcomes.
    """
    if isinstance(now, str):
        now = _time(now)
    if isinstance(run_end, str):
        run_end = _time(run_end)
    if (not isinstance(now, dt.datetime) or not isinstance(run_end, dt.datetime) or
            now.tzinfo is None or run_end.tzinfo is None or run_end <= now):
        raise FamilyError('invalid_observation_window')
    observed = _time(metadata_observed_at) if isinstance(metadata_observed_at, str) else metadata_observed_at
    if (not isinstance(observed, dt.datetime) or observed.tzinfo is None or
            type(max_metadata_age_seconds) is not int or not 0 <= max_metadata_age_seconds <= 3600 or
            not 0 <= (now - observed).total_seconds() <= max_metadata_age_seconds):
        raise FamilyError('stale_or_future_metadata')
    if (type(max_baskets) is not int or not 1 <= max_baskets <= 1024 or
            type(max_markets) is not int or not 3 <= max_markets <= 1024):
        raise FamilyError('invalid_cohort_budget')
    if not isinstance(catalogs, dict) or set(catalogs) != set(SERIES):
        raise FamilyError('incomplete_reviewed_catalogs')
    total = 0
    seen = set()
    for catalog in catalogs.values():
        if (not isinstance(catalog, dict) or not isinstance(catalog.get('markets'), list) or
                catalog.get('cursor') != ''):
            raise FamilyError('incomplete_catalog')
        total += len(catalog['markets'])
        for m in catalog['markets']:
            if not isinstance(m, dict) or not isinstance(m.get('ticker'), str):
                raise FamilyError('invalid_market_record')
            if m['ticker'] in seen:
                raise FamilyError('duplicate_market_ticker')
            seen.add(m['ticker'])
    if total > MAX_INPUT_MARKETS or len(events_by_ticker) > MAX_INPUT_EVENTS:
        raise FamilyError('input_budget_exceeded')
    exclusions, qualified, sources = [], defaultdict(lambda: {'KXBTC': [], 'KXBTCD': []}), {}

    def exclude(ticker, reason, market=None):
        row = {'series_ticker': ticker, 'reason': reason}
        if market is not None:
            row.update(ticker=market.get('ticker'), event_ticker=market.get('event_ticker'))
        exclusions.append(row)

    for ticker in SERIES:
        series = series_by_ticker.get(ticker, {})
        failure = None
        if series.get('ticker') != ticker or series.get('contract_terms_url') != BTC['terms_url']:
            failure = 'series_terms_mismatch'
        elif terms_hashes.get(ticker, terms_hashes.get(BTC['terms_url'])) != BTC['terms_sha256']:
            failure = 'terms_hash_mismatch'
        elif not _source_matches(series.get('settlement_sources'), BTC):
            failure = 'unreviewed_settlement_source'
        if failure:
            for market in catalogs[ticker]['markets'] or [None]:
                exclude(ticker, failure, market)
            continue
        series_hash = _digest(series)
        # An event response can embed all of its markets. Hash/verify it once,
        # rather than serializing that entire response once per market (O(n²)).
        event_cache = {}
        for market in catalogs[ticker]['markets']:
            try:
                lower, upper, common = _semantics(market, ticker, now, run_end)
                event = events_by_ticker.get(market['event_ticker'], {})
                if event.get('event_ticker') != market['event_ticker']:
                    raise FamilyError('event_identity_mismatch')
                cache_key = (market['event_ticker'], market['close_time'])
                if cache_key not in event_cache:
                    try:
                        coefficient = _verify_event(series, event, ticker, market['close_time'], now, run_end,
                            series_fee_changes_by_ticker.get(ticker),
                            event_fee_changes_by_ticker.get(market['event_ticker']))
                        event_cache[cache_key] = (coefficient, {
                            'series_sha256': series_hash, 'event_sha256': _digest(event),
                            'series_fee_changes_sha256': _digest(series_fee_changes_by_ticker[ticker]),
                            'event_fee_changes_sha256': _digest(event_fee_changes_by_ticker[market['event_ticker']]),
                        })
                    except FamilyError as error:
                        event_cache[cache_key] = str(error)
                verified = event_cache[cache_key]
                if isinstance(verified, str):
                    raise FamilyError(verified)
                coefficient, shared_source_hashes = verified
                volume = _volume(market)
                qualified[common][ticker].append((lower, upper, market, coefficient, volume))
                sources[market['ticker']] = {
                    'market_sha256': _digest(market), **shared_source_hashes,
                }
            except FamilyError as error:
                exclude(ticker, str(error), market)
    candidates = []
    for common, group in sorted(qualified.items(), key=lambda item: repr(item[0])):
        thresholds = sorted(group['KXBTCD'], key=lambda item: (item[0], item[2]['ticker']))
        strikes = [item[0] for item in thresholds]
        ranges = sorted(group['KXBTC'], key=lambda item: (item[0], item[1], item[2]['ticker']))
        if len(set(strikes)) != len(strikes):
            for _, _, m, _, _ in thresholds + ranges:
                exclude('KXBTC' if m['event_ticker'].startswith('KXBTC-') else 'KXBTCD',
                        'ambiguous_threshold_strike', m)
            continue
        for lower, upper, market, coefficient, volume in ranges:
            low_index = bisect_left(strikes, lower) - 1
            high_index = bisect_left(strikes, upper)
            if low_index < 0 or high_index >= len(strikes):
                exclude('KXBTC', 'no_compatible_enclosing_thresholds', market)
                continue
            low, high = thresholds[low_index], thresholds[high_index]
            legs = [{'ticker': item[2]['ticker'], 'side': side, 'coefficient_ppm': item[3]}
                    for item, side in ((low, 'yes'), (high, 'no'),
                                       ((lower, upper, market, coefficient, volume), 'no'))]
            basis = {'lower_threshold_cents': low[0], 'upper_threshold_cents': high[0],
                     'interval_lower_cents': lower, 'interval_upper_cents': upper}
            tickers = [leg['ticker'] for leg in legs]
            fingerprint = _digest({'common': common, 'basis': basis, 'tickers': tickers})
            candidates.append({
                'stablekey': 'btc-range-' + fingerprint[:24],
                'lower_ticker': tickers[0], 'upper_ticker': tickers[1], 'range_ticker': tickers[2],
                **basis, 'legs': legs, 'floor_dollars': '2', 'fee_verified': True,
                'close_time': market['close_time'], 'expiration_time': market['expiration_time'],
                'model_classification': MODEL_CLASSIFICATION, 'observation_only': True,
                'unconditional_certificate': False, 'covered_exception': 'common_all_no_missing_data',
                'uncovered_exceptions': ['exchange_outcome_review', 'contract_modification',
                                         'independent_discretionary_fractional_payout'],
                'terms_url': BTC['terms_url'], 'terms_sha256': BTC['terms_sha256'],
                'rule_fingerprint': fingerprint,
                'minimum_volume_24h_centicontracts': min(volume, low[4], high[4]),
                'enclosure_slack_cents': (lower - low[0]) + (high[0] - upper),
                'source_record_hashes': {t: sources[t] for t in tickers},
                'provenance': 'BTC 2026-09-17 exact common BRTI minute; a<l<=u<=b; '
                              '2+A-B-I>=2 under scalar/all-NO model only. Gross funding; '
                              'no cross-event collateral credit; no execution permission.',
            })
    cohorts = defaultdict(list)
    for basket in candidates:
        cohorts[_time(basket['close_time'])].append(basket)
    for close in cohorts:
        cohorts[close].sort(key=lambda b: (-b['minimum_volume_24h_centicontracts'],
                                           b['enclosure_slack_cents'], b['stablekey']))
    rotating = deque(deque(cohorts[close]) for close in sorted(cohorts))
    selected, selected_tickers = [], set()
    budget_rejections = Counter()
    while rotating:
        cohort = rotating.popleft()
        basket = cohort.popleft()
        if cohort:
            rotating.append(cohort)
        tickers = {leg['ticker'] for leg in basket['legs']}
        if len(selected) >= max_baskets:
            budget_rejections['basket_budget'] += 1
        elif len(selected_tickers | tickers) > max_markets:
            budget_rejections['market_budget'] += 1
        else:
            selected.append(basket)
            selected_tickers.update(tickers)
    all_markets = {m['ticker']: m for c in catalogs.values() for m in c['markets']}
    exclusions.sort(key=lambda r: (r['series_ticker'], r.get('event_ticker') or '',
                                    r.get('ticker') or '', r['reason']))
    return {'schema': 'btc-basket-qualification-v1', 'baskets': selected,
            'markets': [all_markets[t] for t in sorted(selected_tickers)], 'exclusions': exclusions,
            'model_classification': MODEL_CLASSIFICATION, 'observation_only': True,
            'metadata_observed_at': observed.isoformat(),
            'rulebook_evidence': rulebook_evidence,
            'selection_policy': {
                'version': 'nearest-enclosing-expiry-round-robin-volume-v1',
                'max_baskets': max_baskets, 'max_markets': max_markets,
                'ranking_uses_future_outcomes': False, 'ranking_uses_prices': False,
                'economic_optimality_claim': False,
                'description': 'Nearest enclosing thresholds by cents; rotate ascending expiries; '
                               'within expiry descending bottleneck pre-observation 24h volume, '
                               'then minimum enclosure slack and stable key.'},
            'counts': {'catalog_markets': total, 'conditional_candidates': len(candidates),
                       'selected_baskets': len(selected), 'selected_markets': len(selected_tickers),
                       'exclusions_by_reason': dict(sorted(Counter(e['reason'] for e in exclusions).items())),
                       'budget_exclusions_by_reason': dict(sorted(budget_rejections.items()))}}
