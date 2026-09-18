"""Catalog acquisition protocol checks; synthetic responses, no network or keys."""
import datetime as dt
import hashlib
import gzip
import json
from pathlib import Path
import sys
import tempfile
import unittest
from urllib.parse import parse_qs, urlsplit

sys.path.insert(0, str(Path(__file__).parents[1] / 'scripts'))
from market_catalog import CatalogClient, CatalogError, PublicResponse, acquire_catalog, read_archived_response


def raw(value):
    return json.dumps(value).encode()


def market(ticker='SERIES-EVENT-ONE'):
    return {'ticker': ticker, 'event_ticker': 'SERIES-EVENT', 'status': 'active'}


class Clock:
    def __init__(self):
        self.seconds = 0.0
        self.sleeps = []

    def monotonic(self):
        return self.seconds

    def utcnow(self):
        return dt.datetime(2026, 9, 17, tzinfo=dt.timezone.utc) + dt.timedelta(seconds=self.seconds)

    def sleep(self, seconds):
        self.sleeps.append(seconds)
        self.seconds += seconds


class CatalogTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.clock = Clock()
        self.urls, self.starts = [], []

    def client(self, responses, **kwargs):
        iterator = iter(responses)

        def get(url):
            self.urls.append(url)
            self.starts.append(self.clock.seconds)
            result = next(iterator)
            if isinstance(result, Exception):
                raise result
            return result if isinstance(result, (bytes, PublicResponse)) else raw(result)

        return CatalogClient(self.root, get, sleep=self.clock.sleep,
                             monotonic=self.clock.monotonic, utcnow=self.clock.utcnow, **kwargs)

    def read_manifest(self):
        return json.loads((self.root / 'catalog-manifest.json').read_text())

    def test_complete_catalog_pages_and_archive(self):
        client = self.client([
            {'series': [{'ticker': 'SERIES', 'fee_type': 'quadratic', 'fee_multiplier': 1}]},
            {'markets': [market()], 'cursor': 'page+2/='},
            {'markets': [market('SERIES-EVENT-TWO')], 'cursor': ''}])
        result = acquire_catalog(self.root, client=client, page_size=1)
        self.assertTrue(result['coverage']['complete'])
        self.assertFalse(result['coverage']['snapshot_atomic'])
        self.assertEqual(result['coverage']['market_count'], 2)
        self.assertEqual(result['coverage']['series_count'], 1)
        self.assertEqual(result['coverage']['scope'], 'open_markets_excluding_multivariate')
        query = parse_qs(urlsplit(self.urls[2]).query)
        self.assertEqual(query, {'status': ['open'], 'mve_filter': ['exclude'],
                                 'limit': ['1'], 'cursor': ['page+2/=']})
        self.assertEqual(self.starts, [0.0, 0.2, 0.4])
        manifest = self.read_manifest()
        self.assertEqual(len(manifest['requests']), 3)
        for filename, source in result['sources'].items():
            body = read_archived_response(self.root, filename, source)
            self.assertEqual(source['sha256'], hashlib.sha256(body).hexdigest())
            self.assertEqual(source['stored_sha256'], hashlib.sha256((self.root / filename).read_bytes()).hexdigest())
            self.assertIn('requested_at', source)
            self.assertIn('completed_at', source)
            self.assertEqual(source['http_status'], 200)
        self.assertEqual(manifest['catalog'], result['coverage'])

    def test_empty_final_page_and_empty_series_are_valid(self):
        client = self.client([{'series': []}, {'markets': [], 'cursor': ''}])
        result = acquire_catalog(self.root, client=client)
        self.assertEqual(result['markets'], [])
        self.assertTrue(result['coverage']['complete'])

    def test_page_budget_never_returns_partial_catalog(self):
        client = self.client([{'series': []}, {'markets': [market()], 'cursor': 'more'}])
        with self.assertRaisesRegex(CatalogError, '^market_page_budget_exhausted$'):
            acquire_catalog(self.root, client=client, max_market_pages=1)
        coverage = self.read_manifest()['catalog']
        self.assertFalse(coverage['complete'])
        self.assertEqual(coverage['market_count'], 1)
        self.assertEqual(coverage['failure'], 'market_page_budget_exhausted')

    def test_duplicate_markets_across_pages_rejected(self):
        client = self.client([{'series': []}, {'markets': [market()], 'cursor': 'more'},
                              {'markets': [market()], 'cursor': ''}])
        with self.assertRaisesRegex(CatalogError, 'duplicate_markets_ticker'):
            acquire_catalog(self.root, client=client)
        self.assertFalse(self.read_manifest()['catalog']['complete'])
        self.assertEqual(len(self.read_manifest()['sources']), 3)

    def test_bad_cursor_and_mve_fail_closed(self):
        cases = [({'markets': [market()]}, 'missing_or_invalid_market_cursor'),
                 ({'markets': [market()], 'cursor': None}, 'missing_or_invalid_market_cursor'),
                 ({'markets': [market()], 'cursor': 2}, 'missing_or_invalid_market_cursor'),
                 ({'markets': [], 'cursor': 'more'}, 'nonprogressing_market_cursor'),
                 ({'markets': [dict(market(), mve_collection_ticker='COMBO')], 'cursor': ''},
                  'mve_exclusion_not_respected')]
        for payload, reason in cases:
            with self.subTest(reason=reason), tempfile.TemporaryDirectory() as directory:
                self.root = Path(directory)
                client = self.client([{'series': []}, payload])
                with self.assertRaisesRegex(CatalogError, '^' + reason + '$'):
                    acquire_catalog(self.root, client=client)
                self.assertFalse(self.read_manifest()['catalog']['complete'])

    def test_cursor_loop(self):
        client = self.client([{'series': []}, {'markets': [market()], 'cursor': 'repeat'},
                              {'markets': [market('SERIES-EVENT-TWO')], 'cursor': 'repeat'}])
        with self.assertRaisesRegex(CatalogError, 'nonprogressing_market_cursor'):
            acquire_catalog(self.root, client=client)

    def test_page_size_contract_and_schema_are_checked(self):
        cases = [({'markets': [market(), market('SERIES-EVENT-TWO')], 'cursor': ''},
                  'market_page_exceeds_limit'),
                 ({'markets': {}, 'cursor': ''}, 'invalid_markets_list'),
                 ({'markets': [{'ticker': 'SERIES-EVENT-ONE'}], 'cursor': ''},
                  'invalid_market_event'),
                 ({'markets': [dict(market(), ticker='../../file')], 'cursor': ''},
                  'invalid_markets_ticker')]
        for payload, reason in cases:
            with self.subTest(reason=reason), tempfile.TemporaryDirectory() as directory:
                self.root = Path(directory)
                client = self.client([{'series': []}, payload])
                with self.assertRaisesRegex(CatalogError, '^' + reason + '$'):
                    acquire_catalog(self.root, client=client, page_size=1)

    def test_series_pagination_or_duplicates_are_not_silently_ignored(self):
        for payload in [{'series': [], 'cursor': 'more'},
                        {'series': [{'ticker': 'DUP'}, {'ticker': 'DUP'}]}]:
            with self.subTest(payload=payload), tempfile.TemporaryDirectory() as directory:
                self.root = Path(directory)
                client = self.client([payload])
                with self.assertRaises(CatalogError):
                    acquire_catalog(self.root, client=client)

    def test_429_retry_archives_error_and_respects_retry_after(self):
        client = self.client([PublicResponse(b'{"error":"slow down"}', 429, 2.5),
                              {'series': []}, {'markets': [], 'cursor': ''}])
        result = acquire_catalog(self.root, client=client)
        self.assertTrue(result['coverage']['complete'])
        self.assertEqual(self.starts[:2], [0, 2.5])
        self.assertEqual(len(result['sources']), 3)
        self.assertEqual(self.read_manifest()['requests'][0]['retry_delay_seconds'], 2.5)

    def test_429_exhaustion_and_unbounded_delay_abort(self):
        cases = [([PublicResponse(b'{}', 429)] * 3, 'public_rate_limit_exhausted'),
                 ([PublicResponse(b'{}', 429, 31)], 'retry_after_outside_budget')]
        for responses, reason in cases:
            with self.subTest(reason=reason), tempfile.TemporaryDirectory() as directory:
                self.root = Path(directory)
                client = self.client(responses)
                with self.assertRaisesRegex(CatalogError, reason):
                    acquire_catalog(self.root, client=client)
                self.assertFalse(self.read_manifest()['catalog']['complete'])
                self.assertLessEqual(len(self.urls), 4)

    def test_http_and_transport_errors_are_bounded_and_redacted(self):
        cases = [([PublicResponse(b'{"details":"server value"}', 500)], 'public_http_error'),
                 ([RuntimeError('potentially sensitive text')], 'public_transport_failed')]
        for responses, reason in cases:
            with self.subTest(reason=reason), tempfile.TemporaryDirectory() as directory:
                self.root = Path(directory)
                client = self.client(responses)
                with self.assertRaisesRegex(CatalogError, '^' + reason + '$'):
                    acquire_catalog(self.root, client=client)
                self.assertNotIn('potentially sensitive text', (self.root / 'catalog-manifest.json').read_text())

    def test_json_duplicate_keys_nonfinite_and_bad_encoding_fail(self):
        for response in [b'{"series":[],"series":[]}', b'{"series":[],"number":NaN}', b'\xff', b'[]']:
            with self.subTest(response=response), tempfile.TemporaryDirectory() as directory:
                self.root = Path(directory)
                client = self.client([response])
                with self.assertRaises(CatalogError):
                    acquire_catalog(self.root, client=client)
                self.assertEqual(len(self.read_manifest()['sources']), 1)

    def test_archive_size_limit_leaves_incomplete_manifest(self):
        client = self.client([{'series': [{'ticker': 'SERIES'}]}], max_archive_bytes=2)
        with self.assertRaisesRegex(CatalogError, 'archive_byte_budget_exceeded'):
            acquire_catalog(self.root, client=client)
        self.assertFalse(self.read_manifest()['catalog']['complete'])

    def test_error_responses_count_toward_cumulative_budget(self):
        client = self.client([PublicResponse(b'{}', 429), {'series': []}], max_decoded_bytes=4)
        with self.assertRaisesRegex(CatalogError, 'archive_decoded_budget_exceeded'):
            acquire_catalog(self.root, client=client)
        manifest = self.read_manifest()
        self.assertEqual(manifest['raw_bytes'], 2)
        self.assertEqual(len(manifest['sources']), 1)
        self.assertEqual(len(manifest['requests']), 2)

    def test_injected_writer_records_raw_and_manifest(self):
        writes = []

        def writer(path, content):
            writes.append((path.name, content))
            path.write_bytes(content)

        client = self.client([{'series': []}, {'markets': [], 'cursor': ''}], writer=writer)
        acquire_catalog(self.root, client=client)
        self.assertIn(('catalog-series-attempt-01.json.gz', gzip.compress(raw({'series': []}), compresslevel=6, mtime=0)), writes)
        self.assertEqual(writes[-1][0], 'catalog-manifest.json')

    def test_shared_client_paces_and_archives_orderbook_after_catalog(self):
        client = self.client([{'series': []}, {'markets': [], 'cursor': ''},
                              {'orderbook_fp': {'yes_dollars': [], 'no_dollars': []}}])
        acquire_catalog(self.root, client=client)
        book = client.get_json('markets/SERIES-EVENT-ONE/orderbook?depth=5', 'book-001')
        self.assertIn('orderbook_fp', book)
        self.assertEqual(self.starts, [0, .2, .4])
        self.assertEqual(len(self.read_manifest()['sources']), 3)
        self.assertEqual(client.requests[-1]['requested_at'], '2026-09-17T00:00:00.400000Z')

    def test_public_fee_event_trade_and_batch_endpoints_are_allowed(self):
        paths = ['events/SERIES-EVENT', 'events/fee_changes', 'series/fee_changes',
                 'markets/trades?ticker=SERIES-EVENT-ONE',
                 'markets/orderbooks?tickers=SERIES-EVENT-ONE']
        client = self.client([{} for _ in paths])
        for i, path in enumerate(paths):
            self.assertEqual(client.get_json(path, 'extra-' + str(i)), {})

    def test_nonmarket_endpoints_and_archive_reuse_are_rejected(self):
        client = self.client([{'series': []}])
        for path in ['portfolio/orders', 'markets/X/../../portfolio/orders',
                     'https://example.org/markets', '/markets', 'markets#fragment',
                     'markets/X%2FY/orderbook']:
            with self.subTest(path=path), self.assertRaisesRegex(CatalogError, 'endpoint_not_public_market_data'):
                client.get_json(path, 'unused')
        client.get_json('series', 'series')
        with self.assertRaisesRegex(CatalogError, 'invalid_or_reused_archive_name'):
            client.get_json('series', 'series')
        with self.assertRaisesRegex(CatalogError, 'archive_already_exists'):
            CatalogClient(self.root)

    def test_compression_preserves_exact_original_and_tracks_both_budgets(self):
        original = raw({'series': [{'ticker': 'SERIES', 'rules': 'repeated contract text ' * 1000}]})
        client = self.client([original, {'markets': [], 'cursor': ''}], max_archive_bytes=2000)
        result = acquire_catalog(self.root, client=client)
        self.assertTrue(result['coverage']['complete'])
        manifest = self.read_manifest()
        self.assertGreater(manifest['decoded_bytes'], 2000)
        self.assertLess(manifest['stored_bytes'], 2000)
        source = result['sources']['catalog-series-attempt-01.json.gz']
        self.assertEqual(read_archived_response(self.root, 'catalog-series-attempt-01.json.gz', source), original)

    def test_resume_requests_only_remaining_pages_with_validated_cursor(self):
        first = self.client([{'series': [{'ticker': 'SERIES'}]},
                             {'markets': [market()], 'cursor': 'NEXT'}])
        with self.assertRaisesRegex(CatalogError, 'market_page_budget_exhausted'):
            acquire_catalog(self.root, client=first, max_market_pages=1, page_size=1)
        self.urls.clear()
        second = self.client([{'markets': [market('SERIES-EVENT-TWO')], 'cursor': ''}], resume=True)
        result = acquire_catalog(self.root, client=second, page_size=1)
        self.assertEqual(len(self.urls), 1)
        self.assertEqual(parse_qs(urlsplit(self.urls[0]).query)['cursor'], ['NEXT'])
        self.assertEqual(len(result['markets']), 2)
        self.assertEqual(result['coverage']['market_pages'], 2)
        self.assertEqual(result['coverage']['resumptions'][0]['retained_market_pages'], 1)
        self.assertEqual(result['coverage']['resumptions'][0]['previous_failure'], 'market_page_budget_exhausted')
        self.urls.clear()
        third = self.client([], resume=True)
        loaded = acquire_catalog(self.root, client=third, page_size=1)
        self.assertEqual(loaded, result)
        self.assertEqual(self.urls, [])

    def test_resume_rejects_changed_query_and_tampered_cursor(self):
        first = self.client([{'series': []}, {'markets': [market()], 'cursor': 'NEXT'}])
        with self.assertRaises(CatalogError):
            acquire_catalog(self.root, client=first, max_market_pages=1, page_size=1)
        second = self.client([], resume=True)
        with self.assertRaisesRegex(CatalogError, 'resumed_catalog_scope_mismatch'):
            acquire_catalog(self.root, client=second, page_size=2)
        manifest = self.read_manifest()
        request = manifest['requests'][-1]
        request['url'] = request['url'].replace('mve_filter=exclude', 'mve_filter=only')
        manifest['sources'][request['file']]['url'] = request['url']
        (self.root / 'catalog-manifest.json').write_text(json.dumps(manifest))
        third = self.client([], resume=True)
        with self.assertRaisesRegex(CatalogError, 'resumed_catalog_query_mismatch'):
            acquire_catalog(self.root, client=third, page_size=1)

    def test_corrupted_compressed_or_original_hash_prevents_resume(self):
        first = self.client([{'series': []}, {'markets': [], 'cursor': ''}])
        acquire_catalog(self.root, client=first)
        filename = next(iter(first.sources))
        original = (self.root / filename).read_bytes()
        (self.root / filename).write_bytes(original[:-1] + bytes([original[-1] ^ 1]))
        with self.assertRaisesRegex(CatalogError, 'archived_stored_hash_mismatch'):
            self.client([], resume=True)
        (self.root / filename).write_bytes(original)
        manifest = self.read_manifest()
        manifest['sources'][filename]['sha256'] = '0' * 64
        (self.root / 'catalog-manifest.json').write_text(json.dumps(manifest))
        with self.assertRaisesRegex(CatalogError, 'archived_original_hash_mismatch'):
            self.client([], resume=True)

    def test_legacy_archive_is_verified_then_migrated_and_resumed(self):
        first = self.client([{'series': []}, {'markets': [market()], 'cursor': 'NEXT'}])
        with self.assertRaises(CatalogError):
            acquire_catalog(self.root, client=first, page_size=1, max_market_pages=1)
        manifest = self.read_manifest()
        manifest['schema_version'] = 1
        for filename, source in list(manifest['sources'].items()):
            plain_name = filename[:-3]
            (self.root / plain_name).write_bytes(gzip.decompress((self.root / filename).read_bytes()))
            (self.root / filename).unlink()
            for field in ('stored_bytes', 'stored_sha256', 'encoding'):
                del source[field]
            manifest['sources'][plain_name] = source
            del manifest['sources'][filename]
            for request in manifest['requests']:
                if request.get('file') == filename:
                    request['file'] = plain_name
        (self.root / 'catalog-manifest.json').write_text(json.dumps(manifest))
        resumed = self.client([{'markets': [market('SERIES-EVENT-TWO')], 'cursor': ''}], resume=True)
        result = acquire_catalog(self.root, client=resumed, page_size=1)
        self.assertEqual(result['coverage']['market_count'], 2)
        self.assertTrue(all(name.endswith('.gz') for name in result['sources']))
        self.assertFalse((self.root / 'catalog-series-attempt-01.json').exists())

    def test_resumed_catalog_ignores_other_archived_public_queries(self):
        client = self.client([{'series': [{'ticker': 'SERIES'}]},
                              {'markets': [market()], 'cursor': ''},
                              {'series': {'ticker': 'SERIES'}},
                              {'markets': [market('SERIES-FRESH-ONE')], 'cursor': ''}])
        result = acquire_catalog(self.root, client=client)
        client.get_json('series/SERIES', 'fresh-series-SERIES')
        client.get_json('markets?status=open&series_ticker=SERIES&limit=1000', 'fresh-markets-SERIES')
        self.urls.clear()
        reloaded = acquire_catalog(self.root, client=self.client([], resume=True))
        self.assertEqual(reloaded['markets'], result['markets'])
        self.assertEqual(reloaded['coverage'], result['coverage'])
        self.assertEqual(self.urls, [])
        self.assertEqual(len(reloaded['sources']), 4)

    def test_compact_projection_preserves_coverage_and_full_archive(self):
        original = dict(market(), volume_24h_fp='15.25', rules_primary='Full reviewed rule',
                        yes_bid_dollars='0.30')
        client = self.client([{'series': []}, {'markets': [original], 'cursor': ''}])
        full = acquire_catalog(self.root, client=client)
        compact = acquire_catalog(self.root, client=self.client([], resume=True), compact_markets=True)
        self.assertEqual(compact['coverage'], full['coverage'])
        self.assertEqual(compact['record_projection'], ['ticker', 'event_ticker', 'volume_24h_fp'])
        self.assertEqual(compact['markets'], [{key: original[key] for key in compact['record_projection']}])
        self.assertEqual(full['markets'], [original])
        source_name = 'catalog-markets-0001-attempt-01.json.gz'
        archived = json.loads(read_archived_response(self.root, source_name, compact['sources'][source_name]))
        self.assertEqual(archived['markets'], [original])

    def test_compact_live_acquisition_still_checks_nonprojected_mve_field(self):
        client = self.client([{'series': []},
                              {'markets': [dict(market(), mve_collection_ticker='COMBO')], 'cursor': ''}])
        with self.assertRaisesRegex(CatalogError, 'mve_exclusion_not_respected'):
            acquire_catalog(self.root, client=client, compact_markets=True)


if __name__ == '__main__':
    unittest.main()
