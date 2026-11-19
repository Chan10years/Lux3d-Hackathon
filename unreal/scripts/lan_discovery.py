"""Bounded IPv4 LAN room-code discovery; no subnet/IP scanning."""
import json
import re
import secrets
import socket
import time

PORT = 18903
VERSION = 1
APP = 'lux3d-hammer-lan'
INSTANCE = secrets.token_hex(16)


def packet(data):
    if len(data) > 1024:
        return None
    try:
        obj = json.loads(data)
        if (isinstance(obj, dict) and obj.get('app') == APP and obj.get('version') == VERSION
                and re.fullmatch(r'[0-9]{6}', str(obj.get('code', '')))
                and re.fullmatch(r'[0-9a-f]{32}', str(obj.get('nonce', '')))):
            return obj
    except (ValueError, UnicodeError):
        pass
    return None


def request_packet(code, nonce):
    return json.dumps(dict(app=APP, version=VERSION, kind='find', code=code, nonce=nonce)).encode()


def answer(data, published_code):
    obj = packet(data)
    if not obj or obj.get('kind') != 'find' or obj['code'] != published_code:
        return None
    return json.dumps({**obj, 'kind': 'found', 'instance': INSTANCE}).encode()


def valid_response(data, code, nonce):
    obj = packet(data)
    return bool(obj and obj.get('kind') == 'found' and obj['code'] == code and obj['nonce'] == nonce
                and re.fullmatch(r'[0-9a-f]{32}', str(obj.get('instance', ''))))


def unique_hosts(responses):
    return dict(responses)


def choose_host(responses):
    hosts = unique_hosts(responses)
    if not hosts:
        raise RuntimeError('未找到房间：请确认房主已开房、连接同一Wi-Fi且允许游戏联网，再试一次。')
    if len(hosts) != 1:
        raise RuntimeError('房号冲突，请其中一位房主退出后重新开房。')
    return next(iter(hosts.values()))


def find(code, broadcasts, timeout=3):
    nonce = secrets.token_hex(16)
    data = request_packet(code, nonce)
    found = []
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        sock.settimeout(.15)
        deadline = time.monotonic() + timeout
        next_send = 0
        while time.monotonic() < deadline:
            if time.monotonic() >= next_send:
                for address in set(broadcasts + ['255.255.255.255', '127.0.0.1']):
                    try: sock.sendto(data, (address, PORT))
                    except OSError: pass
                next_send = time.monotonic() + .8
            try:
                response, peer = sock.recvfrom(1025)
                if valid_response(response, code, nonce):
                    found.append((json.loads(response)['instance'], peer[0]))
            except socket.timeout:
                pass
    return choose_host(found)


def serve(get_code, stop):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        if hasattr(socket, 'SO_EXCLUSIVEADDRUSE'):
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        sock.bind(('0.0.0.0', PORT))
        sock.settimeout(.3)
        while not stop.is_set():
            try:
                data, peer = sock.recvfrom(1025)
                response = answer(data, get_code())
                if response: sock.sendto(response, peer)
            except socket.timeout:
                pass
