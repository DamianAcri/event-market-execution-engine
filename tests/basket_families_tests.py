"""Independent payoff boundaries and fail-closed public-source qualification."""
import copy
import datetime as dt
from decimal import Decimal
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).parents[1] / 'scripts'))
import basket_families as baskets
from market_families import FamilyError, REVIEWED_SECONDARY


def fixture(day=17):
    catalogs, series, events, series_changes, event_changes = {}, {}, {}, {}, {}
    for ticker in baskets.SERIES:
        event_ticker = ticker + '-26SEP' + str(day) + '17'
        close = '2026-09-' + str(day) + 'T21:00:00Z'
        source = [{'name': 'CF Benchmarks', 'url': baskets.BTC['source_url']}]
        series[ticker] = {'ticker': ticker, 'settlement_sources': source,
                          'contract_terms_url': baskets.BTC['terms_url'],
                          'fee_type': 'quadratic', 'fee_multiplier': 1}
        events[event_ticker] = {'event_ticker': event_ticker, 'series_ticker': ticker,
                               'mutually_exclusive': ticker == 'KXBTC',
                               'settlement_sources': source, 'strike_date': close}
        series_changes[ticker] = {'series_fee_change_arr': []}
        event_changes[event_ticker] = {'event_fee_changes': [], 'cursor': ''}
        markets = []
        bounds = [(99.99, None), (199.99, None), (299.99, None)] if ticker == 'KXBTCD' else [(100, 199.99), (200, 299.99)]
        for index, (lower, upper) in enumerate(bounds):
            condition = 'above ' + str(lower) if upper is None else 'between ' + str(lower) + '-' + str(upper)
            markets.append({
                'ticker': event_ticker + '-X' + str(index), 'event_ticker': event_ticker,
                'market_type': 'binary', 'status': 'active', 'floor_strike': lower,
                'cap_strike': upper, 'strike_type': 'greater' if upper is None else 'between',
                'notional_value_dollars': '1.0000', 'open_time': '2026-09-17T00:00:00Z',
                'close_time': close, 'expiration_time': '2026-09-25T21:00:00Z',
                'expected_expiration_time': '2026-09-' + str(day) + 'T21:05:00Z',
                'latest_expiration_time': '2026-09-25T21:00:00Z',
                'settlement_timer_seconds': 60, 'can_close_early': True,
                'volume_24h_fp': str(100 + index),
                'rules_primary': "If the simple average of the sixty seconds of CF Benchmarks' "
                    'Bitcoin Real-Time Index (BRTI) before 5 PM EDT is ' + condition +
                    ' at 5 PM EDT on Sep ' + str(day) + ', 2026, then the market resolves to Yes.',
                'rules_secondary': REVIEWED_SECONDARY,
            })
        catalogs[ticker] = {'markets': markets, 'cursor': ''}
    return catalogs, series, events, series_changes, event_changes


class BasketFamiliesTests(unittest.TestCase):
    def setUp(self):
        self.now = dt.datetime(2026, 9, 17, 12, tzinfo=dt.timezone.utc)
        self.end = self.now + dt.timedelta(hours=2)
        self.catalogs, self.series, self.events, self.sc, self.ec = fixture()
        self.hashes = {t: baskets.BTC['terms_sha256'] for t in baskets.SERIES}

    def qualify(self, **kwargs):
        return baskets.qualify_baskets(self.catalogs, self.series, self.events, self.hashes,
            self.now, self.end, self.sc, self.ec, metadata_observed_at=kwargs.pop('metadata_observed_at', self.now),
            **kwargs)

    def test_exact_basis_and_three_legs_have_conditional_floor(self):
        result = self.qualify()
        self.assertEqual(result['counts']['selected_baskets'], 2)
        self.assertEqual(result['exclusions'], [])
        for basket in result['baskets']:
            self.assertEqual([leg['side'] for leg in basket['legs']], ['yes', 'no', 'no'])
            self.assertTrue(basket['fee_verified'])
            a, b, lower, upper = [Decimal(basket[name]) / 100 for name in (
                'lower_threshold_cents', 'upper_threshold_cents', 'interval_lower_cents', 'interval_upper_cents')]
            # Independent arithmetic checks every boundary and the unrounded
            # real-number gaps, not just cent-valued scalar outcomes.
            points = {Decimal(0), a, b, lower, upper}
            for boundary in (a, b, lower, upper):
                points.update((boundary - Decimal('.0001'), boundary + Decimal('.0001')))
            for x in points:
                self.assertGreaterEqual(int(x > a) + int(not x > b) + int(not lower <= x <= upper), 2)
            self.assertEqual(0 + (1 - 0) + (1 - 0), 2)  # common missing-data NO

    def test_no_rulebook_input_can_upgrade_to_unconditional_or_order_permission(self):
        result = self.qualify(rulebook_evidence={'status': 'fully_certified', 'allow_orders': True})
        self.assertTrue(result['observation_only'])
        for basket in result['baskets']:
            self.assertEqual(basket['model_classification'], baskets.MODEL_CLASSIFICATION)
            self.assertFalse(basket['unconditional_certificate'])
            self.assertIn('independent_discretionary_fractional_payout', basket['uncovered_exceptions'])
        # Independent exceptional allocations need not preserve the scalar floor.
        self.assertLess(Decimal('.1') + (1 - Decimal('.9')) + (1 - Decimal('.9')), 2)

    def test_nearest_enclosure_strict_lower_inclusive_upper(self):
        # Change the first lower threshold from99.99 to100: equality does not
        # imply YES(A) when the inclusive range settles YES at100.
        market = self.catalogs['KXBTCD']['markets'][0]
        market['floor_strike'] = 100
        market['rules_primary'] = market['rules_primary'].replace('above 99.99', 'above 100')
        result = self.qualify()
        self.assertEqual(result['counts']['selected_baskets'], 1)
        self.assertEqual(result['counts']['exclusions_by_reason']['no_compatible_enclosing_thresholds'], 1)
        remaining = result['baskets'][0]
        self.assertEqual(remaining['upper_threshold_cents'], remaining['interval_upper_cents'])

    def test_no_title_or_ticker_strike_inference(self):
        self.catalogs['KXBTC']['markets'][0]['title'] = 'Wrong and irrelevant title'
        self.assertEqual(self.qualify()['counts']['selected_baskets'], 2)
        self.catalogs['KXBTC']['markets'][0]['rules_primary'] = 'Bitcoin between100 and200'
        self.assertEqual(self.qualify()['counts']['exclusions_by_reason']['unreviewed_rule_text'], 1)

    def test_metadata_age_and_future_fail_closed(self):
        for delta in (-301, 1):
            with self.subTest(delta=delta), self.assertRaisesRegex(FamilyError, 'stale_or_future_metadata'):
                self.qualify(metadata_observed_at=self.now + dt.timedelta(seconds=delta))
        self.assertEqual(self.qualify(metadata_observed_at=self.now - dt.timedelta(seconds=300))['counts']['selected_baskets'], 2)

    def test_complete_catalogs_and_budgets_are_required(self):
        for cursor in ('next', None):
            with self.subTest(cursor=cursor):
                self.catalogs['KXBTC']['cursor'] = cursor
                with self.assertRaisesRegex(FamilyError, 'incomplete_catalog'):
                    self.qualify()
        self.setUp()
        del self.catalogs['KXBTC']
        with self.assertRaisesRegex(FamilyError, 'incomplete_reviewed_catalogs'):
            self.qualify()
        self.setUp()
        with self.assertRaisesRegex(FamilyError, 'invalid_cohort_budget'):
            self.qualify(max_markets=2)

    def test_terms_source_and_unsupported_families_are_not_guessed(self):
        self.hashes['KXBTC'] = 'changed'
        result = self.qualify()
        self.assertEqual(result['counts']['selected_baskets'], 0)
        self.assertEqual(result['counts']['exclusions_by_reason']['terms_hash_mismatch'], 2)
        self.setUp()
        self.series['KXBTC']['settlement_sources'][0]['url'] = 'https://example.org/BRTI'
        self.assertEqual(self.qualify()['counts']['exclusions_by_reason']['unreviewed_settlement_source'], 2)

    def test_range_precision_and_rule_boundary_mismatch_reject(self):
        for field, value, reason in (
                ('floor_strike', '100.001', 'unsupported_strike_precision'),
                ('cap_strike', '200', 'range_rule_mismatch'),
                ('floor_strike', True, 'invalid_number'),
                ('cap_strike', 'Infinity', 'invalid_number'),
                ('rules_secondary', REVIEWED_SECONDARY + ' Modified.', 'unreviewed_rule_text')):
            with self.subTest(field=field, value=value):
                self.setUp(); self.catalogs['KXBTC']['markets'][0][field] = value
                self.assertEqual(self.qualify()['counts']['exclusions_by_reason'][reason], 1)

    def test_different_reference_day_or_expiration_never_shares_basis(self):
        market = self.catalogs['KXBTC']['markets'][0]
        market['rules_primary'] = market['rules_primary'].replace('Sep 17', 'Sep 18')
        self.assertEqual(self.qualify()['counts']['exclusions_by_reason']['rule_reference_close_mismatch'], 1)
        self.setUp()
        self.catalogs['KXBTC']['markets'][0]['expiration_time'] = '2026-09-26T21:00:00Z'
        self.assertEqual(self.qualify()['counts']['exclusions_by_reason']['no_compatible_enclosing_thresholds'], 1)

    def test_inactive_custom_provisional_and_early_close_not_admitted(self):
        for field, value, reason in (
                ('status', 'settled', 'market_not_active'),
                ('custom_strike', {'value': 'x'}, 'unsupported_contract_condition'),
                ('is_provisional', True, 'unsupported_contract_condition'),
                ('close_time', self.end.isoformat(), 'closes_before_capture_end')):
            with self.subTest(field=field):
                self.setUp(); self.catalogs['KXBTC']['markets'][0][field] = value
                self.assertEqual(self.qualify()['counts']['exclusions_by_reason'][reason], 1)

    def test_event_sources_exclusivity_strike_date_and_identity_fail_closed(self):
        for field, value, reason in (
                ('mutually_exclusive', False, 'event_payoff_metadata_mismatch'),
                ('settlement_sources', [], 'event_settlement_source_mismatch'),
                ('strike_date', '2026-09-18T21:00:00Z', 'event_reference_time_mismatch'),
                ('event_ticker', 'OTHER', 'event_identity_mismatch')):
            with self.subTest(field=field):
                self.setUp(); self.events['KXBTC-26SEP1717'][field] = value
                self.assertEqual(self.qualify()['counts']['exclusions_by_reason'][reason], 2)

    def test_per_leg_event_fee_override_and_fractional_multiplier(self):
        self.series['KXBTCD']['fee_multiplier'] = .5
        self.events['KXBTC-26SEP1717'].update(fee_type_override='quadratic_with_maker_fees', fee_multiplier_override=2)
        for basket in self.qualify()['baskets']:
            self.assertEqual([l['coefficient_ppm'] for l in basket['legs']], [35000, 35000, 140000])

    def test_partial_unknown_and_incomplete_fee_metadata_excluded(self):
        self.events['KXBTC-26SEP1717']['fee_type_override'] = 'quadratic'
        self.assertEqual(self.qualify()['counts']['exclusions_by_reason']['partial_event_fee_override'], 2)
        self.setUp(); self.sc['KXBTC'] = {}
        self.assertEqual(self.qualify()['counts']['exclusions_by_reason']['incomplete_fee_schedule'], 2)
        self.setUp(); self.ec['KXBTC-26SEP1717']['cursor'] = 'more'
        self.assertEqual(self.qualify()['counts']['exclusions_by_reason']['incomplete_fee_schedule'], 2)

    def test_announced_fee_change_in_window_rejects_including_endpoint(self):
        change = {'series_ticker': 'KXBTC', 'event_ticker': 'KXBTC-26SEP1717',
                  'scheduled_ts': self.end.isoformat()}
        self.ec['KXBTC-26SEP1717']['event_fee_changes'] = [change]
        self.assertEqual(self.qualify()['counts']['exclusions_by_reason']['scheduled_fee_change_during_observation'], 2)
        change['scheduled_ts'] = self.now.isoformat()
        self.assertEqual(self.qualify()['counts']['selected_baskets'], 2)
        change['event_ticker'] = 'OTHER'
        self.assertEqual(self.qualify()['counts']['exclusions_by_reason']['fee_schedule_identity_mismatch'], 2)

    def test_duplicate_id_fails_and_duplicate_scalar_strike_is_ambiguous(self):
        duplicate = copy.deepcopy(self.catalogs['KXBTCD']['markets'][0])
        self.catalogs['KXBTCD']['markets'].append(duplicate)
        with self.assertRaisesRegex(FamilyError, 'duplicate_market_ticker'):
            self.qualify()
        duplicate['ticker'] += '-OTHER'
        result = self.qualify()
        self.assertEqual(result['counts']['selected_baskets'], 0)
        self.assertEqual(result['counts']['exclusions_by_reason']['ambiguous_threshold_strike'], 6)

    def test_selection_is_deterministic_immutable_and_independent_of_prices(self):
        before = copy.deepcopy(self.catalogs)
        original = self.qualify()
        self.assertEqual(before, self.catalogs)
        for catalog in self.catalogs.values():
            catalog['markets'].reverse()
        self.assertEqual(original, self.qualify())
        expected = [b['stablekey'] for b in original['baskets']]
        for catalog in self.catalogs.values():
            for market in catalog['markets']:
                market.update(yes_ask_dollars='.01', yes_bid_dollars='.99')
        self.assertEqual(expected, [b['stablekey'] for b in self.qualify()['baskets']])

    def test_budget_rotation_samples_multiple_expiries_before_second_basket(self):
        catalogs, _, events, _, ec = fixture(day=18)
        for ticker in baskets.SERIES:
            self.catalogs[ticker]['markets'] += catalogs[ticker]['markets']
        self.events.update(events); self.ec.update(ec)
        result = self.qualify(max_baskets=2)
        self.assertEqual(len({b['close_time'] for b in result['baskets']}), 2)
        self.assertEqual(result['counts']['budget_exclusions_by_reason']['basket_budget'], 2)
        result = self.qualify(max_markets=3)
        self.assertEqual(result['counts']['selected_baskets'], 1)
        self.assertEqual(result['counts']['budget_exclusions_by_reason']['market_budget'], 3)

    def test_provenance_changes_with_fee_or_rule_source_records(self):
        original = self.qualify()['baskets'][0]
        self.series['KXBTC']['fee_multiplier'] = 2
        changed = self.qualify()['baskets'][0]
        self.assertEqual(original['stablekey'], changed['stablekey'])
        self.assertNotEqual(original['source_record_hashes'], changed['source_record_hashes'])


if __name__ == '__main__':
    unittest.main()
