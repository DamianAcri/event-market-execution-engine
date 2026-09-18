"""Rule qualification and fee boundaries, independent synthetic public records."""
import copy
import datetime as dt
from decimal import Decimal
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).parents[1] / 'scripts'))
import market_families as families


def fixture(ticker='KXBTCD', strikes=(100, 200, 300)):
    entry = families.REVIEWED_FAMILIES[ticker]
    event = ticker + '-26SEP1717'
    series = {'ticker': ticker, 'contract_terms_url': entry['terms_url'],
              'settlement_sources': [{'name': 'CF Benchmarks', 'url': entry['source_url']}],
              'fee_type': 'quadratic', 'fee_multiplier': 1}
    markets = []
    for strike in strikes:
        markets.append({
            'ticker': event + '-T' + str(strike), 'event_ticker': event,
            'market_type': 'binary', 'status': 'active', 'strike_type': 'greater',
            'floor_strike': strike, 'notional_value_dollars': '1.0000',
            'open_time': '2026-09-17T00:00:00Z', 'close_time': '2026-09-17T21:00:00Z',
            'expiration_time': '2026-09-24T21:00:00Z',
            'expected_expiration_time': '2026-09-17T21:05:00Z',
            'rules_primary': "If the simple average of the sixty seconds of CF Benchmarks' " + entry['asset'] +
                ' Real-Time Index (' + entry['index'] + ') before 5 PM EDT is above ' + str(strike) +
                ' at 5 PM EDT on Sep 17, 2026, then the market resolves to Yes.',
            'rules_secondary': families.REVIEWED_SECONDARY,
        })
    event_data = {'event_ticker': event, 'series_ticker': ticker,
                  'collateral_return_type': 'DIRECNET', 'mutually_exclusive': False,
                  'settlement_sources': series['settlement_sources'], 'strike_date': '2026-09-17T21:00:00Z'}
    return {'markets': markets, 'cursor': ''}, series, event_data


class FamilyTests(unittest.TestCase):
    def setUp(self):
        self.now = dt.datetime(2026, 9, 17, 12, tzinfo=dt.timezone.utc)
        self.end = self.now + dt.timedelta(hours=2)
        self.hashes = {ticker: entry['terms_sha256'] for ticker, entry in families.REVIEWED_FAMILIES.items()}
        self.catalogs, self.series, self.events = {}, {}, {}
        for ticker in families.REVIEWED_FAMILIES:
            catalog, series, event = fixture(ticker)
            self.catalogs[ticker] = catalog
            self.series[ticker] = series
            self.events[ticker] = event

    def qualify(self):
        return families.qualify_catalog(self.catalogs, self.series, self.hashes, self.end, self.now)

    def verify(self, group=None, event=None, series_changes=None, event_changes=None):
        group = self.qualify()['groups'][0] if group is None else group
        ticker = group['series_ticker']
        return families.verify_group_fees(group, self.series[ticker], event or self.events[ticker],
            self.now, self.end,
            series_changes if series_changes is not None else {'series_fee_change_arr': []},
            event_changes if event_changes is not None else {'event_fee_changes': [], 'cursor': ''})

    def test_reviewed_btc_and_eth_implications_include_no_data_and_equal_boundaries(self):
        result = self.qualify()
        self.assertEqual(result['counts']['qualified_markets'], 6)
        self.assertEqual(result['exclusions'], [])
        for group in result['groups']:
            self.assertFalse(group['fee_verified'])
            self.assertEqual(group['relationship_type'], 'implication')
            # Boundary prices and missing-data settlement are independent payoff cases.
            for reference in (None, 0, 100, 100.01, 200, 200.01, 300, 300.01):
                outcomes = [False if reference is None else Decimal(str(reference)) > Decimal(str(m['floor_strike']))
                            for m in group['markets']]
                for low in range(len(outcomes)):
                    for high in range(low + 1, len(outcomes)):
                        self.assertFalse(outcomes[high] and not outcomes[low])

    def test_permutation_deterministic_and_inputs_unchanged(self):
        before = copy.deepcopy(self.catalogs)
        original = self.qualify()
        self.assertEqual(self.catalogs, before)
        for catalog in self.catalogs.values():
            catalog['markets'].reverse()
        self.catalogs = dict(reversed(list(self.catalogs.items())))
        self.assertEqual(original, self.qualify())

    def test_nonreviewed_family_never_qualified_from_matching_title(self):
        catalog, series, _ = fixture()
        self.catalogs['KXNFLSPREAD'] = catalog
        self.series['KXNFLSPREAD'] = series
        result = self.qualify()
        self.assertEqual(len(result['groups']), 2)
        self.assertEqual(result['counts']['exclusions_by_reason']['unreviewed_family'], 3)

    def test_series_changes_and_incomplete_pages_exclude_whole_series(self):
        for target, key, value, reason in [
                ('hash', 'KXBTCD', 'changed', 'terms_hash_mismatch'),
                ('series', 'contract_terms_url', 'https://example.org/BTC.pdf', 'series_terms_mismatch'),
                ('series', 'settlement_sources', [], 'unreviewed_settlement_source'),
                ('series', 'fee_type', 'flat', 'unsupported_fee_type'),
                ('catalog', 'cursor', 'next', 'incomplete_catalog')]:
            with self.subTest(reason=reason):
                self.setUp()
                obj = self.hashes if target == 'hash' else self.series['KXBTCD'] if target == 'series' else self.catalogs['KXBTCD']
                obj[key] = value
                result = self.qualify()
                self.assertEqual(len(result['groups']), 1)
                self.assertEqual(result['counts']['exclusions_by_reason'][reason], 3)

    def test_changed_primary_secondary_underlying_and_payout_exclude_contract(self):
        mutations = [
            ('rules_primary', 'Same title, unknown rule', 'unreviewed_rule_text'),
            ('rules_secondary', families.REVIEWED_SECONDARY + ' Fair value exception.', 'unreviewed_rule_text'),
            ('notional_value_dollars', '0.50', 'unsupported_notional'),
            ('floor_strike', 99, 'strike_rule_mismatch'),
            ('floor_strike', 'NaN', 'invalid_number'),
            ('strike_type', 'less', 'unsupported_market_payoff'),
            ('market_type', 'scalar', 'unsupported_market_payoff'),
            ('cap_strike', 150, 'unsupported_contract_condition'),
            ('custom_strike', {'football_team': 'ABC'}, 'unsupported_contract_condition'),
            ('functional_strike', 'other', 'unsupported_contract_condition'),
            ('is_provisional', True, 'unsupported_contract_condition'),
            ('fee_waiver_expiration_time', '2027-01-01T00:00:00Z', 'unsupported_contract_condition'),
            ('status', 'settled', 'market_not_active'),
            ('close_time', '2026-09-17T22:00:00Z', 'rule_reference_close_mismatch'),
        ]
        for field, value, reason in mutations:
            with self.subTest(field=field):
                self.setUp()
                self.catalogs['KXBTCD']['markets'][0][field] = value
                result = self.qualify()
                self.assertEqual(result['counts']['exclusions_by_reason'][reason], 1)
                self.assertEqual(result['counts']['qualified_markets'], 5)

    def test_different_settlement_windows_do_not_share_implications(self):
        self.catalogs['KXBTCD']['markets'][0]['expiration_time'] = '2026-09-25T21:00:00Z'
        result = self.qualify()
        self.assertEqual(result['counts']['exclusions_by_reason']['event_settlement_conditions_mismatch'], 3)
        self.assertEqual(len(result['groups']), 1)

    def test_different_events_and_assets_are_never_merged(self):
        more = copy.deepcopy(self.catalogs['KXBTCD']['markets'])
        for m in more:
            m['event_ticker'] += '-OTHER'
            m['ticker'] = m['event_ticker'] + '-T' + str(m['floor_strike'])
        self.catalogs['KXBTCD']['markets'] += more
        result = self.qualify()
        self.assertEqual(len(result['groups']), 3)
        for group in result['groups']:
            self.assertEqual({m['event_ticker'] for m in group['markets']}, {group['event_ticker']})

    def test_duplicate_identity_fails_and_duplicate_strike_is_not_guessed_equivalent(self):
        m = copy.deepcopy(self.catalogs['KXBTCD']['markets'][0])
        self.catalogs['KXBTCD']['markets'].append(m)
        with self.assertRaisesRegex(families.FamilyError, 'duplicate_market_ticker'):
            self.qualify()
        m['ticker'] += '-OTHER'
        result = self.qualify()
        self.assertEqual(result['counts']['exclusions_by_reason']['duplicate_strike_requires_equivalence_review'], 4)

    def test_time_window_and_timezones_fail_closed(self):
        self.catalogs['KXBTCD']['markets'][0]['open_time'] = '2026-09-17T13:00:00Z'
        self.catalogs['KXETHD']['markets'][0]['close_time'] = '2026-09-17T21:00:00'
        result = self.qualify()
        self.assertEqual(result['counts']['exclusions_by_reason']['market_not_open_yet'], 1)
        self.assertEqual(result['counts']['exclusions_by_reason']['timestamp_without_timezone'], 1)
        result = families.qualify_catalog(self.catalogs, self.series, self.hashes,
                                         dt.datetime(2026, 9, 17, 21, tzinfo=dt.timezone.utc), self.now)
        self.assertEqual(result['counts']['qualified_markets'], 0)

    def test_marketing_query_does_not_change_index_but_authority_path_and_extra_source_do(self):
        sources = self.series['KXBTCD']['settlement_sources']
        sources[0]['url'] += '?ref=blog.cfbenchmarks.com'
        self.assertEqual(len(self.qualify()['groups']), 2)
        sources[0]['url'] = 'https://example.com/data/indices/BRTI'
        self.assertEqual(len(self.qualify()['groups']), 1)
        sources[0]['url'] = families.REVIEWED_FAMILIES['KXBTCD']['source_url']
        sources.append(dict(sources[0]))
        self.assertEqual(len(self.qualify()['groups']), 1)

    def test_fees_use_fractional_series_and_event_multiplier_without_float_rounding(self):
        self.series['KXBTCD']['fee_multiplier'] = 0.5
        self.assertEqual(self.verify()['coefficient_ppm'], 35000)
        event = copy.deepcopy(self.events['KXBTCD'])
        event.update(fee_type_override='quadratic_with_maker_fees', fee_multiplier_override=2)
        group = self.verify(event=event)
        self.assertEqual(group['coefficient_ppm'], 140000)
        self.assertTrue(group['fee_verified'])
        event.update(fee_type_override=None, fee_multiplier_override=None)
        self.assertEqual(self.verify(event=event)['coefficient_ppm'], 35000)

    def test_invalid_fees_never_fall_back_to_general_rate(self):
        for value in (None, True, 'NaN', 'Infinity', -1, 100, '0.123456789'):
            with self.subTest(multiplier=value):
                series = dict(self.series['KXBTCD'], fee_multiplier=value)
                with self.assertRaises(families.FamilyError):
                    families.resolve_taker_fee(series)
        event = dict(self.events['KXBTCD'], fee_type_override='quadratic')
        with self.assertRaisesRegex(families.FamilyError, 'partial_event_fee_override'):
            self.verify(event=event)
        event['fee_multiplier_override'] = 1
        event['fee_type_override'] = 'flat'
        with self.assertRaisesRegex(families.FamilyError, 'unsupported_fee_type'):
            self.verify(event=event)

    def test_event_identity_source_reference_and_exclusivity_are_verified(self):
        for key, value, reason in [
            ('series_ticker', 'OTHER', 'event_identity_mismatch'),
            ('event_ticker', 'OTHER', 'event_identity_mismatch'),
            ('mutually_exclusive', True, 'event_payoff_metadata_mismatch'),
            ('settlement_sources', [], 'event_settlement_source_mismatch'),
            ('strike_date', '2026-09-17T20:00:00Z', 'event_reference_time_mismatch')]:
            with self.subTest(key=key):
                with self.assertRaisesRegex(families.FamilyError, reason):
                    self.verify(event=dict(self.events['KXBTCD'], **{key: value}))

    def test_fee_changes_at_capture_end_reject_fixed_policy_including_reset(self):
        event_change = {'series_ticker': 'KXBTCD', 'event_ticker': self.events['KXBTCD']['event_ticker'],
                        'scheduled_ts': self.end.isoformat(), 'fee_type_override': None,
                        'fee_multiplier_override': None}
        with self.assertRaisesRegex(families.FamilyError, 'scheduled_fee_change_during_capture'):
            self.verify(event_changes={'event_fee_changes': [event_change], 'cursor': ''})
        series_change = {'series_ticker': 'KXBTCD', 'scheduled_ts': (self.now + dt.timedelta(seconds=1)).isoformat(),
                         'fee_type': 'quadratic', 'fee_multiplier': 1}
        with self.assertRaisesRegex(families.FamilyError, 'scheduled_fee_change_during_capture'):
            self.verify(series_changes={'series_fee_change_arr': [series_change]})
        # Current event metadata already supplies the effective state at now.
        series_change['scheduled_ts'] = self.now.isoformat()
        self.assertTrue(self.verify(series_changes={'series_fee_change_arr': [series_change]})['fee_verified'])
        series_change['scheduled_ts'] = (self.end + dt.timedelta(seconds=1)).isoformat()
        self.assertTrue(self.verify(series_changes={'series_fee_change_arr': [series_change]})['fee_verified'])

    def test_incomplete_and_wrong_event_fee_schedules_fail_closed(self):
        for schedule in ({}, {'event_fee_changes': []}, {'event_fee_changes': [], 'cursor': 'next'}):
            with self.subTest(schedule=schedule):
                with self.assertRaisesRegex(families.FamilyError, 'incomplete_fee_schedule'):
                    self.verify(event_changes=schedule)
        with self.assertRaisesRegex(families.FamilyError, 'fee_schedule_identity_mismatch'):
            self.verify(event_changes={'event_fee_changes': [{
                'series_ticker': 'KXBTCD', 'event_ticker': 'OTHER',
                'scheduled_ts': self.end.isoformat()}], 'cursor': ''})


if __name__ == '__main__':
    unittest.main()
