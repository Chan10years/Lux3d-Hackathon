"""LAN-only game API gateway around the unmodified portable generation service."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import secrets
import socket
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.request import Request, build_opener, ProxyHandler
from urllib.error import HTTPError, URLError
from urllib.parse import urlsplit

ID = r'[0-9a-f]{32}'
GET_PATH = re.compile(rf'/api/v1/(health|tasks(/{ID})?|weapons(/{ID})?)|/workflow-assets/{ID}/(weapon\.glb|sketch\.png|reference\.png)')
POST_PATH = re.compile(rf'/api/v1/tasks(/{ID}/resume)?|/api/v1/weapons/{ID}/equip')
MAX_UPLOAD = 6 * 1024 * 1024
MAX_RESPONSE = 64 * 1024 * 1024

class ForgeGateway(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = False
    upstream = None
    stop_token = None
    def __init__(self, address): super().__init__(address, GatewayHandler)
    def server_bind(self):
        if hasattr(socket, 'SO_EXCLUSIVEADDRUSE'):
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        super().server_bind()

class GatewayHandler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        print('[ForgeGateway] ' + format % args, flush=True)

    def reply(self, code, body, content_type='application/json'):
        try:
            self.send_response(code)
            self.send_header('Content-Type', content_type)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError): pass

    def do_GET(self): self.forward()
    def do_POST(self): self.forward()

    def forward(self):
        self.connection.settimeout(30)
        path = urlsplit(self.path).path
        if self.headers.get('Origin'):
            self.reply(403, b'{"detail":"Native LAN game clients only"}'); return
        if path == '/internal/stop' and self.command == 'POST':
            if self.client_address[0] != '127.0.0.1' or not self.server.stop_token or not secrets.compare_digest(
                self.headers.get('X-Forge-Stop', ''), self.server.stop_token):
                self.reply(403, b'{"detail":"Local stop token required"}'); return
            self.reply(200, b'{"status":"stopping"}')
            threading.Thread(target=self.server.shutdown, daemon=True).start()
            return
        allowed_get, allowed_post = bool(GET_PATH.fullmatch(path)), bool(POST_PATH.fullmatch(path))
        if not allowed_get and not allowed_post:
            self.reply(404, b'{"detail":"Not exposed by the game gateway"}'); return
        if (self.command == 'GET' and not allowed_get) or (self.command == 'POST' and not allowed_post):
            self.reply(405, b'{"detail":"Method not allowed"}'); return
        if not self.server.upstream:
            self.reply(503, b'{"detail":"Generation service is starting"}'); return
        try:
            size = int(self.headers.get('Content-Length', '0'))
        except ValueError:
            self.reply(400, b'{"detail":"Invalid body length"}'); return
        if size < 0 or size > MAX_UPLOAD or self.headers.get('Transfer-Encoding'):
            self.reply(413, b'{"detail":"Upload too large or unsupported encoding"}'); return
        body = self.rfile.read(size) if self.command == 'POST' else None
        headers = {'Content-Type': self.headers.get('Content-Type', 'application/octet-stream')}
        request = Request(self.server.upstream + self.path, data=body, headers=headers, method=self.command)
        try:
            # Explicitly bypass desktop/system HTTP proxies for the local upstream.
            try: response = build_opener(ProxyHandler({})).open(request, timeout=120)
            except HTTPError as error: response = error
            with response:
                payload = response.read(MAX_RESPONSE + 1)
                if len(payload) > MAX_RESPONSE:
                    self.reply(502, b'{"detail":"Upstream response too large"}'); return
                if path == '/api/v1/health' and response.status == 200:
                    health = json.loads(payload)
                    health.pop('csrf', None)
                    health['lan_gateway'] = 'weaponforge-v1'
                    payload = json.dumps(health, ensure_ascii=False).encode('utf-8')
                self.reply(response.status, payload, response.headers.get('Content-Type', 'application/octet-stream'))
        except (URLError, TimeoutError, OSError, ValueError):
            self.reply(502, b'{"detail":"Generation upstream unavailable; inspect host generation logs"}')

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--handoff', type=Path, required=True)
    parser.add_argument('--state-dir', type=Path, required=True)
    parser.add_argument('--port', type=int, default=8020)
    args = parser.parse_args()
    state = args.state_dir.resolve()
    state.mkdir(parents=True, exist_ok=True)
    # Reserve the LAN port first; the original launcher then chooses a different local API port.
    server = ForgeGateway(('0.0.0.0', args.port))
    server.stop_token = secrets.token_urlsafe(32)
    spec = importlib.util.spec_from_file_location('original_forge_launcher', args.handoff.resolve() / 'Runtime/launch.py')
    original = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(original)
    metadata = state / 'service.json'
    try:
        with original.services(state) as (api, health):
            server.upstream = api
            info = {'api_url': api, 'gateway_port': server.server_port, 'settings_url': api + '/seedream.html',
                    'pid': os.getpid(), 'stop_token': server.stop_token}
            metadata.write_text(json.dumps(info, indent=2), encoding='utf-8')
            print(f'Forge LAN gateway ready on port {server.server_port}; local settings: {info["settings_url"]}', flush=True)
            print('Credentials are configured only on the host. Keep this service running for model downloads.', flush=True)
            server.serve_forever()
    finally:
        server.server_close()
        if metadata.exists(): metadata.unlink()

if __name__ == '__main__':
    try: main()
    except KeyboardInterrupt: pass
