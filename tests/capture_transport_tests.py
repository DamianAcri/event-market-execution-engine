"""Independent stdlib TLS/WS fixture. Never contacts Kalshi or uses user keys."""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import socket
import ssl
import struct
import subprocess
import tempfile
import threading
import time
import sys


def main():
    parser = argparse.ArgumentParser()
    for arg in ('client', 'engine', 'openssl', 'metadata'):
        parser.add_argument('--' + arg, required=True)
    args = parser.parse_args()
    selected_tickers = [m["ticker"] for m in sorted(json.loads(Path(args.metadata).read_text())["markets"], key=lambda m: m["id"]) if m["id"] in (1, 2)]
    with tempfile.TemporaryDirectory(prefix='eme-tls-fixture-') as directory:
        root = Path(directory)
        key, cert, pub = (root / name for name in ('key.pem', 'cert.pem', 'public.pem'))
        def openssl(*command):
            return subprocess.run([args.openssl, *map(str, command)], check=True, capture_output=True).stdout
        openssl('genpkey', '-algorithm', 'RSA', '-pkeyopt', 'rsa_keygen_bits:2048', '-out', key)
        config = root / 'cert.cnf'
        config.write_text('[req]\ndistinguished_name=dn\nx509_extensions=ext\nprompt=no\n[dn]\nCN=localhost\n[ext]\nsubjectAltName=IP:127.0.0.1\nbasicConstraints=critical,CA:TRUE\n')
        openssl('req', '-new', '-x509', '-key', key, '-out', cert, '-days', '1', '-sha256', '-config', config)
        openssl('pkey', '-in', key, '-pubout', '-out', pub)
        wrong_key, wrong_cert = root / 'wrong-key.pem', root / 'wrong-cert.pem'
        openssl('genpkey', '-algorithm', 'RSA', '-pkeyopt', 'rsa_keygen_bits:2048', '-out', wrong_key)
        openssl('req', '-new', '-x509', '-key', wrong_key, '-out', wrong_cert, '-days', '1', '-sha256', '-config', config)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        # Trusted certificate with a deliberately different IP tests hostname
        # verification independently of trust and DNS/address-family fallback.
        mismatch_config, mismatch_cert = root / 'mismatch.cnf', root / 'mismatch.pem'
        mismatch_config.write_text(config.read_text().replace('IP:127.0.0.1', 'IP:127.0.0.2'))
        openssl('req', '-new', '-x509', '-key', key, '-out', mismatch_cert, '-days', '1', '-sha256', '-config', mismatch_config)
        mismatch_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        mismatch_context.load_cert_chain(mismatch_cert, key)

        def exact(sock, count):
            result = b''
            while len(result) < count:
                part = sock.recv(count - len(result))
                if not part:
                    raise EOFError()
                result += part
            return result

        def read_frame(sock):
            first, second = exact(sock, 2)
            length = second & 127
            if length == 126:
                length = struct.unpack('!H', exact(sock, 2))[0]
            elif length == 127:
                length = struct.unpack('!Q', exact(sock, 8))[0]
            assert second & 128, 'client messages must be masked'
            mask = exact(sock, 4)
            payload = exact(sock, length)
            return first & 15, bytes(x ^ mask[i % 4] for i, x in enumerate(payload))

        def send_frame(sock, payload, opcode=1, final=True):
            if isinstance(payload, str):
                payload = payload.encode()
            length = len(payload)
            header = bytes([(128 if final else 0) | opcode])
            if length < 126:
                header += bytes([length])
            elif length < 65536:
                header += bytes([126]) + struct.pack('!H', length)
            else:
                header += bytes([127]) + struct.pack('!Q', length)
            sock.sendall(header + payload)

        import importlib.util
        sys.path.insert(0, str(Path(__file__).parents[1] / 'scripts'))
        spec = importlib.util.spec_from_file_location('runner', Path(__file__).parents[1] / 'scripts/capture_readonly.py')
        runner = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(runner)
        paper_path = root / 'paper-policy.json'
        params = runner.paper_policy(json.loads(Path(args.metadata).read_text()))
        basket_metadata = root / 'basket-metadata.json'
        basket_metadata_value = {'schema_version': 1, 'metadata_version': 2, 'venue': 'kalshi',
            'markets': [{'id': i, 'ticker': 'SYNTHETIC-BASKET-' + str(i)} for i in (1, 2, 3)], 'constraints': []}
        basket_metadata.write_text(json.dumps(basket_metadata_value))
        # The CLI appends a text-mode newline: CRLF on Windows, LF on Unix.
        # Neither terminator belongs to the canonical bytes bound by the policy.
        basket_canonical = subprocess.run([args.engine, 'metadata', 'canonical', str(basket_metadata)],
            capture_output=True, check=True).stdout.rstrip(b'\r\n')
        assert basket_canonical == json.dumps(basket_metadata_value, sort_keys=True,
            separators=(',', ':'), ensure_ascii=False).encode('utf-8'), 'fixture canonical metadata mismatch'
        basket_metadata_hash = hashlib.sha256(basket_canonical).hexdigest()
        from basket_screen_oracle_tests import fixture as basket_fixture
        passed = 0
        for scenario in ('untrusted', 'hostname', 'normal', 'empty_sides', 'reconnect', 'gap', 'duplicate', 'subscription_error', 'malformed', 'auth', 'idle', 'snapshot_timeout', 'handshake_timeout', 'oversize', 'overflow', 'paper_quiet', 'paper_partial', 'paper_disconnect', 'paper_eof', 'paper_burst', 'paper_trades', 'trade_gap', 'trade_ack_timeout', 'basket_quiet', 'basket_gap', 'basket_burst', 'basket_expiry', 'basket_trades'):
            is_basket = scenario.startswith('basket_')
            selected_tickers = ([m['ticker'] for m in basket_metadata_value['markets']] if is_basket else
                [m['ticker'] for m in sorted(json.loads(Path(args.metadata).read_text())['markets'], key=lambda m: m['id']) if m['id'] in (1, 2)])
            trades_enabled = scenario in ('paper_trades', 'trade_gap', 'trade_ack_timeout', 'basket_trades')
            errors, wire_messages, headers_seen, pong_seen = [], [], [], []
            listener = socket.socket()
            listener.bind(('127.0.0.1', 0))
            listener.listen(4)
            listener.settimeout(5)
            port = listener.getsockname()[1]
            attempts = 2 if scenario == 'reconnect' else 1
            def serve():
                try:
                    for attempt in range(attempts):
                        raw, _ = listener.accept()
                        raw.settimeout(4)
                        server_context = mismatch_context if scenario == 'hostname' else context
                        with server_context.wrap_socket(raw, server_side=True) as sock:
                            sock.settimeout(3)
                            request = b''
                            while not request.endswith(b'\r\n\r\n'):
                                request += exact(sock, 1)
                            lines = request.decode().split('\r\n')
                            assert lines[0] == 'GET /trade-api/ws/v2 HTTP/1.1'
                            headers = dict(line.split(': ', 1) for line in lines[1:] if ': ' in line)
                            headers = {key.lower(): value for key, value in headers.items()}
                            headers_seen.append(headers)
                            assert headers['kalshi-access-key'] == 'fixture'
                            timestamp = headers['kalshi-access-timestamp']
                            assert abs(int(timestamp) - int(time.time() * 1000)) < 5000
                            message = root / 'signed.txt'
                            signature = root / 'signature.bin'
                            message.write_text(timestamp + 'GET/trade-api/ws/v2')
                            signature.write_bytes(base64.b64decode(headers['kalshi-access-signature']))
                            openssl('dgst', '-sha256', '-verify', pub, '-signature', signature,
                                    '-sigopt', 'rsa_padding_mode:pss', '-sigopt', 'rsa_pss_saltlen:32', message)
                            if scenario == 'auth':
                                sock.sendall(b'HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n\r\n')
                                continue
                            if scenario == 'handshake_timeout':
                                time.sleep(2)
                                continue
                            accept = base64.b64encode(hashlib.sha1((headers['sec-websocket-key'] + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest())
                            sock.sendall(b'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ' + accept + b'\r\n\r\n')
                            commands = []
                            for command_index in range(2 if trades_enabled else 1):
                                opcode, payload = read_frame(sock)
                                assert opcode == 1
                                command = json.loads(payload)
                                assert command['cmd'] == 'subscribe'
                                assert command['id'] == command_index + 1
                                if command_index == 0:
                                    assert command['params']['use_yes_price'] is True
                                    assert command['params']['channels'] == ['orderbook_delta']
                                else:
                                    assert 'use_yes_price' not in command['params']
                                    assert command['params']['channels'] == ['trade']
                                assert command['params']['market_tickers'] == selected_tickers
                                commands.append(command)
                            def send_message(message, fragment=False):
                                # Whitespace is intentional: persisted bytes must remain exact.
                                if scenario == 'empty_sides' and message['type'] == 'orderbook_snapshot':
                                    message['msg'].pop('no_dollars_fp')
                                    if message['msg']['market_ticker'] == selected_tickers[1]:
                                        message['msg'].pop('yes_dollars_fp')
                                payload = ' ' + json.dumps(message, separators=(', ', ': ')) + '\n'
                                wire_messages.append(payload)
                                if fragment:
                                    send_frame(sock, payload[:13], final=False)
                                    send_frame(sock, b'heartbeat', opcode=9)
                                    send_frame(sock, payload[13:], opcode=0)
                                else:
                                    send_frame(sock, payload)
                            for command in reversed(commands):
                                if scenario != 'trade_ack_timeout' or command['id'] == 1:
                                    send_message({'type': 'subscribed', 'id': command['id'], 'msg': {'channel': command['params']['channels'][0], 'sid': 10 + command['id']}})
                            def send_trade(seq, ticker, *, block=None, millis=True):
                                message = {'type': 'trade', 'sid': 12, 'seq': seq, 'msg': {
                                    'trade_id': 'fixture-' + str(seq), 'market_ticker': ticker,
                                    'yes_price_dollars': '0.2400', 'no_price_dollars': '0.7600',
                                    'count_fp': '1.25', 'taker_side': 'no', 'taker_outcome_side': 'no',
                                    'taker_book_side': 'ask', 'ts': 1760000000}}
                                if millis:
                                    message['msg']['ts_ms'] = 1760000000123
                                if block is not None:
                                    message['msg']['is_block_trade'] = block
                                send_message(message)
                            if scenario in ('paper_trades', 'basket_trades', 'trade_gap'):
                                # Trade sequencing is independent of book seq=40;
                                # observation is valid before any book snapshot.
                                send_trade(700, selected_tickers[0], block=False)
                            if scenario == 'subscription_error':
                                send_message({'type': 'error', 'sid': 11, 'seq': 1, 'msg': {'code': 25}})
                            elif scenario == 'snapshot_timeout':
                                for _ in range(13):
                                    send_frame(sock, b'heartbeat', opcode=9)
                                    time.sleep(0.15)
                            elif scenario != 'idle':
                                for market_index, ticker in enumerate(commands[0]['params']['market_tickers']):
                                    send_message({'type': 'orderbook_snapshot', 'sid': 11, 'seq': 40 + market_index,
                                        'msg': {'market_ticker': ticker,
                                                'yes_dollars_fp': [['0.5000' if market_index == 0 else '0.7000', '5.00']],
                                                'no_dollars_fp': [['0.6000' if market_index == 0 else '0.8000', '5.00']]}}, fragment=scenario == 'normal')
                                    if scenario in ('paper_trades', 'basket_trades') and market_index == 0:
                                        send_trade(701, selected_tickers[1], block=True)
                                if scenario == 'malformed':
                                    send_frame(sock, '{"type":')
                                elif scenario == 'oversize':
                                    send_frame(sock, ' ' * (1024 * 1024 + 1))
                                elif scenario == 'overflow':
                                    send_message({'type': 'ignored', 'padding': 'x' * 8192})
                                elif scenario == 'trade_gap':
                                    send_trade(702, selected_tickers[0])
                                elif scenario == 'trade_ack_timeout':
                                    # Valid books and transport heartbeats cannot
                                    # hide the missing trade-channel acknowledgement.
                                    for _ in range(13):
                                        send_frame(sock, b'heartbeat', opcode=9)
                                        time.sleep(0.15)
                                else:
                                    send_message({'type': 'orderbook_delta', 'sid': 11,
                                        'seq': 44 if scenario in ('gap', 'reconnect') and attempt == 0 else 41 if scenario == 'duplicate' else 40 + len(selected_tickers),
                                        'msg': {'market_ticker': commands[0]['params']['market_tickers'][0], 'side': 'yes', 'price_dollars': '0.5000', 'delta_fp': '1.00'}})
                                    if scenario in ('paper_trades', 'basket_trades'):
                                        send_trade(702, selected_tickers[0], millis=False)
                            if scenario in ('paper_disconnect', 'basket_gap'):
                                send_message({'type': 'orderbook_delta', 'sid': 11, 'seq': 100,
                                    'msg': {'market_ticker': selected_tickers[0], 'side': 'yes', 'price_dollars': '0.5000', 'delta_fp': '1.00'}})
                            if scenario in ('paper_burst', 'basket_burst'):
                                for seq in range(41 + len(selected_tickers), 2041 + len(selected_tickers)):
                                    send_message({'type': 'orderbook_delta', 'sid': 11, 'seq': seq,
                                        'msg': {'market_ticker': selected_tickers[0], 'side': 'yes', 'price_dollars': '0.5000', 'delta_fp': '1.00'}})
                            if scenario == 'basket_expiry':
                                # Healthy transport, unchanged books: the policy
                                # deadline must fire without a new market tick.
                                for _ in range(10):
                                    send_frame(sock, b'heartbeat', opcode=9)
                                    time.sleep(0.2)
                            if scenario == 'paper_quiet':
                                # No market message follows the signal. Orders must
                                # complete before idle timeout, while client lives.
                                deadline = time.monotonic() + 0.5
                                while time.monotonic() < deadline:
                                    trace = root / scenario / 'paper.jsonl'
                                    if trace.exists() and 'order_response' in trace.read_text():
                                        break
                                    time.sleep(0.005)
                                else:
                                    raise AssertionError('paper orders did not advance during silence')
                            try:
                                while True:
                                    opcode, payload = read_frame(sock)
                                    if opcode == 10:
                                        pong_seen.append(payload)
                                    else:
                                        assert opcode == 8, 'unexpected client traffic: only subscription and protocol control allowed'
                            except (EOFError, ConnectionError, ssl.SSLError):
                                pass
                except (ConnectionError, ssl.SSLError):
                    if scenario not in ('oversize', 'overflow', 'snapshot_timeout', 'trade_ack_timeout', 'untrusted', 'hostname'):
                        errors.append('unexpected disconnect')
                except Exception as error:
                    errors.append(repr(error))
                finally:
                    listener.close()
            thread = threading.Thread(target=serve)
            thread.start()
            output_dir = root / scenario
            trusted_cert = wrong_cert if scenario == 'untrusted' else mismatch_cert if scenario == 'hostname' else cert
            parameters = json.loads(json.dumps(params))
            if scenario == 'paper_partial':
                parameters['reject_legs'] = [True, False]
            if scenario == 'paper_disconnect':
                parameters['leg_latency_ns'] = [5000000000, 5000000000]
            if scenario == 'paper_eof':
                parameters['lifecycle']['response_latency_ns'] = [5000000000, 5000000000]
            paper_path.write_text(json.dumps(parameters))
            basket_policy_path = root / 'basket-policy.json'
            if is_basket:
                screen = basket_fixture(1000)
                screen['markets'] = basket_metadata_value['markets']
                screen['books'], screen['as_of_ms'] = [], 0
                # Quiet books must remain eligible even though these REST-only
                # age/skew fields are smaller than the fixture's quiet period.
                screen['max_age_ms'], screen['max_skew_ms'] = 1, 1
                wall_ms = int(time.time() * 1000)
                basket_policy_path.write_text(json.dumps({'schema_version': 1,
                    'kind': 'conditional_basket_observation', 'qualification_sha256': 'a' * 64,
                    'metadata_sha256': basket_metadata_hash, 'valid_from_unix_ms': wall_ms - 60000,
                    'valid_until_unix_ms': wall_ms + (1200 if scenario == 'basket_expiry' else 60000), 'max_episode_events': 10000,
                    'freshness_mode': 'contiguous_shared_stream', 'screen': screen}))
            result = subprocess.run([args.client, str(basket_metadata) if is_basket else args.metadata, str(port), str(trusted_cert), str(key), str(output_dir),
                                     '6000' if scenario == 'reconnect' else '4000', str(attempts), scenario] + ([str(basket_policy_path)] if is_basket else [str(paper_path)] if scenario.startswith('paper_') else []),
                                    capture_output=True, text=True, timeout=10)
            thread.join(timeout=6)
            assert not thread.is_alive() and not errors, (scenario, errors, result.stdout, result.stderr)
            assert result.returncode == 0, (scenario, result.stdout, result.stderr)
            summary = json.loads(result.stdout)
            assert summary['connections'] == attempts, (scenario, summary)
            if scenario == 'overflow':
                assert not summary['finalized'] and not (output_dir / 'manifest.json').exists(), summary
            else:
                assert summary['finalized'], (scenario, summary)
                transcripts = [subprocess.run([args.engine, 'session', 'replay', str(output_dir), str(output_dir / 'replay.json')],
                                              capture_output=True, text=True, check=True).stdout for _ in range(2)]
                assert transcripts[0] == transcripts[1], 'replay must be byte-identical'
                plan = json.loads((output_dir / 'replay.json').read_text())
                assert plan['schema_version'] == (4 if trades_enabled else 3), (scenario, plan)
                replay_lines = [json.loads(line) for line in transcripts[0].splitlines()]
                observed_trades = [line for line in replay_lines if line['type'] == 'public_trade']
                assert len(observed_trades) == summary['public_trades'], (scenario, summary, observed_trades)
                if scenario in ('paper_trades', 'basket_trades', 'trade_gap'):
                    expected = [(700, 1, False, 1760000000123)]
                    if scenario in ('paper_trades', 'basket_trades'):
                        expected += [(701, 2, True, 1760000000123), (702, 1, None, 1760000000000)]
                    assert len(observed_trades) == len(expected), (scenario, observed_trades)
                    for trade, (seq, market, block, timestamp) in zip(observed_trades, expected):
                        assert {name: trade[name] for name in ('trade_id', 'market_id', 'yes_price_1e4',
                            'quantity_centicontracts', 'taker_side', 'exchange_time_ms', 'is_block_trade', 'candidates')} == {
                            'trade_id': 'fixture-' + str(seq), 'market_id': market, 'yes_price_1e4': 2400,
                            'quantity_centicontracts': 125, 'taker_side': 'no', 'exchange_time_ms': timestamp,
                            'is_block_trade': block, 'candidates': []}, trade
                raw_journal = (output_dir / 'market.journal').read_bytes()
                for header in headers_seen:
                    assert header['kalshi-access-signature'].encode() not in raw_journal
                if scenario not in ('oversize', 'malformed', 'snapshot_timeout'):
                    for payload in wire_messages:
                        assert payload.encode() in raw_journal, (scenario, 'raw bytes changed')
            if scenario in ('untrusted', 'hostname'):
                assert summary['reason'] == 'tls_verification_or_handshake' and not headers_seen, summary
            if scenario == 'normal':
                assert summary['market_updates'] == 3 and pong_seen, summary
            if scenario == 'empty_sides':
                assert summary['market_updates'] == 3, summary
            if scenario == 'auth':
                assert summary['reason'] == 'authentication_rejected', summary
            if scenario == 'reconnect':
                assert summary['market_updates'] == 5, summary
            if scenario in ('gap', 'duplicate', 'malformed', 'subscription_error', 'trade_gap'):
                assert summary['reason'] == 'feed_invalidated', summary
            if scenario == 'trade_gap':
                assert summary['market_updates'] == 2 and summary['public_trades'] == 1, summary
            if scenario == 'idle':
                assert summary['reason'] == 'idle_timeout', summary
            if scenario in ('snapshot_timeout', 'trade_ack_timeout'):
                assert summary['reason'] == 'snapshot_timeout', summary
            if scenario == 'handshake_timeout':
                assert summary['reason'] == 'connect_timeout', summary
            if scenario == 'oversize':
                assert summary['reason'] == 'read_failure' and summary['market_updates'] == 2, summary
            if scenario.startswith('paper_'):
                paper = json.loads((output_dir / 'paper-summary.json').read_text())
                assert paper['live_replay_equal'] is True and paper['attempts'] == 1, (scenario, paper)
                if scenario in ('paper_quiet', 'paper_burst', 'paper_trades'):
                    assert paper['completed_pairs'] == 1 and paper['lifecycle']['unknown_orders'] == 0, paper
                    assert paper['lifecycle']['simulated_net_pnl_micro_usd'] is None, paper
                if scenario == 'paper_partial':
                    assert paper['residual_exit']['sold_centicontracts'] > 0 and paper['lifecycle']['orders'] == 3, paper
                if scenario == 'paper_disconnect':
                    assert paper['spent_micro_usd'] == 0, paper
                if scenario == 'paper_eof':
                    assert paper['lifecycle']['unknown_orders'] == 2 and paper['lifecycle']['reserved_micro_usd'] > 0, paper
                if scenario == 'paper_burst':
                    assert summary['market_updates'] == 2003, summary
                if scenario in ('paper_trades', 'basket_trades'):
                    assert summary['market_updates'] == 3 and summary['public_trades'] == 3, summary
                    # Independently compare the economic records; timestamps and
                    # all execution quantities must match, not just final totals.
                    def economic_trace(path):
                        records = []
                        for line in path.read_text().splitlines():
                            record = json.loads(line)
                            if record['type'] in ('paper_status', 'paper_timing', 'study_start'):
                                continue
                            if record['type'] == 'study_complete':
                                record.pop('manifest_sha256', None)
                                record.pop('plan_sha256', None)
                            records.append(record)
                        return records
                    assert economic_trace(output_dir / 'paper.jsonl') == economic_trace(output_dir / 'paper-replay.jsonl')
                assert paper['receive_callback_to_decisions']['samples'] == summary['market_updates'], paper
            if is_basket:
                if scenario == 'basket_trades':
                    assert summary['public_trades'] == 3, summary
                basket = json.loads((output_dir / 'basket-summary.json').read_text())
                assert basket['live_replay_equal'] is True, (scenario, basket)
                assert basket['mode'] == 'live_conditional_observation_no_orders', basket
                assert basket['orders_sent'] == 0 and basket['simulated_fills'] is False, basket
                assert basket['incomplete'] is False and basket['event_budget_exceeded'] is False, basket
                assert not (output_dir / 'paper.jsonl').exists()
                def basket_trace(path):
                    values = []
                    for line in path.read_text().splitlines():
                        value = json.loads(line)
                        if value['type'] in ('basket_start', 'basket_status', 'basket_timing'):
                            continue
                        if value['type'] == 'basket_complete':
                            value.pop('manifest_sha256', None)
                            value.pop('plan_sha256', None)
                        values.append(value)
                    return values
                assert basket_trace(output_dir / 'basket.jsonl') == basket_trace(output_dir / 'basket-replay.jsonl')
                replayed = subprocess.run([args.engine, 'basket', 'observe', str(output_dir), str(output_dir / 'basket-policy.json')],
                    capture_output=True, text=True, check=True)
                independent_path = root / 'independent-basket-replay.jsonl'
                independent_path.write_text(replayed.stdout)
                assert basket_trace(independent_path) == basket_trace(output_dir / 'basket.jsonl')
                assert basket['receive_callback_to_decisions']['samples'] == summary['market_updates'], basket
                assert summary['market_updates'] == (2004 if scenario == 'basket_burst' else 4), summary
                assert summary['reason'] == ('feed_invalidated' if scenario == 'basket_gap' else 'idle_timeout'), summary
            passed += 1
            print('PASS', scenario, summary, flush=True)
        print('PASS', passed, 'TLS/WS fixture scenarios')

if __name__ == '__main__':
    main()
