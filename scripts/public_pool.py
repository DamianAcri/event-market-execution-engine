"""Bounded, persistent public HTTPS GET transport. No credentials or redirects.

Each worker owns its connections. Callers own their archives; do not share a
mutable CatalogClient between workers. A single limiter also covers retries.
Only the public market-data API and the pinned BTC contract URL are admitted.
Byte exhaustion stops queued starts and further reads; already pending reads
may add at most one 64 KiB chunk per active worker before they fail closed.
"""
import http.client
import math
import socket
import ssl
import threading
import time
from urllib.parse import urlsplit

from market_catalog import API, CatalogError, MAX_RESPONSE_BYTES, PublicResponse, _retry_after


class PublicPool:
    def __init__(self, *, workers=4, interval=0.2, max_decoded_bytes=256 * 1024 * 1024,
                 connection_factory=None, clock=time.monotonic, sleep=time.sleep):
        if (type(workers) is not int or not 1 <= workers <= 8 or
                not math.isfinite(interval) or interval < 0.2 or
                type(max_decoded_bytes) is not int or max_decoded_bytes <= 0):
            raise ValueError('invalid_public_pool_limits')
        self.workers, self.interval = workers, interval
        self.max_decoded_bytes = max_decoded_bytes
        self.clock, self.sleep = clock, sleep
        self.context = ssl.create_default_context() if connection_factory is None else None
        self.factory = connection_factory or (
            lambda host: http.client.HTTPSConnection(host, timeout=10, context=self.context))
        self.local = threading.local()
        self.lock = threading.Lock()
        self.active = threading.BoundedSemaphore(workers)
        self.connections, self.metrics = [], []
        self.next_start = 0.0
        self.decoded_bytes = 0
        self.byte_budget_exhausted = False
        self.closed = False

    def _pace(self):
        # Never hold the accounting lock during a cooldown: existing responses
        # must still be able to finish while another request waits to start.
        while True:
            with self.lock:
                self._check_available()
                now = self.clock()
                delay = max(0.0, self.next_start - now)
                if not delay:
                    self.next_start = now + self.interval
                    return
            self.sleep(delay)

    def _check_available(self):
        """Called with self.lock held."""
        if self.closed:
            raise CatalogError('public_pool_closed')
        if self.byte_budget_exhausted:
            raise CatalogError('public_total_byte_limit')

    @staticmethod
    def _target(url):
        parsed = urlsplit(url)
        if parsed.scheme != 'https' or parsed.username or parsed.password or parsed.port or parsed.fragment:
            raise CatalogError('endpoint_not_public_market_data')
        if parsed.netloc == 'assets.kalshi.com' and parsed.path == '/contract_terms/BTC.pdf' and not parsed.query:
            return parsed.netloc, parsed.path
        base = urlsplit(API)
        if parsed.netloc != base.netloc or not parsed.path.startswith(base.path):
            raise CatalogError('endpoint_not_public_market_data')
        relative = parsed.path[len(base.path):]
        import re
        if not re.fullmatch(r'(?:series(?:/[A-Za-z0-9_.:-]+)?|markets|markets/(?:orderbooks|trades)|'
                            r'markets/[A-Za-z0-9_.:-]+/orderbook|events/[A-Za-z0-9_.:-]+)', relative):
            raise CatalogError('endpoint_not_public_market_data')
        return parsed.netloc, parsed.path + ('?' + parsed.query if parsed.query else '')

    def __call__(self, url):
        host, target = self._target(url)
        with self.active:
            connections = getattr(self.local, 'connections', None)
            if connections is None:
                connections = self.local.connections = {}
            # A server may close an idle keepalive. One GET-only transport retry
            # reopens it; it is paced and separately recorded, never redirected.
            for retry in range(2):
                self._pace()
                start = self.clock()
                connection = connections.get(host)
                reused = connection is not None
                metric = {'url': url, 'transport_attempt': retry + 1,
                          'reused_connection': reused, 'connect_seconds': 0.0}
                deadline = start + 30
                expired = threading.Event()
                watchdog = None
                response = None

                def remaining_time():
                    remaining = deadline - self.clock()
                    if expired.is_set() or remaining <= 0:
                        raise CatalogError('public_request_timeout')
                    return remaining

                try:
                    if connection is None:
                        connection = self.factory(host)
                        connection.connect()
                        metric['connect_seconds'] = self.clock() - start
                        connections[host] = connection
                        with self.lock:
                            self.connections.append(connection)
                    # Keep the socket reference after http.client detaches it
                    # for Connection: close. A response file can still own that
                    # same descriptor while connection.sock is already None.
                    owned_socket = connection.sock
                    remaining = remaining_time()
                    owned_socket.settimeout(remaining)

                    def expire_request():
                        expired.set()
                        try:
                            owned_socket.shutdown(socket.SHUT_RDWR)
                        except OSError:
                            pass

                    # Socket timeouts alone are inactivity timeouts: a trickle
                    # of header/body bytes can keep resetting them. This single
                    # per-active-request watchdog interrupts established socket
                    # I/O at the absolute deadline. It is joined before reuse.
                    # System DNS resolution cannot be interrupted by stdlib;
                    # connect uses its 10s socket timeout and late completion is
                    # rejected before sending the request. Do not describe DNS
                    # or multi-address connect as hard wall-clock bounded.
                    watchdog = threading.Timer(remaining, expire_request)
                    watchdog.daemon = True
                    watchdog.start()
                    connection.request('GET', target, headers={
                        'Accept': '*/*', 'Accept-Encoding': 'identity',
                        'User-Agent': 'event-market-execution-engine-public-research/1'})
                    response = connection.getresponse()
                    remaining_time()
                    metric['time_to_headers_seconds'] = self.clock() - start
                    length = response.getheader('Content-Length')
                    if length is not None and (not length.isdigit() or int(length) > MAX_RESPONSE_BYTES):
                        raise CatalogError('public_response_size_limit')
                    if response.getheader('Content-Encoding', 'identity').lower() not in ('', 'identity'):
                        raise CatalogError('unexpected_public_content_encoding')
                    body = bytearray()
                    while True:
                        left = remaining_time()
                        # Content-Length is enforced by http.client. Avoid a
                        # redundant read on its already closed response/socket.
                        if length is not None and len(body) == int(length):
                            break
                        with self.lock:
                            self._check_available()
                            allowance = min(65536, self.max_decoded_bytes - self.decoded_bytes + 1,
                                            MAX_RESPONSE_BYTES + 1 - len(body))
                        owned_socket.settimeout(left)
                        chunk = response.read1(allowance)
                        remaining_time()
                        if not chunk:
                            break
                        body.extend(chunk)
                        with self.lock:
                            self.decoded_bytes += len(chunk)
                            if self.decoded_bytes >= self.max_decoded_bytes:
                                self.byte_budget_exhausted = True
                            if self.decoded_bytes > self.max_decoded_bytes:
                                raise CatalogError('public_total_byte_limit')
                        if len(body) > MAX_RESPONSE_BYTES:
                            raise CatalogError('public_response_size_limit')
                    if length is not None and len(body) != int(length):
                        raise CatalogError('truncated_public_response')
                    watchdog.cancel()
                    watchdog.join()
                    watchdog = None
                    remaining_time()
                    retry_after = _retry_after('Retry-After: ' + response.getheader('Retry-After')) if response.getheader('Retry-After') else None
                    metric.update(http_status=response.status, response_bytes=len(body))
                    if response.status == 429:
                        delay = retry_after if retry_after is not None else 1.0
                        if math.isfinite(delay) and 0 <= delay <= 30:
                            with self.lock:
                                self.next_start = max(self.next_start, self.clock() + max(0.2, delay))
                    if response.will_close:
                        connection.close()
                        connections.pop(host, None)
                    return PublicResponse(bytes(body), response.status, retry_after)
                except (OSError, http.client.HTTPException) as error:
                    metric['error'] = type(error).__name__
                    if connection is not None:
                        connection.close()
                    connections.pop(host, None)
                    if expired.is_set() or self.clock() >= deadline:
                        metric['error'] = 'public_request_timeout'
                        raise CatalogError('public_request_timeout') from None
                    if retry or not reused:
                        raise CatalogError('public_transport_failed') from None
                except CatalogError as error:
                    metric['error'] = str(error)
                    if connection is not None:
                        connection.close()
                    connections.pop(host, None)
                    raise
                finally:
                    if watchdog is not None:
                        watchdog.cancel()
                        watchdog.join()
                    if response is not None:
                        # In Python 3.9, read1() reaching Content-Length zero
                        # does not mark HTTPResponse closed. Release its file
                        # explicitly or the next getresponse() raises
                        # ResponseNotReady despite receiving the full body.
                        # Closing the response preserves a persistent socket;
                        # it also releases detached sockets on error paths.
                        response.close()
                    metric['elapsed_seconds'] = max(0.0, self.clock() - start)
                    with self.lock:
                        self.metrics.append(metric)
            raise CatalogError('public_transport_failed')

    def close(self):
        """Call after worker shutdown; never close another worker's active socket."""
        with self.lock:
            self.closed = True
            for connection in self.connections:
                connection.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()
