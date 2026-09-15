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


def main():
    parser = argparse.ArgumentParser()
    for arg in ('client', 'engine', 'openssl', 'metadata'):
        parser.add_argument('--' + arg, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='eme-tls-fixture-') as directory:
        root = Path(directory)
        key, cert, pub = (root / name for name in ('key.pem', 'cert.pem', 'public.pem'))
        def openssl(*command):
            return subprocess.run([args.openssl, *map(str, command)], check=True, capture_output=True).stdout
        openssl('genpkey', '-algorithm', 'RSA', '-pkeyopt', 'rsa_keygen_bits:2048', '-out', key)
        config = root / 'cert.cnf'
        config.write_text('[req]\ndistinguished_name=dn\nx509_extensions=ext\nprompt=no\n[dn]\nCN=localhost\n[ext]\nsubjectAltName=DNS:localhost\nbasicConstraints=critical,CA:TRUE\n')
        openssl('req', '-new', '-x509', '-key', key, '-out', cert, '-days', '1', '-sha256', '-config', config)
        openssl('pkey', '-in', key, '-pubout', '-out', pub)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)

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

        passed = 0
        for scenario in ('untrusted', 'hostname', 'normal', 'reconnect', 'gap', 'duplicate', 'subscription_error', 'malformed', 'auth', 'idle', 'snapshot_timeout', 'handshake_timeout', 'oversize', 'overflow'):
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
                        with context.wrap_socket(raw, server_side=True) as sock:
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
                                time.sleep(0.8)
                                continue
                            accept = base64.b64encode(hashlib.sha1((headers['sec-websocket-key'] + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest())
                            sock.sendall(b'HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ' + accept + b'\r\n\r\n')
                            commands = []
                            for _ in range(2):
                                opcode, payload = read_frame(sock)
                                assert opcode == 1
                                command = json.loads(payload)
                                assert command['cmd'] == 'subscribe'
                                assert command['params']['use_yes_price'] is True
                                assert command['params']['channels'] == ['orderbook_delta']
                                commands.append(command)
                            def send_message(message, fragment=False):
                                # Whitespace is intentional: persisted bytes must remain exact.
                                payload = ' ' + json.dumps(message, separators=(', ', ': ')) + '\n'
                                wire_messages.append(payload)
                                if fragment:
                                    send_frame(sock, payload[:13], final=False)
                                    send_frame(sock, b'heartbeat', opcode=9)
                                    send_frame(sock, payload[13:], opcode=0)
                                else:
                                    send_frame(sock, payload)
                            for command in reversed(commands):
                                send_message({'type': 'subscribed', 'id': command['id'], 'msg': {'channel': 'orderbook_delta', 'sid': 10 + command['id']}})
                            if scenario == 'subscription_error':
                                send_message({'type': 'error', 'sid': 11, 'seq': 1, 'msg': {'code': 25}})
                            elif scenario == 'snapshot_timeout':
                                for _ in range(4):
                                    send_frame(sock, b'heartbeat', opcode=9)
                                    time.sleep(0.15)
                            elif scenario != 'idle':
                                for command in commands:
                                    send_message({'type': 'orderbook_snapshot', 'sid': 10 + command['id'], 'seq': 40,
                                        'msg': {'market_ticker': command['params']['market_ticker'],
                                                'yes_dollars_fp': [['0.5000' if command['id'] == 1 else '0.7000', '5.00']],
                                                'no_dollars_fp': [['0.6000' if command['id'] == 1 else '0.8000', '5.00']]}}, fragment=scenario == 'normal')
                                if scenario == 'malformed':
                                    send_frame(sock, '{"type":')
                                elif scenario == 'oversize':
                                    send_frame(sock, ' ' * (1024 * 1024 + 1))
                                elif scenario == 'overflow':
                                    send_message({'type': 'ignored', 'padding': 'x' * 8192})
                                else:
                                    send_message({'type': 'orderbook_delta', 'sid': 11,
                                        'seq': 43 if scenario in ('gap', 'reconnect') and attempt == 0 else 40 if scenario == 'duplicate' else 41,
                                        'msg': {'market_ticker': commands[0]['params']['market_ticker'], 'side': 'yes', 'price_dollars': '0.5000', 'delta_fp': '1.00'}})
                            try:
                                while True:
                                    opcode, payload = read_frame(sock)
                                    if opcode == 10:
                                        pong_seen.append(payload)
                            except (EOFError, ConnectionError, ssl.SSLError):
                                pass
                except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
                    if scenario not in ('oversize', 'overflow', 'snapshot_timeout', 'untrusted', 'hostname'):
                        errors.append('unexpected disconnect')
                except Exception as error:
                    errors.append(repr(error))
                finally:
                    listener.close()
            thread = threading.Thread(target=serve)
            thread.start()
            output_dir = root / scenario
            result = subprocess.run([args.client, args.metadata, str(port), str(cert), str(key), str(output_dir),
                                     '1800' if scenario == 'reconnect' else '1100', str(attempts), scenario],
                                    capture_output=True, text=True, timeout=6)
            thread.join(timeout=4)
            assert not thread.is_alive() and not errors, (scenario, errors)
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
            if scenario == 'auth':
                assert summary['reason'] == 'authentication_rejected', summary
            if scenario == 'reconnect':
                assert summary['market_updates'] == 5, summary
            if scenario in ('gap', 'duplicate', 'malformed', 'subscription_error'):
                assert summary['reason'] == 'feed_invalidated', summary
            if scenario == 'idle':
                assert summary['reason'] == 'idle_timeout', summary
            if scenario == 'snapshot_timeout':
                assert summary['reason'] == 'snapshot_timeout', summary
            if scenario == 'handshake_timeout':
                assert summary['reason'] == 'connect_timeout', summary
            passed += 1
            print('PASS', scenario, summary, flush=True)
        print('PASS', passed, 'TLS/WS fixture scenarios')

if __name__ == '__main__':
    main()
