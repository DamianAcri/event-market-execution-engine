"""Read-only, bounded public catalog discovery with an auditable request archive.

Markets are the paginated ``status=open&mve_filter=exclude`` API universe, not
every venue instrument. Pagination is not an atomic snapshot: a market may open,
close or change while the scan runs. Successful completion means that the API
cursor was exhausted, not that those races have been eliminated.

API contracts checked 2026-09-17:
https://docs.kalshi.com/api-reference/market/get-markets
https://docs.kalshi.com/api-reference/market/get-series-list
"""
import datetime as dt
import email.utils
import gzip
import hashlib
import io
import json
import math
from pathlib import Path
import re
import subprocess
import tempfile
import time
from typing import NamedTuple, Optional
from urllib.parse import parse_qs, urlencode, urlsplit


API = 'https://external-api.kalshi.com/trade-api/v2/'
MAX_RESPONSE_BYTES = 32 * 1024 * 1024
MAX_ARCHIVE_BYTES = 256 * 1024 * 1024
MAX_DECODED_BYTES = 1024 * 1024 * 1024


class CatalogError(Exception):
    """Fixed diagnostic code; never includes response bodies or credentials."""


class PublicResponse(NamedTuple):
    body: bytes
    status: int = 200
    retry_after_seconds: Optional[float] = None


def _utcnow():
    return dt.datetime.now(dt.timezone.utc)


def _timestamp(clock):
    value = clock()
    if value.tzinfo is None:
        raise ValueError('archive clock must be timezone aware')
    return value.astimezone(dt.timezone.utc).isoformat().replace('+00:00', 'Z')


def _retry_after(headers):
    values = [line.partition(':')[2].strip() for line in headers.splitlines()
              if line.lower().startswith('retry-after:')]
    if not values:
        return None
    value = values[-1]
    try:
        result = float(value)
    except ValueError:
        try:
            deadline = email.utils.parsedate_to_datetime(value)
            result = (deadline - _utcnow()).total_seconds()
        except (TypeError, ValueError, OverflowError):
            raise CatalogError('invalid_retry_after') from None
    if not math.isfinite(result) or result < 0:
        raise CatalogError('invalid_retry_after')
    return result


def public_get(url):
    """GET using existing curl only. No redirect, curlrc, cookies or API auth."""
    with tempfile.TemporaryDirectory(prefix='eme-public-') as folder:
        headers = Path(folder) / 'headers'
        body = Path(folder) / 'body'
        result = subprocess.run([
            'curl', '--disable', '--silent', '--show-error', '--request', 'GET',
            '--proto', '=https', '--max-time', '30', '--connect-timeout', '10',
            '--max-filesize', str(MAX_RESPONSE_BYTES), '--dump-header', str(headers),
            '--output', str(body), '--write-out', '%{http_code}', url],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, check=False)
        if result.returncode or not body.is_file() or body.stat().st_size > MAX_RESPONSE_BYTES:
            raise CatalogError('public_transport_failed')
        try:
            status = int(result.stdout)
        except ValueError:
            raise CatalogError('invalid_http_status') from None
        retry = _retry_after(headers.read_text(encoding='iso-8859-1'))
        return PublicResponse(body.read_bytes(), status, retry)


def _json_object(raw):
    def unique_pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise CatalogError('duplicate_json_key')
            result[key] = value
        return result

    def invalid_constant(_):
        raise CatalogError('nonfinite_json_number')

    try:
        value = json.loads(raw, object_pairs_hook=unique_pairs, parse_constant=invalid_constant)
    except (ValueError, UnicodeError, RecursionError):
        raise CatalogError('invalid_public_json') from None
    if not isinstance(value, dict):
        raise CatalogError('invalid_public_object')
    return value


def read_archived_response(root, filename, source):
    """Verify original and stored hashes; decode legacy plain or gzip responses."""
    if not re.fullmatch(r'[A-Za-z0-9_-]+\.json(?:\.gz)?', filename):
        raise CatalogError('invalid_archived_filename')
    path = Path(root) / filename
    if path.is_symlink() or not path.is_file():
        raise CatalogError('missing_or_unsafe_archived_file')
    size = source.get('response_bytes')
    if type(size) is not int or not 0 <= size <= MAX_RESPONSE_BYTES:
        raise CatalogError('invalid_archived_response_size')
    # A gzip stream can be slightly larger than the input, even at level zero.
    if path.stat().st_size > MAX_RESPONSE_BYTES + 65536:
        raise CatalogError('archived_file_too_large')
    stored = path.read_bytes()
    encoding = source.get('encoding', 'identity')
    if encoding == 'gzip':
        if (not filename.endswith('.gz') or len(stored) != source.get('stored_bytes') or
                hashlib.sha256(stored).hexdigest() != source.get('stored_sha256')):
            raise CatalogError('archived_stored_hash_mismatch')
        try:
            with gzip.GzipFile(fileobj=io.BytesIO(stored), mode='rb') as stream:
                decoded = stream.read(size + 1)
        except (OSError, EOFError):
            raise CatalogError('invalid_archived_gzip') from None
    elif encoding == 'identity' and not filename.endswith('.gz'):
        decoded = stored
    else:
        raise CatalogError('invalid_archive_encoding')
    if len(decoded) != size or hashlib.sha256(decoded).hexdigest() != source.get('sha256'):
        raise CatalogError('archived_original_hash_mismatch')
    return decoded


class CatalogClient:
    """Paced public market GETs; injectable transport and clocks for offline tests.

    ``public_get`` accepts one URL and returns bytes (successful synthetic or
    existing transports) or PublicResponse (preserves HTTP status/Retry-After).
    ``writer`` accepts (Path, bytes); it is used for raw responses and manifests.
    Use resume=True to reopen, verify and losslessly migrate an older archive.
    ``get_json`` names must be unique within an archive.
    """
    def __init__(self, root, public_get=None, *, max_retries=2, sleep=time.sleep,
                 monotonic=time.monotonic, utcnow=_utcnow, writer=None,
                 max_archive_bytes=MAX_ARCHIVE_BYTES, max_decoded_bytes=MAX_DECODED_BYTES,
                 resume=False):
        if (type(max_retries) is not int or not 0 <= max_retries <= 5 or
                type(max_archive_bytes) is not int or max_archive_bytes <= 0 or
                type(max_decoded_bytes) is not int or max_decoded_bytes <= 0):
            raise ValueError('invalid catalog request budget')
        self.root = Path(root)
        self.root.mkdir(parents=True, exist_ok=True)
        self.manifest_path = self.root / 'catalog-manifest.json'
        if self.manifest_path.exists() and not resume:
            raise CatalogError('archive_already_exists')
        self.transport = public_get or globals()['public_get']
        self.max_retries = max_retries
        self.sleep, self.monotonic, self.utcnow = sleep, monotonic, utcnow
        self.writer = writer or (lambda path, value: path.write_bytes(value))
        self.custom_writer = writer is not None
        self.max_archive_bytes = max_archive_bytes
        self.max_decoded_bytes = max_decoded_bytes
        self.resume_enabled = resume
        self.last_start = None
        self.raw_bytes = 0
        self.stored_bytes = 0
        self.names = set()
        self.sources = {}
        self.manifest = {'schema_version': 2, 'started_at': _timestamp(utcnow),
                         'request_policy': {'minimum_start_interval_seconds': 0.2,
                                            'maximum_retries_429': max_retries,
                                            'maximum_retry_after_seconds': 30,
                                            'maximum_response_bytes': MAX_RESPONSE_BYTES,
                                            'maximum_archive_bytes': max_archive_bytes,
                                            'maximum_decoded_bytes': max_decoded_bytes},
                         'requests': [], 'sources': self.sources}
        self.requests = self.manifest['requests']
        if resume:
            self._resume()
        self.save()

    def save(self):
        content = (json.dumps(self.manifest, indent=2) + '\n').encode()
        if self.custom_writer:
            self.writer(self.manifest_path, content)
        else:
            temporary = self.manifest_path.with_suffix('.json.tmp')
            self.writer(temporary, content)
            temporary.replace(self.manifest_path)

    def _resume(self):
        if not self.manifest_path.is_file() or self.manifest_path.is_symlink():
            raise CatalogError('missing_or_unsafe_archive_manifest')
        self.manifest = _json_object(self.manifest_path.read_bytes())
        if (self.manifest.get('schema_version') not in (1, 2) or
                not isinstance(self.manifest.get('sources'), dict) or
                not isinstance(self.manifest.get('requests'), list)):
            raise CatalogError('invalid_archive_manifest')
        self.sources, self.requests = self.manifest['sources'], self.manifest['requests']
        # Validate every original response before mutating any part of the archive.
        for filename, source in self.sources.items():
            if not isinstance(source, dict):
                raise CatalogError('invalid_archive_source')
            read_archived_response(self.root, filename, source)
            self.raw_bytes += source['response_bytes']
            self.stored_bytes += (self.root / filename).stat().st_size
        if self.raw_bytes > self.max_decoded_bytes:
            raise CatalogError('archive_decoded_budget_exceeded')
        for request in self.requests:
            if not isinstance(request, dict):
                raise CatalogError('invalid_archived_request')
            filename = request.get('file')
            if filename:
                source = self.sources.get(filename)
                if (not source or source.get('url') != request.get('url') or
                        source.get('sha256') != request.get('sha256') or
                        source.get('http_status') != request.get('http_status')):
                    raise CatalogError('archived_request_source_mismatch')
                self.names.add(re.sub(r'-attempt-[0-9]+\.json(?:\.gz)?$', '', filename))
        self.manifest['schema_version'] = 2
        self.manifest['request_policy'].update({
            'maximum_archive_bytes': self.max_archive_bytes,
            'maximum_decoded_bytes': self.max_decoded_bytes})
        # Each migration commits the gzip reference before removing its verified
        # legacy duplicate. An interrupted migration retains at least one copy.
        for filename, source in list(self.sources.items()):
            if source.get('encoding', 'identity') != 'identity':
                continue
            body = read_archived_response(self.root, filename, source)
            stored = gzip.compress(body, compresslevel=6, mtime=0)
            new_name = filename + '.gz'
            new_path = self.root / new_name
            if new_path.exists():
                if new_path.is_symlink() or new_path.read_bytes() != stored:
                    raise CatalogError('archive_migration_file_conflict')
            else:
                self.writer(new_path, stored)
            updated = dict(source, encoding='gzip', stored_bytes=len(stored),
                           stored_sha256=hashlib.sha256(stored).hexdigest())
            read_archived_response(self.root, new_name, updated)
            self.sources[new_name] = updated
            del self.sources[filename]
            for request in self.requests:
                if request.get('file') == filename:
                    request['file'] = new_name
            self.stored_bytes += len(stored) - len(body)
            self._update_sizes()
            self.save()
            (self.root / filename).unlink()
        if self.stored_bytes > self.max_archive_bytes:
            raise CatalogError('archive_byte_budget_exceeded')
        self._update_sizes()

    def _update_sizes(self):
        self.manifest.update(raw_bytes=self.raw_bytes, decoded_bytes=self.raw_bytes,
                             stored_bytes=self.stored_bytes)

    def get_json(self, relative_path, name):
        """Archive and parse one allowed public endpoint, retrying only HTTP 429."""
        split = urlsplit(relative_path)
        if (split.scheme or split.netloc or split.fragment or
                not re.fullmatch(r'(?:series(?:/[A-Za-z0-9_.:-]+)?|markets|'
                                 r'markets/(?:orderbooks|trades)|'
                                 r'markets/[A-Za-z0-9_.:-]+/orderbook|'
                                 r'events/[A-Za-z0-9_.:-]+)', split.path)):
            raise CatalogError('endpoint_not_public_market_data')
        if not re.fullmatch(r'[A-Za-z0-9_-]{1,96}', name) or name in self.names:
            raise CatalogError('invalid_or_reused_archive_name')
        self.names.add(name)
        url = API + relative_path
        for attempt in range(self.max_retries + 1):
            if self.last_start is not None:
                self.sleep(max(0.0, 0.2 - (self.monotonic() - self.last_start)))
            start = self.monotonic()
            self.last_start = start
            request = {'url': url, 'attempt': attempt + 1,
                       'requested_at': _timestamp(self.utcnow)}
            self.manifest['requests'].append(request)
            self.save()
            try:
                response = self.transport(url)
            except Exception:
                request.update({'completed_at': _timestamp(self.utcnow),
                                'elapsed_seconds': max(0.0, self.monotonic() - start),
                                'error': 'public_transport_failed'})
                self.save()
                raise CatalogError('public_transport_failed') from None
            request.update({'completed_at': _timestamp(self.utcnow),
                            'elapsed_seconds': max(0.0, self.monotonic() - start)})
            if isinstance(response, bytes):
                response = PublicResponse(response)
            if (not isinstance(response, PublicResponse) or
                    type(response.status) is not int or not 100 <= response.status <= 599 or
                    not isinstance(response.body, bytes)):
                request['error'] = 'invalid_transport_response'
                self.save()
                raise CatalogError('invalid_transport_response')
            request['http_status'] = response.status
            request['response_bytes'] = len(response.body)
            if len(response.body) > MAX_RESPONSE_BYTES or self.raw_bytes + len(response.body) > self.max_decoded_bytes:
                request['error'] = 'archive_decoded_budget_exceeded'
                self.save()
                raise CatalogError('archive_decoded_budget_exceeded')
            stored = gzip.compress(response.body, compresslevel=6, mtime=0)
            if self.stored_bytes + len(stored) > self.max_archive_bytes:
                request['error'] = 'archive_byte_budget_exceeded'
                self.save()
                raise CatalogError('archive_byte_budget_exceeded')
            filename = '{}-attempt-{:02d}.json.gz'.format(name, attempt + 1)
            path = self.root / filename
            if path.exists():
                raise CatalogError('archive_file_already_exists')
            self.writer(path, stored)
            self.raw_bytes += len(response.body)
            self.stored_bytes += len(stored)
            source = dict(request, sha256=hashlib.sha256(response.body).hexdigest(),
                          encoding='gzip', stored_bytes=len(stored),
                          stored_sha256=hashlib.sha256(stored).hexdigest())
            request['file'] = filename
            request['sha256'] = source['sha256']
            self.sources[filename] = source
            self._update_sizes()
            self.save()
            if response.status == 200:
                try:
                    return _json_object(response.body)
                except CatalogError as error:
                    request['error'] = str(error)
                    self.save()
                    raise
            if response.status != 429:
                request['error'] = 'public_http_error'
                self.save()
                raise CatalogError('public_http_error')
            delay = response.retry_after_seconds
            if delay is None:
                delay = 1.0 * (2 ** attempt)
            if (isinstance(delay, bool) or not isinstance(delay, (float, int)) or
                    not math.isfinite(delay) or not 0 <= delay <= 30):
                request['error'] = 'retry_after_outside_budget'
                self.save()
                raise CatalogError('retry_after_outside_budget')
            if attempt == self.max_retries:
                request['error'] = 'public_rate_limit_exhausted'
                self.save()
                raise CatalogError('public_rate_limit_exhausted')
            request['retry_delay_seconds'] = max(0.2, delay)
            self.save()
            self.sleep(max(0.2, delay))
        raise AssertionError('unreachable request retry')


def _records(payload, key, seen):
    items = payload.get(key)
    if not isinstance(items, list):
        raise CatalogError('invalid_' + key + '_list')
    for item in items:
        if (not isinstance(item, dict) or not isinstance(item.get('ticker'), str) or
                not re.fullmatch(r'[A-Za-z0-9_.:-]{1,256}', item['ticker'])):
            raise CatalogError('invalid_' + key + '_ticker')
        if item['ticker'] in seen:
            raise CatalogError('duplicate_' + key + '_ticker')
        seen.add(item['ticker'])
        if key == 'markets':
            if not isinstance(item.get('event_ticker'), str) or not item['event_ticker']:
                raise CatalogError('invalid_market_event')
            if item.get('mve_collection_ticker') or item.get('mve_selected_legs'):
                raise CatalogError('mve_exclusion_not_respected')
    return items


def _series_records(payload):
    if payload.get('cursor') not in (None, ''):
        raise CatalogError('unexpected_series_pagination')
    return _records(payload, 'series', set())


def _market_page(payload, page_size, seen_tickers, seen_cursors):
    if 'cursor' not in payload or not isinstance(payload['cursor'], str):
        raise CatalogError('missing_or_invalid_market_cursor')
    items = _records(payload, 'markets', seen_tickers)
    if len(items) > page_size:
        raise CatalogError('market_page_exceeds_limit')
    cursor = payload['cursor']
    if cursor and (not items or cursor in seen_cursors or len(cursor) > 8192):
        raise CatalogError('nonprogressing_market_cursor')
    if cursor:
        seen_cursors.add(cursor)
    return items, cursor


def _project_markets(items, compact):
    if not compact:
        return items
    fields = ('ticker', 'event_ticker', 'volume_24h_fp')
    return [{field: item[field] for field in fields if field in item} for item in items]


def _catalog_result(archive, series, markets, coverage, compact):
    return {'markets': markets, 'series': series, 'coverage': coverage, 'sources': archive.sources,
            'record_projection': ['ticker', 'event_ticker', 'volume_24h_fp'] if compact else 'full'}


def _saved_catalog(archive, coverage, page_size, compact_markets):
    if (coverage.get('scope') != 'open_markets_excluding_multivariate' or
            coverage.get('filters') != {'status': 'open', 'mve_filter': 'exclude'} or
            coverage.get('series_scope') != 'unfiltered_series_list' or
            coverage.get('snapshot_atomic') is not False or coverage.get('page_size') != page_size):
        raise CatalogError('resumed_catalog_scope_mismatch')
    resumable = {'archive_byte_budget_exceeded', 'archive_decoded_budget_exceeded',
                 'market_page_budget_exhausted', 'public_transport_failed',
                 'public_http_error', 'public_rate_limit_exhausted', 'retry_after_outside_budget'}
    if coverage.get('failure') and coverage['failure'] not in resumable:
        raise CatalogError('catalog_failure_not_resumable')
    series, markets, seen_tickers, seen_cursors, cursor, pages = None, [], set(), set(), '', 0
    for request in archive.requests:
        filename = request.get('file')
        if request.get('http_status') != 200 or not filename:
            continue
        url = request.get('url', '')
        is_catalog_series = re.fullmatch(r'catalog-series(?:-resume-[0-9]+)?-attempt-[0-9]+\.json(?:\.gz)?', filename)
        is_catalog_market = re.fullmatch(r'catalog-markets-[0-9]+(?:-resume-[0-9]+)?-attempt-[0-9]+\.json(?:\.gz)?', filename)
        if is_catalog_series:
            if url != API + 'series':
                raise CatalogError('resumed_catalog_query_mismatch')
            if series is not None:
                raise CatalogError('duplicate_archived_series_response')
            series = _series_records(_json_object(read_archived_response(
                archive.root, filename, archive.sources[filename])))
        elif is_catalog_market:
            if not url.startswith(API + 'markets?') or (pages and not cursor):
                raise CatalogError('resumed_catalog_query_mismatch')
            expected = {'status': ['open'], 'mve_filter': ['exclude'], 'limit': [str(page_size)]}
            if cursor:
                expected['cursor'] = [cursor]
            if parse_qs(urlsplit(url).query, keep_blank_values=True) != expected:
                raise CatalogError('resumed_catalog_query_mismatch')
            payload = _json_object(read_archived_response(archive.root, filename, archive.sources[filename]))
            items, cursor = _market_page(payload, page_size, seen_tickers, seen_cursors)
            markets.extend(_project_markets(items, compact_markets))
            pages += 1
    if (len(markets) != coverage.get('market_count') or pages != coverage.get('market_pages') or
            len(series or []) != coverage.get('series_count') or
            (coverage.get('complete') and (not pages or cursor))):
        raise CatalogError('resumed_catalog_counts_mismatch')
    return series, markets, seen_tickers, seen_cursors, cursor, pages


def acquire_catalog(root, public_get=None, *, max_market_pages=500, page_size=1000,
                    max_retries=2, sleep=time.sleep, monotonic=time.monotonic,
                    utcnow=_utcnow, writer=None, client=None, resume=False,
                    compact_markets=False):
    """Return broad public records only after complete, validated pagination.

    Failure preserves catalog-manifest.json with complete=false, a fixed failure
    code, successfully traversed counts, and every received raw response within
    the archive byte budget. Partial data is never returned as a complete result.
    The unpaginated /series response is checked separately from /markets.
    compact_markets keeps only identity and 24h activity in returned market
    records after full validation; original complete responses remain archived.
    The top-level record_projection value is 'full' or the retained field list.
    """
    if (type(max_market_pages) is not int or not 1 <= max_market_pages <= 1000 or
            type(page_size) is not int or not 1 <= page_size <= 1000):
        raise ValueError('invalid catalog pagination budget')
    archive = client or CatalogClient(root, public_get, max_retries=max_retries,
                                      sleep=sleep, monotonic=monotonic, utcnow=utcnow,
                                      writer=writer, resume=resume)
    if Path(root).resolve() != archive.root.resolve():
        raise CatalogError('catalog_archive_mismatch_or_reuse')
    saved = archive.manifest.get('catalog')
    if saved and not archive.resume_enabled:
        raise CatalogError('catalog_archive_mismatch_or_reuse')
    coverage = {'complete': False, 'scope': 'open_markets_excluding_multivariate',
                'filters': {'status': 'open', 'mve_filter': 'exclude'},
                'series_scope': 'unfiltered_series_list',
                'snapshot_atomic': False,
                'snapshot_limitation': 'Markets may open, close or change during cursor traversal; '
                                       'completion is API cursor exhaustion, not a point-in-time venue snapshot.',
                'max_market_pages': max_market_pages, 'page_size': page_size,
                'market_pages': 0, 'market_count': 0, 'series_count': 0,
                'started_at': _timestamp(archive.utcnow)}
    if saved:
        coverage = saved
        series, markets, seen_tickers, seen_cursors, cursor, pages = _saved_catalog(
            archive, coverage, page_size, compact_markets)
        if coverage['complete']:
            return _catalog_result(archive, series, markets, coverage, compact_markets)
        coverage.setdefault('resumptions', []).append({
            'resumed_at': _timestamp(archive.utcnow), 'previous_failure': coverage.pop('failure', None),
            'previous_completed_at': coverage.pop('completed_at', None), 'retained_market_pages': pages})
        coverage['max_market_pages'] = max_market_pages
    else:
        series, markets, seen_tickers, seen_cursors, cursor, pages = None, [], set(), set(), '', 0
    archive.manifest['catalog'] = coverage
    archive.save()

    def fresh_name(base):
        suffix = 0
        name = base
        while name in archive.names:
            suffix += 1
            name = base + '-resume-' + str(suffix)
        return name

    try:
        if series is None:
            series = _series_records(archive.get_json('series', fresh_name('catalog-series')))
        coverage['series_count'] = len(series)
        # A prior interruption can happen after persisting the terminal page but
        # before marking completion. The validated empty cursor is sufficient.
        if pages and not cursor:
            coverage['complete'] = True
            coverage['completed_at'] = _timestamp(archive.utcnow)
            archive.save()
            return _catalog_result(archive, series, markets, coverage, compact_markets)
        for page in range(pages, max_market_pages):
            query = dict(coverage['filters'], limit=page_size)
            if cursor:
                query['cursor'] = cursor
            payload = archive.get_json('markets?' + urlencode(query),
                                       fresh_name('catalog-markets-{:04d}'.format(page + 1)))
            items, cursor = _market_page(payload, page_size, seen_tickers, seen_cursors)
            markets.extend(_project_markets(items, compact_markets))
            coverage['market_pages'] = page + 1
            coverage['market_count'] = len(markets)
            archive.save()
            if not cursor:
                coverage['complete'] = True
                coverage['completed_at'] = _timestamp(archive.utcnow)
                archive.save()
                return _catalog_result(archive, series, markets, coverage, compact_markets)
        raise CatalogError('market_page_budget_exhausted')
    except CatalogError as error:
        coverage['failure'] = str(error)
        coverage['completed_at'] = _timestamp(archive.utcnow)
        archive.save()
        raise
