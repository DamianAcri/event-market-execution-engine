"""Reviewed contract semantics for discovery, independent of economic ranking.

This registry certifies only implications within a single reviewed threshold event.
It does not infer semantics from titles or treat unsupported contracts as equivalent.
Terms were read in full on 2026-09-17: both BTC and ETH use the same minute average
for an event, pay USD 1, and resolve NO when the common reference data is absent.
No NFL rule is admitted: exceptional fractional/fair-price payouts are unsupported.

Sources: https://assets.kalshi.com/contract_terms/{BTC,ETH}.pdf,
https://docs.kalshi.com/openapi.yaml (EventData, Series, fee-change endpoints),
https://kalshi.com/docs/kalshi-fee-schedule.pdf (general taker coefficient), and
https://docs.kalshi.com/getting_started/fee_rounding (balance precision).
The fee helper verifies published schedules, not a member's private fee tier.
"""
from collections import Counter
import datetime as dt
from decimal import Decimal, InvalidOperation
import hashlib
import json
import re
from urllib.parse import urlsplit


REVIEWED_FAMILIES = {
    'KXBTCD': {
        'family': 'btc_minute_above', 'asset': 'Bitcoin', 'index': 'BRTI',
        'terms_url': 'https://assets.kalshi.com/contract_terms/BTC.pdf',
        'terms_sha256': 'e7d857369971e75e9db14c5e2d91c29b94eb9a06e83e2acd9777991c4f2a0e2f',
        'source_url': 'https://www.cfbenchmarks.com/data/indices/BRTI',
    },
    'KXETHD': {
        'family': 'eth_minute_above', 'asset': 'Ethereum', 'index': 'ERTI',
        'terms_url': 'https://assets.kalshi.com/contract_terms/ETH.pdf',
        'terms_sha256': 'ae079241099608c13c0c0ed31a6c91be174c6e61abe706dd76e8976ba682cc6d',
        'source_url': 'https://www.cfbenchmarks.com/data/indices/ETHUSD_RTI',
    },
}

REVIEWED_SECONDARY = (
    'Not all cryptocurrency price data is the same. While checking a source like '
    'Google or Coinbase may help guide your decision, the price used to determine '
    "this market is based on CF Benchmarks' corresponding Real Time Index (RTI). "
    'At the last minute before expiration, 60 RTI prices are collected. The official '
    'and final value is the average of these prices.'
)

_MONTHS = {name: i + 1 for i, name in enumerate(
    ('Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'))}
_TICKER = re.compile(r'[A-Z0-9][A-Z0-9_.-]{0,127}')
_RULES = {ticker: re.compile(
    r"If the simple average of the sixty seconds of CF Benchmarks' " + entry['asset'] +
    r' Real-Time Index \(' + entry['index'] + r'\) before '
    r'(?P<time>(?P<hour>[0-9]{1,2}) (?P<meridiem>AM|PM) (?P<zone>EDT|EST)) is above '
    r'(?P<strike>[0-9]+(?:\.[0-9]+)?) at (?P=time) on '
    r'(?P<month>[A-Z][a-z]{2}) (?P<day>[0-9]{1,2}), (?P<year>[0-9]{4}), '
    r'then the market resolves to Yes\.'
) for ticker, entry in REVIEWED_FAMILIES.items()}


class FamilyError(ValueError):
    """Fixed reason code; never includes arbitrary source text."""


def _decimal(value):
    if isinstance(value, bool):
        raise FamilyError('invalid_number')
    try:
        result = Decimal(str(value))
    except (InvalidOperation, ValueError):
        raise FamilyError('invalid_number') from None
    if not result.is_finite():
        raise FamilyError('invalid_number')
    return result


def _time(value):
    try:
        result = dt.datetime.fromisoformat(value.replace('Z', '+00:00'))
    except (ValueError, TypeError, AttributeError):
        raise FamilyError('invalid_timestamp') from None
    if result.tzinfo is None or result.utcoffset() is None:
        raise FamilyError('timestamp_without_timezone')
    return result.astimezone(dt.timezone.utc)


def _source_matches(sources, entry):
    # The venue appends a marketing query to BRTI's URL; queries do not alter the
    # named index. Authority and path must match, and additional sources fail closed.
    if not isinstance(sources, list) or len(sources) != 1:
        return False
    source = sources[0]
    if not isinstance(source, dict) or source.get('name') != 'CF Benchmarks':
        return False
    try:
        actual, expected = urlsplit(source.get('url', '')), urlsplit(entry['source_url'])
        return (actual.scheme, actual.netloc, actual.path, actual.fragment) == (
            expected.scheme, expected.netloc, expected.path, '')
    except (TypeError, ValueError):
        return False


def resolve_taker_fee(series, event=None):
    """Return exact integral ppm, including a paired per-event public override.

    Both admitted quadratic schedules share the general taker formula. Maker
    coefficients are deliberately absent: this engine's current policy is IOC.
    Unknown/flat/partially specified rates fail closed rather than default to 1.
    """
    fee_type = series.get('fee_type')
    multiplier = series.get('fee_multiplier')
    if event is not None:
        kind = event.get('fee_type_override')
        factor = event.get('fee_multiplier_override')
        if (kind is None) != (factor is None):
            raise FamilyError('partial_event_fee_override')
        if kind is not None:
            fee_type, multiplier = kind, factor
    if fee_type not in ('quadratic', 'quadratic_with_maker_fees'):
        raise FamilyError('unsupported_fee_type')
    factor = _decimal(multiplier)
    coefficient = factor * 70000
    if factor < 0 or coefficient > 1000000 or coefficient != coefficient.to_integral_value():
        raise FamilyError('unsupported_fee_multiplier')
    return int(coefficient)


def _market_semantics(market, ticker, deadline, now):
    entry = REVIEWED_FAMILIES[ticker]
    if market.get('status') != 'active':
        raise FamilyError('market_not_active')
    if market.get('market_type') != 'binary' or market.get('strike_type') != 'greater':
        raise FamilyError('unsupported_market_payoff')
    if (not isinstance(market.get('ticker'), str) or not _TICKER.fullmatch(market['ticker']) or
            not isinstance(market.get('event_ticker'), str) or
            not _TICKER.fullmatch(market['event_ticker']) or
            not market['event_ticker'].startswith(ticker + '-') or
            not market['ticker'].startswith(market['event_ticker'] + '-')):
        raise FamilyError('market_event_identity_mismatch')
    closes, expires = _time(market.get('close_time')), _time(market.get('expiration_time'))
    if closes <= deadline:
        raise FamilyError('closes_before_capture_end')
    if expires < closes:
        raise FamilyError('expiration_before_close')
    if now is not None and _time(market.get('open_time')) > now:
        raise FamilyError('market_not_open_yet')
    if _decimal(market.get('notional_value_dollars')) != 1:
        raise FamilyError('unsupported_notional')
    if market.get('cap_strike') is not None or any(market.get(field) for field in (
            'functional_strike', 'custom_strike', 'is_provisional', 'fee_waiver_expiration_time',
            'mve_collection_ticker', 'mve_selected_legs', 'result')):
        raise FamilyError('unsupported_contract_condition')
    primary = market.get('rules_primary')
    match = _RULES[ticker].fullmatch(primary) if isinstance(primary, str) else None
    if match is None or market.get('rules_secondary') != REVIEWED_SECONDARY:
        raise FamilyError('unreviewed_rule_text')
    strike = _decimal(market.get('floor_strike'))
    if (strike != _decimal(match['strike']) or not 0 <= strike <= 100000000 or
            strike * 100 != (strike * 100).to_integral_value()):
        raise FamilyError('strike_rule_mismatch')
    try:
        hour = int(match['hour'])
        if not 1 <= hour <= 12:
            raise ValueError
        hour = hour % 12 + (12 if match['meridiem'] == 'PM' else 0)
        reference_time = dt.datetime(int(match['year']), _MONTHS[match['month']], int(match['day']),
                                     hour, tzinfo=dt.timezone(dt.timedelta(
                                         hours=-4 if match['zone'] == 'EDT' else -5)))
    except (ValueError, KeyError):
        raise FamilyError('invalid_rule_reference_time') from None
    if reference_time != closes:
        raise FamilyError('rule_reference_close_mismatch')
    for field in ('expected_expiration_time', 'latest_expiration_time'):
        if market.get(field) is not None and _time(market[field]) < closes:
            raise FamilyError('expiration_before_close')
    if (market.get('can_close_early') not in (None, False, True) or
            (market.get('settlement_timer_seconds') is not None and
             (type(market['settlement_timer_seconds']) is not int or market['settlement_timer_seconds'] < 0))):
        raise FamilyError('unsupported_settlement_metadata')
    normalized = primary[:match.start('strike')] + '<STRIKE>' + primary[match.end('strike'):]
    # All fields affecting the common determination window must agree exactly.
    semantics = (normalized, market['rules_secondary'], market['close_time'],
                 market['expiration_time'], market.get('expected_expiration_time'),
                 market.get('latest_expiration_time'), market.get('settlement_timer_seconds'),
                 market.get('can_close_early'), entry['terms_sha256'])
    return strike, semantics


def qualify_catalog(catalogs, series_by_ticker, terms_hashes, deadline, now=None):
    """Qualify every listed event; no budget, activity filter, or economic score.

    catalogs is {series_ticker: {'markets': [...], 'cursor': ''}}; callers must
    complete pagination before this function. terms_hashes is keyed by series
    ticker (or its exact terms URL) and contains hashes of freshly archived bytes.
    Returned groups still require verify_group_fees before economic admission.
    """
    if deadline.tzinfo is None or (now is not None and now.tzinfo is None):
        raise FamilyError('timestamp_without_timezone')
    groups, excluded = [], []

    def exclude(ticker, reason, market=None):
        row = {'series_ticker': ticker, 'reason': reason}
        if isinstance(market, dict):
            row.update(ticker=market.get('ticker'), event_ticker=market.get('event_ticker'))
        excluded.append(row)

    for ticker in sorted(catalogs):
        catalog = catalogs[ticker]
        if not isinstance(catalog, dict) or not isinstance(catalog.get('markets'), list):
            raise FamilyError('invalid_catalog')
        markets = catalog['markets']
        entry = REVIEWED_FAMILIES.get(ticker)
        series = series_by_ticker.get(ticker, {})
        reason = None
        if entry is None:
            reason = 'unreviewed_family'
        elif catalog.get('cursor'):
            reason = 'incomplete_catalog'
        elif series.get('ticker') != ticker or series.get('contract_terms_url') != entry['terms_url']:
            reason = 'series_terms_mismatch'
        elif terms_hashes.get(ticker, terms_hashes.get(entry['terms_url'])) != entry['terms_sha256']:
            reason = 'terms_hash_mismatch'
        elif not _source_matches(series.get('settlement_sources'), entry):
            reason = 'unreviewed_settlement_source'
        if reason:
            for market in markets or [None]:
                exclude(ticker, reason, market)
            continue
        try:
            coefficient = resolve_taker_fee(series)
        except FamilyError as error:
            for market in markets or [None]:
                exclude(ticker, str(error), market)
            continue
        events = {}
        seen = set()
        # Duplicate identities taint the whole series snapshot; arbitrary first
        # record selection could bind a ticker to the wrong rule or quote.
        if any(not isinstance(m, dict) or not isinstance(m.get('ticker'), str) for m in markets):
            raise FamilyError('invalid_market_record')
        for market in markets:
            if market['ticker'] in seen:
                raise FamilyError('duplicate_market_ticker')
            seen.add(market['ticker'])
            try:
                strike, semantics = _market_semantics(market, ticker, deadline, now)
                events.setdefault(market['event_ticker'], []).append((strike, semantics, market))
            except FamilyError as error:
                exclude(ticker, str(error), market)
        for event, members in sorted(events.items()):
            failure = None
            if len(members) < 2:
                failure = 'fewer_than_two_compatible_strikes'
            elif len({item[1] for item in members}) != 1:
                failure = 'event_settlement_conditions_mismatch'
            elif len({item[0] for item in members}) != len(members):
                failure = 'duplicate_strike_requires_equivalence_review'
            if failure:
                for _, _, market in members:
                    exclude(ticker, failure, market)
                continue
            members.sort(key=lambda item: (item[0], item[2]['ticker']))
            fingerprint = hashlib.sha256(json.dumps(members[0][1], separators=(',', ':')).encode()).hexdigest()
            groups.append({
                'series_ticker': ticker, 'event_ticker': event, 'family': entry['family'],
                'terms_url': entry['terms_url'], 'terms_sha256': entry['terms_sha256'],
                'markets': [item[2] for item in members],
                'close_time': members[0][2]['close_time'],
                'expiration_time': members[0][2]['expiration_time'],
                'rule_fingerprint': fingerprint, 'coefficient_ppm': coefficient,
                'fee_verified': False,
                'relationship_type': 'implication',
                'provenance': 'Reviewed ' + entry['family'] + ' 2026-09-17; higher threshold implies lower. '
                              'Same event/reference minute/settlement rules; absent data resolves both NO. '
                              'No cross-event, cross-asset, or arbitrary fair-price settlement claim.',
            })
    excluded.sort(key=lambda row: (row['series_ticker'], row.get('event_ticker') or '',
                                  row.get('ticker') or '', row['reason']))
    return {'groups': groups, 'exclusions': excluded,
            'counts': {'catalog_markets': sum(len(c['markets']) for c in catalogs.values()),
                       'qualified_markets': sum(len(g['markets']) for g in groups),
                       'qualified_groups': len(groups),
                       'exclusions_by_reason': dict(sorted(Counter(r['reason'] for r in excluded).items()))}}


def verify_group_fees(group, series, event, now, run_end, series_fee_changes, event_fee_changes):
    """Admit a fixed-policy window only after public override/schedule checks.

    The dictionaries are complete API responses, with event pages consolidated
    and cursor empty. Reject any announced fee change during the run, even if it
    appears harmless; the current paper engine cannot change its policy mid-run.
    """
    if now.tzinfo is None or run_end.tzinfo is None or run_end <= now:
        raise FamilyError('invalid_fee_window')
    ticker = group['series_ticker']
    entry = REVIEWED_FAMILIES.get(ticker)
    if (entry is None or series.get('ticker') != ticker or
            event.get('series_ticker') != ticker or event.get('event_ticker') != group['event_ticker']):
        raise FamilyError('event_identity_mismatch')
    # collateral_return_type describes optional account netting, not the dollar
    # payoff of a contract. DIRECNET is currently published for these thresholds.
    # Paper funding stays at gross cost and assumes collateral return disabled.
    if event.get('mutually_exclusive') is not False:
        raise FamilyError('event_payoff_metadata_mismatch')
    if not _source_matches(event.get('settlement_sources'), entry):
        raise FamilyError('event_settlement_source_mismatch')
    if event.get('strike_date') is not None and _time(event['strike_date']) != _time(group['close_time']):
        raise FamilyError('event_reference_time_mismatch')
    coefficient = resolve_taker_fee(series, event)
    if (not isinstance(series_fee_changes, dict) or
            not isinstance(series_fee_changes.get('series_fee_change_arr'), list) or
            not isinstance(event_fee_changes, dict) or
            not isinstance(event_fee_changes.get('event_fee_changes'), list) or
            event_fee_changes.get('cursor') != ''):
        raise FamilyError('incomplete_fee_schedule')
    for scope, changes in [('series', series_fee_changes['series_fee_change_arr']),
                           ('event', event_fee_changes['event_fee_changes'])]:
        for change in changes:
            if (not isinstance(change, dict) or change.get('series_ticker') != ticker or
                    (scope == 'event' and change.get('event_ticker') != group['event_ticker'])):
                raise FamilyError('fee_schedule_identity_mismatch')
            if now < _time(change.get('scheduled_ts')) <= run_end:
                raise FamilyError('scheduled_fee_change_during_capture')
    result = dict(group)
    result.update(coefficient_ppm=coefficient, fee_verified=True,
                  fee_provenance='Published quadratic taker coefficient with series multiplier and event override; '
                                 'announced series/event fee changes excluded through capture end. '
                                 'Private account tier unqueried; no maker-fee model. '
                                 'https://docs.kalshi.com/openapi.yaml')
    return result
