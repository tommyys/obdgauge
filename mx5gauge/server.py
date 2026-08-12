"""Tiny stdlib HTTP server: serves the gauge UI and a JSON snapshot endpoint."""
import json
import os
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

WEB_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'web')


def make_handler(gauge):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass                                  # keep the console clean

        def _send(self, code, body, ctype):
            if isinstance(body, str):
                body = body.encode('utf-8')
            self.send_response(code)
            self.send_header('Content-Type', ctype)
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Cache-Control', 'no-store')
            self.end_headers()
            try:
                self.wfile.write(body)
            except BrokenPipeError:
                pass

        def do_GET(self):
            path = self.path.split('?')[0]
            if path == '/data':
                self._send(200, json.dumps(gauge.snapshot()), 'application/json')
                return
            if path in ('/', '/index.html'):
                fn = os.path.join(WEB_DIR, 'index.html')
                with open(fn, 'rb') as fh:
                    self._send(200, fh.read(), 'text/html; charset=utf-8')
                return
            self._send(404, 'not found', 'text/plain')

    return Handler


class PortInUse(Exception):
    pass


def serve(gauge, port=8420):
    try:
        httpd = ThreadingHTTPServer(('127.0.0.1', port), make_handler(gauge))
    except OSError as exc:
        # EADDRINUSE — almost always a previous run still holding the port
        raise PortInUse(
            'port %d is already in use — another gauge is probably still '
            'running.\n  Free it with:  lsof -ti tcp:%d | xargs kill'
            % (port, port)) from exc
    t = threading.Thread(target=httpd.serve_forever, daemon=True)
    t.start()
    return httpd
