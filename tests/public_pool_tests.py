"""Public transport protocol tests with fake sockets; no network or credentials."""
from concurrent.futures import ThreadPoolExecutor
import http.client
import io
from pathlib import Path
import sys
import threading
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from market_catalog import API, CatalogError
from public_pool import PublicPool


class Clock:
    def __init__(self): self.now = 1.0
    def time(self): return self.now
    def sleep(self, delay): self.now += delay


class Response:
    def __init__(self, body=b'{}', status=200, headers=None, closes=False):
        self.body, self.status = body, status
        self.headers = headers if headers is not None else {'Content-Length': str(len(body))}
        self.will_close = closes
    def getheader(self, name, default=None): return self.headers.get(name, default)
    def read1(self, size):
        chunk, self.body = self.body[:size], self.body[size:]
        return chunk
    def close(self): pass


class Connection:
    def __init__(self, host, responses, calls):
        self.host, self.responses, self.calls = host, iter(responses), calls
        self.sock = self
        self.connected = self.closed = 0
    def connect(self): self.connected += 1
    def settimeout(self, value): assert 0 < value <= 30
    def request(self, method, target, headers):
        self.calls.append((self.host, method, target, headers, threading.get_ident()))
    def getresponse(self):
        value = next(self.responses)
        if isinstance(value, Exception): raise value
        return value
    def close(self): self.closed += 1
    def shutdown(self, _): self.closed += 1


class PublicPoolTests(unittest.TestCase):
    def pool(self, batches, **options):
        clock, connections, calls = Clock(), [], []
        batches = iter(batches)
        def factory(host):
            connection = Connection(host, next(batches), calls)
            connections.append(connection)
            return connection
        pool = PublicPool(connection_factory=factory, clock=clock.time, sleep=clock.sleep, **options)
        self.addCleanup(pool.close)
        return pool, connections, calls, clock

    def test_reuses_verified_get_connection_and_paces(self):
        pool, connections, calls, clock = self.pool([[Response(), Response()]])
        pool(API + 'series/KXBTC'); pool(API + 'markets?series_ticker=KXBTC')
        self.assertEqual(len(connections), 1)
        self.assertEqual(connections[0].connected, 1)
        self.assertAlmostEqual(clock.now, 1.2)
        self.assertEqual([m['reused_connection'] for m in pool.metrics], [False, True])
        for _, method, _, headers, _ in calls:
            self.assertEqual(method, 'GET')
            self.assertNotIn('Authorization', headers)
            self.assertNotIn('Cookie', headers)
            self.assertEqual(headers['Accept-Encoding'], 'identity')

    def test_real_http_client_consumes_and_closes_responses_before_reuse(self):
        """Exercise stdlib protocol state, not just our permissive fake responses."""
        packets = []
        expected = (b'', b'{}', b'x' * 131072, b'abc')
        for body in expected[:-1]:
            packets.append(b'HTTP/1.1 200 OK\r\nContent-Length: ' + str(len(body)).encode()
                           + b'\r\n\r\n' + body)
        packets.append(b'HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n')

        class MemorySocket:
            def __init__(self):
                self.packets = iter(packets)
                self.streams, self.requests = [], []
            def settimeout(self, _): pass
            def sendall(self, data): self.requests.append(data)
            def makefile(self, mode):
                self.streams.append(io.BytesIO(next(self.packets)))
                return self.streams[-1]
            def close(self): pass
            def shutdown(self, _): pass

        class MemoryHTTPConnection(http.client.HTTPConnection):
            def connect(self):
                self.sock = memory

        memory, clock, connections = MemorySocket(), Clock(), []
        def factory(host):
            connection = MemoryHTTPConnection(host)
            connections.append(connection)
            return connection
        with PublicPool(connection_factory=factory, clock=clock.time, sleep=clock.sleep) as pool:
            for body in expected:
                self.assertEqual(pool(API + 'markets').body, body)
                self.assertTrue(memory.streams[-1].closed)
            self.assertEqual(len(pool.metrics), len(expected))
            self.assertEqual([m['reused_connection'] for m in pool.metrics], [False, True, True, True])
            self.assertFalse(any('error' in metric for metric in pool.metrics))
        self.assertEqual(len(connections), 1)
        self.assertEqual(len(memory.requests), len(expected))

    def test_reconnects_once_on_closed_idle_socket(self):
        pool, connections, _, clock = self.pool([
            [Response(), http.client.RemoteDisconnected()], [Response(b'{"ok":true}')]])
        pool(API + 'markets')
        self.assertEqual(pool(API + 'markets').body, b'{"ok":true}')
        self.assertEqual(len(connections), 2)
        self.assertEqual(len(pool.metrics), 3)
        self.assertAlmostEqual(clock.now, 1.4)

    def test_no_retry_when_new_connection_fails(self):
        pool, _, calls, _ = self.pool([[http.client.RemoteDisconnected()]])
        with self.assertRaisesRegex(CatalogError, 'public_transport_failed'):
            pool(API + 'markets')
        self.assertEqual(len(calls), 1)

    def test_server_close_does_not_reuse_socket(self):
        pool, connections, _, _ = self.pool([[Response(closes=True)], [Response()]])
        pool(API + 'markets'); pool(API + 'markets')
        self.assertEqual(len(connections), 2)

    def test_only_public_market_endpoints_and_pinned_terms(self):
        pool, _, calls, _ = self.pool([])
        for url in (API + 'portfolio/orders', API + 'account/balance',
                    'http://external-api.kalshi.com/trade-api/v2/markets',
                    'https://external-api.kalshi.com.evil.test/trade-api/v2/markets',
                    'https://user:pass@external-api.kalshi.com/trade-api/v2/markets',
                    API + 'markets#fragment', API + '../portfolio/orders',
                    'https://assets.kalshi.com/contract_terms/OTHER.pdf'):
            with self.subTest(url=url), self.assertRaises((CatalogError, ValueError)):
                pool(url)
        self.assertFalse(calls)

    def test_redirect_is_returned_without_following(self):
        pool, _, calls, _ = self.pool([[Response(b'', 302, {'Location': 'https://example.com'})]])
        self.assertEqual(pool(API + 'markets').status, 302)
        self.assertEqual(len(calls), 1)

    def test_shared_retry_after_cooldown(self):
        pool, _, _, clock = self.pool([[Response(b'', 429, {'Retry-After': '3'}), Response()]])
        self.assertEqual(pool(API + 'markets').retry_after_seconds, 3)
        pool(API + 'markets')
        self.assertAlmostEqual(clock.now, 4)

    def test_decoded_total_budget_applies_across_responses(self):
        pool, _, _, _ = self.pool([[Response(b'1234'), Response(b'5678')]], max_decoded_bytes=7)
        pool(API + 'markets')
        with self.assertRaisesRegex(CatalogError, 'public_total_byte_limit'):
            pool(API + 'markets')

    def test_exhausted_byte_budget_stops_all_later_requests(self):
        pool, connections, calls, _ = self.pool([[Response(b'1234'), Response(b'5678')]],
                                                max_decoded_bytes=7)
        pool(API + 'markets')
        for _ in range(3):
            with self.assertRaisesRegex(CatalogError, 'public_total_byte_limit'):
                pool(API + 'markets')
        self.assertEqual(pool.decoded_bytes, 8)
        self.assertEqual(len(calls), 2)
        self.assertEqual(len(connections), 1)

    def test_exact_byte_budget_accepts_complete_response_then_stops(self):
        pool, _, calls, _ = self.pool([[Response(b'1234')]], max_decoded_bytes=4)
        self.assertEqual(pool(API + 'markets').body, b'1234')
        with self.assertRaisesRegex(CatalogError, 'public_total_byte_limit'):
            pool(API + 'markets')
        self.assertEqual(len(calls), 1)

    def test_late_eof_after_connection_close_is_not_accepted(self):
        clock = Clock()
        class ClosingResponse(Response):
            def read1(self, _size):
                clock.sleep(2)
                return b''
        class ClosingConnection(Connection):
            def getresponse(self):
                clock.sleep(29)
                # Match http.client: the response file still owns the socket,
                # but the connection no longer exposes it after these headers.
                self.sock = None
                return ClosingResponse(headers={'Connection': 'close'}, closes=True)
        connection = ClosingConnection('host', [], [])
        with PublicPool(connection_factory=lambda _: connection, clock=clock.time, sleep=clock.sleep) as pool:
            with self.assertRaisesRegex(CatalogError, 'public_request_timeout'):
                pool(API + 'markets')
            self.assertEqual(pool.metrics[0]['elapsed_seconds'], 31)

    def test_headers_completing_after_deadline_are_rejected(self):
        clock = Clock()
        class SlowHeaders(Connection):
            def getresponse(self):
                clock.sleep(31)
                return Response(b'')
        connection = SlowHeaders('host', [], [])
        with PublicPool(connection_factory=lambda _: connection, clock=clock.time, sleep=clock.sleep) as pool:
            with self.assertRaisesRegex(CatalogError, 'public_request_timeout'):
                pool(API + 'markets')

    def test_late_dns_or_connect_completion_never_sends_get(self):
        clock, calls = Clock(), []
        class SlowConnect(Connection):
            def connect(self):
                clock.sleep(31)
                super().connect()
        connection = SlowConnect('host', [Response()], calls)
        with PublicPool(connection_factory=lambda _: connection, clock=clock.time, sleep=clock.sleep) as pool:
            with self.assertRaisesRegex(CatalogError, 'public_request_timeout'):
                pool(API + 'markets')
        self.assertFalse(calls)

    def test_watchdog_interrupts_headers_and_is_joined_before_return(self):
        timers = []
        class Timer:
            def __init__(self, delay, callback):
                self.delay, self.callback = delay, callback
                self.cancelled = self.joined = False
                timers.append(self)
            def start(self): pass
            def cancel(self): self.cancelled = True
            def join(self): self.joined = True
        class TrickleHeaders(Connection):
            def getresponse(self):
                timers[-1].callback()
                raise http.client.RemoteDisconnected()
        connection = TrickleHeaders('host', [], [])
        with patch('public_pool.threading.Timer', Timer), PublicPool(connection_factory=lambda _: connection) as pool:
            with self.assertRaisesRegex(CatalogError, 'public_request_timeout'):
                pool(API + 'markets')
        self.assertEqual(len(timers), 1)
        self.assertTrue(timers[0].cancelled and timers[0].joined)
        self.assertGreaterEqual(connection.closed, 2)  # watchdog shutdown plus owner close

    def test_completed_watchdog_is_cancelled_and_joined_before_connection_reuse(self):
        timers = []
        class Timer:
            def __init__(self, delay, callback):
                self.cancelled = self.joined = False
                timers.append(self)
            def start(self):
                if len(timers) > 1:
                    assert timers[-2].cancelled and timers[-2].joined
            def cancel(self): self.cancelled = True
            def join(self): self.joined = True
        pool, connections, _, _ = self.pool([[Response(), Response()]])
        with patch('public_pool.threading.Timer', Timer):
            pool(API + 'markets'); pool(API + 'markets')
        self.assertEqual(len(connections), 1)
        self.assertEqual(len(timers), 2)
        self.assertTrue(all(timer.cancelled and timer.joined for timer in timers))

    def test_retry_after_wait_does_not_hold_body_accounting_lock(self):
        pool, _, _, clock = self.pool([[Response(b'', 429, {'Retry-After': '3'})]])
        pool(API + 'markets')
        waiting, release = threading.Event(), threading.Event()
        def sleep(delay):
            waiting.set()
            if not release.wait(timeout=2):
                raise AssertionError('pace wait was not released')
            clock.sleep(delay)
        pool.sleep = sleep
        with ThreadPoolExecutor(max_workers=1) as executor:
            future = executor.submit(pool._pace)
            try:
                self.assertTrue(waiting.wait(timeout=1))
                acquired = pool.lock.acquire(timeout=0.2)
                try:
                    self.assertTrue(acquired, 'cooldown blocks in-flight body accounting')
                finally:
                    if acquired:
                        pool.lock.release()
            finally:
                release.set()
            future.result(timeout=2)

    def test_truncated_oversized_or_encoded_responses_fail(self):
        for response, reason in ((Response(b'x', headers={'Content-Length': '2'}), 'truncated'),
            (Response(headers={'Content-Length': str(40*1024*1024)}), 'size_limit'),
            (Response(headers={'Content-Encoding': 'gzip'}), 'content_encoding')):
            pool, _, _, _ = self.pool([[response]])
            with self.subTest(reason=reason), self.assertRaisesRegex(CatalogError, reason):
                pool(API + 'markets')

    def test_closed_pool_cannot_start_a_request(self):
        pool, _, calls, _ = self.pool([])
        pool.close()
        with self.assertRaisesRegex(CatalogError, 'public_pool_closed'):
            pool(API + 'markets')
        self.assertFalse(calls)

    def test_worker_connections_have_one_owner(self):
        gate = threading.Barrier(2)
        calls, created = [], []
        def factory(host):
            connection = Connection(host, [Response(), Response()], calls)
            created.append(connection)
            return connection
        with PublicPool(workers=2, connection_factory=factory) as pool:
            def work():
                gate.wait(timeout=5)
                pool(API + 'markets'); pool(API + 'series/KXBTC')
            with ThreadPoolExecutor(max_workers=2) as executor:
                futures = [executor.submit(work) for _ in range(2)]
                for future in futures: future.result()
        self.assertEqual(len(created), 2)
        self.assertEqual(len({call[-1] for call in calls}), 2)
        self.assertEqual(sum(m['reused_connection'] for m in pool.metrics), 2)


if __name__ == '__main__':
    unittest.main()
