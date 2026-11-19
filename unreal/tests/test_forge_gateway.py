"""Real HTTP regression checks for the LAN boundary; no cloud calls or credentials."""
import importlib.util
import json
from pathlib import Path
import threading
import socket
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.request import Request, urlopen, build_opener, ProxyHandler
from urllib.error import HTTPError

spec = importlib.util.spec_from_file_location('gateway', Path(__file__).resolve().parents[1] / 'scripts/forge_gateway.py')
gateway = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gateway)

class LocalBackend(BaseHTTPRequestHandler):
    def log_message(self, *args): pass
    def do_GET(self):
        if not self.headers['Host'].startswith('127.0.0.1:'):
            self.send_error(403); return
        payload = b'glTF-test-content' if 'weapon.glb' in self.path else json.dumps({'status': 'ok', 'csrf': 'local-only-token'}).encode()
        self.send_response(200); self.send_header('Content-Length', str(len(payload))); self.end_headers(); self.wfile.write(payload)
    def do_POST(self):
        data = self.rfile.read(int(self.headers.get('Content-Length', 0)))
        self.send_response(409 if data == b'busy' else 202)
        self.send_header('Content-Length', str(len(data))); self.end_headers(); self.wfile.write(data)

class GatewayChecks(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.backend = ThreadingHTTPServer(('127.0.0.1', 0), LocalBackend)
        cls.server = gateway.ForgeGateway(('127.0.0.1', 0))
        cls.server.upstream = 'http://127.0.0.1:' + str(cls.backend.server_port)
        for server in (cls.backend, cls.server):
            threading.Thread(target=server.serve_forever, daemon=True).start()
        cls.api = 'http://127.0.0.1:' + str(cls.server.server_port)
        cls.opener = build_opener(ProxyHandler({}))
    @classmethod
    def tearDownClass(cls):
        for server in (cls.server, cls.backend): server.shutdown(); server.server_close()
    def request(self, path, data=None, headers=None):
        request = Request(self.api + path, data=data, headers={'Host': '192.168.1.10:8020', **(headers or {})})
        try:
            with self.opener.open(request, timeout=3) as reply: return reply.status, reply.read()
        except HTTPError as error: return error.code, error.read()
    def test_health_rewrites_host_and_removes_local_token(self):
        status, body = self.request('/api/v1/health')
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(body)['lan_gateway'], 'weaponforge-v1')
        self.assertNotIn('csrf', json.loads(body))
    def test_task_upload_preserves_body_and_status(self):
        self.assertEqual(self.request('/api/v1/tasks', b'PNG multipart bytes'), (202, b'PNG multipart bytes'))
    def test_busy_error_is_preserved(self):
        self.assertEqual(self.request('/api/v1/tasks', b'busy'), (409, b'busy'))
    def test_model_download_is_unchanged(self):
        self.assertEqual(self.request('/workflow-assets/' + 'a' * 32 + '/weapon.glb'), (200, b'glTF-test-content'))
    def test_settings_and_other_paths_are_not_exposed(self):
        for path in ('/seedream.html', '/api/v1/config', '/api/v1/image-config', '/api/v1/economy', '/workflow-assets/../credentials/key'):
            with self.subTest(path=path): self.assertEqual(self.request(path)[0], 404)
    def test_wrong_method_is_rejected(self):
        self.assertEqual(self.request('/api/v1/health', b'body')[0], 405)
    def test_browser_origin_is_rejected(self):
        self.assertEqual(self.request('/api/v1/tasks', b'body', {'Origin': 'https://example.org'})[0], 403)
    def test_gateway_port_is_exclusively_reserved_for_original_port_scan(self):
        reserved = gateway.ForgeGateway(('0.0.0.0', 0))
        try:
            with socket.socket() as listener:
                with self.assertRaises(OSError): listener.bind(('127.0.0.1', reserved.server_port))
        finally: reserved.server_close()

if __name__ == '__main__': unittest.main()
