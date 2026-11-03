import json
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from umeko_provider_sdk import ProviderError
from umeko_provider_sdk.errors import format_http_error
from umeko_provider_sdk.http import HttpSession


class ProviderErrorsTest(unittest.TestCase):
    def test_details_and_reason(self):
        text = format_http_error(400, json.dumps({'error': {'message': 'At most 3 image(s) may be provided.', 'type': 'BadRequestError', 'param': 'image', 'code': 400}}))
        for expected in ('HTTP 400', 'At most 3 image(s)', 'BadRequestError', 'image', '400'):
            self.assertIn(expected, text)
        self.assertNotIn('提示：', text)
        self.assertIn('bad_image', format_http_error(400, '{"error":{"type":"invalid_request_error","code":"bad_image"}}'))
        self.assertIn('Retry-After: 17', format_http_error(429, '{"error":{"message":"Too many requests"}}', {'retry-after': '17'}))
        self.assertIn('服务器未返回错误详情', format_http_error(400, ''))
        self.assertLessEqual(len(format_http_error(400, json.dumps({'error': {'message': 'Image rejected: ' + '长' * 10000, 'type': 'BadRequest'}}))), 1000)

    def test_redacts_secrets(self):
        message = format_http_error(400, json.dumps({'error': {'message': 'Invalid image; Authorization: Bearer fixture-bearer-token; api_key=fixture-api-key; password=fixture-password; opaque-fixture-secret'}}), secrets=('opaque-fixture-secret',))
        for secret in ('fixture-bearer-token', 'fixture-api-key', 'fixture-password', 'opaque-fixture-secret'):
            self.assertNotIn(secret, message)
        self.assertIn('Invalid image', message)

    def test_real_sdk_http_error(self):
        body = b'{"error":{"message":"Rate limited for fixture-provider-secret and opaque-hidden-password"}}'
        class Handler(BaseHTTPRequestHandler):
            def do_POST(self):
                self.rfile.read(int(self.headers['Content-Length']))
                self.send_response(429)
                self.send_header('Retry-After', '17')
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            def log_message(self, *_): pass
        server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
        try:
            base = f'http://127.0.0.1:{server.server_port}/v1'
            with self.assertRaises(ProviderError) as caught:
                HttpSession([base], environment_proxy=False).request(base + '/models', method='POST', headers={'Authorization': 'Bearer fixture-provider-secret'}, data={'password': 'opaque-hidden-password'})
            message = str(caught.exception)
            self.assertIn('HTTP 429', message)
            self.assertIn('Rate limited', message)
            self.assertIn('Retry-After: 17', message)
            self.assertNotIn('fixture-provider-secret', message)
            self.assertNotIn('opaque-hidden-password', message)
        finally:
            server.shutdown(); server.server_close(); thread.join(timeout=2)


if __name__ == '__main__':
    unittest.main()
