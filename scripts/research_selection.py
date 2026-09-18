"""Deterministic coverage sampling, not a fitted profitability ranking."""
import datetime as dt
import json
from decimal import Decimal

NFL_TERMS = {
    'KXNFLGAME': ('https://assets.kalshi.com/contract_terms/FOOTBALLGAMEWIN.pdf',
                  '19578b71cd63da63cc2894c60542ef06bb5de56b9366add56f07cee645cfca4b'),
    'KXNFLSPREAD': ('https://assets.kalshi.com/contract_terms/FOOTBALLSPREAD.pdf',
                    '1daaea87a853adcb485bdb80fb8bd5c5fa7c831d8e237f7446bc16ea43705f5a'),
}


def spaced(values, count):
    """Include endpoints and evenly spaced ranks; no future prices or outcomes."""
    if len(values) <= count:
        return list(values)
    if count == 1:
        return [values[len(values) // 2]]
    return [values[i * (len(values) - 1) // (count - 1)] for i in range(count)]


def select_research(public, series, now, seconds, validate_btc, error,
                    btc_events=2, btc_per_event=16, nfl_budget=16, nfl_sources=None):
    if (public.get('cursor') or not 1 <= btc_events <= 4 or
            not 2 <= btc_per_event <= 32 or not 0 <= nfl_budget <= 32 or
            btc_events * btc_per_event + nfl_budget > 64):
        raise error('Cobertura incompleta o presupuesto fuera del limite de 64 mercados.')
    deadline = now + dt.timedelta(seconds=seconds + 600)
    groups = {}
    for market in public['markets']:
        if (market['status'] == 'active' and market.get('strike_type') == 'greater' and
                dt.datetime.fromisoformat(market['close_time'].replace('Z', '+00:00')) > deadline):
            groups.setdefault(market['event_ticker'], []).append(market)
    events = sorted((event for event in groups if len(groups[event]) >= 2),
                    key=lambda event: (min(m['close_time'] for m in groups[event]), event))
    if len(events) < btc_events:
        raise error('No hay suficientes eventos BTC para la cobertura solicitada.')
    metadata = {'schema_version': 1, 'metadata_version': int(now.strftime('%Y%m%d%H%M%S')),
                'venue': 'kalshi', 'markets': [], 'constraints': []}
    selected, coverage = [], []
    for event in events[:btc_events]:
        ordered = sorted(groups[event], key=lambda m: (Decimal(str(m['floor_strike'])), m['ticker']))
        sample = spaced(ordered, btc_per_event)
        checked, chosen = validate_btc({'cursor': '', 'markets': sample}, series, now, seconds, len(sample))
        offset = len(metadata['markets'])
        metadata['markets'].extend({'id': m['id'] + offset, 'ticker': m['ticker']} for m in checked['markets'])
        for relation in checked['constraints']:
            relation['id'] = len(metadata['constraints']) + 1
            for key in ('antecedent', 'consequent'):
                relation['relationship'][key] += offset
            metadata['constraints'].append(relation)
        selected.extend(chosen)
        coverage.append({'event': event, 'role': 'certified_btc_threshold',
                         'eligible_markets': len(ordered), 'selected_markets': len(chosen),
                         'excluded_by_budget': len(ordered) - len(chosen)})
    # NFL grouping is for observation only: matching ticker suffixes do NOT
    # establish a payout relation. Full rules include ties/fair-value exceptions.
    sports_groups = {}
    for ticker, public_sports in (nfl_sources or {}).items():
        if ticker not in NFL_TERMS or public_sports.get('cursor'):
            raise error('La cobertura NFL no esta completa o la serie no esta revisada.')
        for market in public_sports['markets']:
            if (market['status'] != 'active' or
                    dt.datetime.fromisoformat(market['close_time'].replace('Z', '+00:00')) <= deadline):
                continue
            event = market['event_ticker']
            if not event.startswith(ticker + '-'):
                raise error('Identificador de evento NFL inesperado.')
            sports_groups.setdefault(event[len(ticker) + 1:], {}).setdefault(ticker, []).append(market)
    paired = [key for key, group in sports_groups.items() if set(group) == set(NFL_TERMS)]
    paired.sort(key=lambda key: (min(m.get('expected_expiration_time') or m['close_time']
                                   for markets in sports_groups[key].values() for m in markets), key))
    if paired and nfl_budget >= 4:
        key = paired[0]
        winners = sorted(sports_groups[key]['KXNFLGAME'], key=lambda m: m['ticker'])
        spreads = sorted(sports_groups[key]['KXNFLSPREAD'],
                         key=lambda m: (json.dumps(m.get('custom_strike'), sort_keys=True), Decimal(str(m['floor_strike'])), m['ticker']))
        observations = spaced(winners, min(len(winners), nfl_budget // 2))
        observations += spaced(spreads, nfl_budget - len(observations))
        for market in observations:
            metadata['markets'].append({'id': len(metadata['markets']) + 1, 'ticker': market['ticker']})
        selected.extend(observations)
        coverage.append({'event_group': key, 'role': 'observation_only_nfl',
                         'eligible_markets': len(winners) + len(spreads), 'selected_markets': len(observations),
                         'excluded_by_budget': len(winners) + len(spreads) - len(observations),
                         'certified_relationships': 0,
                         'reason': 'Fractional states are not modeled, and no cross-contract lower bound has been established for discretionary fair-value/cancellation settlements.'})
    if len({m['ticker'] for m in metadata['markets']}) != len(metadata['markets']):
        raise error('Mercados duplicados en la seleccion.')
    report = {'profile': 'research_v1', 'selection_frozen_at': now.isoformat(),
              'selection_method': 'Earliest eligible BTC events, evenly spaced strike ranks including tails; earliest paired NFL group by expected expiration, sampled by rank for observation only. Not an optimal-market claim.',
              'universe': ['KXBTCD', *list((nfl_sources or {}).keys())],
              'listed_btc_markets': len(public['markets']), 'eligible_btc_events': len(events),
              'selected_btc_events': btc_events, 'excluded_btc_events': len(events) - btc_events,
              'eligible_paired_nfl_groups': len(paired), 'groups': coverage,
              'selected_markets': len(selected), 'certified_relationships': len(metadata['constraints']),
              'observation_only_markets': sum(g['selected_markets'] for g in coverage if g['role'] == 'observation_only_nfl'),
              'scope_limit': 'Static bounded sample of three series, not the whole venue. Missing observations are not evidence of absent opportunities.'}
    return metadata, selected, report
