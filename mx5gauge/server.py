"""Tiny stdlib HTTP server: serves the gauge UI, a JSON snapshot, and the
drive library the on-screen picker browses."""
import json
import os
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from . import library

WEB_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'web')

# Refuse absurd request bodies outright rather than reading them into memory.
MAX_BODY = 4096


def make_handler(gauge, root=None, on_select=None, on_seek=None):
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

        def _json(self, code, obj):
            self._send(code, json.dumps(obj), 'application/json')

        def do_GET(self):
            path = self.path.split('?')[0]
            if path == '/data':
                self._json(200, gauge.snapshot())
                return
            if path == '/sessions':
                # the drive being written right now has no summary yet, so it
                # would otherwise be listed as "interrupted" while you drive it
                rec = getattr(gauge, 'recorder', None)
                writing = None
                if rec is not None and getattr(rec, 'path', None) \
                        and getattr(rec, 'rows', 0) > 0:
                    writing = os.path.basename(rec.path)
                self._json(200, {
                    'entries': library.scan(root) if root else [],
                    'current': gauge.current_file,
                    'recording': writing,
                    # switching is a replay-only idea: in the car the live link
                    # is the whole point, so the picker shows but cannot load
                    'can_switch': bool(on_select) and gauge.source_kind != 'live',
                })
                return
            if path == '/':
                path = '/index.html'
            # serve any file in web/ — basename only, so '..' cannot escape
            name = os.path.basename(path)
            fn = os.path.join(WEB_DIR, name)
            if name and os.path.isfile(fn):
                ctype = 'text/html; charset=utf-8' if name.endswith('.html') \
                    else 'text/plain; charset=utf-8'
                with open(fn, 'rb') as fh:
                    self._send(200, fh.read(), ctype)
                return
            self._send(404, 'not found', 'text/plain')

        def _body(self):
            """The decoded JSON body, or None (a reply has been sent)."""
            try:
                n = int(self.headers.get('Content-Length') or 0)
            except ValueError:
                n = 0
            if n <= 0 or n > MAX_BODY:
                self._json(400, {'ok': False, 'error': 'bad body'})
                return None
            try:
                return json.loads(self.rfile.read(n).decode('utf-8')) or {}
            except Exception:
                self._json(400, {'ok': False, 'error': 'bad json'})
                return None

        def do_POST(self):
            path = self.path.split('?')[0]
            if path == '/select':
                self._do_select()
            elif path == '/seek':
                self._do_seek()
            else:
                self._send(404, 'not found', 'text/plain')

        def _do_select(self):
            if on_select is None:
                self._json(409, {'ok': False, 'error': 'switching not available'})
                return
            if gauge.source_kind == 'live':
                self._json(409, {'ok': False,
                                 'error': 'live session — not switching'})
                return
            req = self._body()
            if req is None:
                return
            entry = library.resolve(root, req.get('name')) if root else None
            if entry is None:
                self._json(404, {'ok': False, 'error': 'no such drive'})
                return
            # A drive is opened at its end: what you asked to see is the
            # finished drive's summary, and scrubbing back is how you replay it.
            on_select(entry, seek_to_end=True)
            self._json(200, {'ok': True, 'name': entry['name']})

        def _do_seek(self):
            # Seeking is meaningless live — there is no recorded timeline to
            # move along, only the car in front of you.
            if on_seek is None or gauge.source_kind == 'live':
                self._json(409, {'ok': False, 'error': 'nothing to scrub'})
                return
            req = self._body()
            if req is None:
                return
            try:
                t = float(req.get('t'))
            except (TypeError, ValueError):
                self._json(400, {'ok': False, 'error': 'bad position'})
                return
            if t != t or t in (float('inf'), float('-inf')):
                self._json(400, {'ok': False, 'error': 'bad position'})
                return
            on_seek(t)
            self._json(200, {'ok': True, 't': t})

    return Handler


class PortInUse(Exception):
    pass


def serve(gauge, port=8420, root=None, on_select=None, on_seek=None):
    try:
        httpd = ThreadingHTTPServer(
            ('127.0.0.1', port),
            make_handler(gauge, root, on_select, on_seek))
    except OSError as exc:
        # EADDRINUSE — almost always a previous run still holding the port
        raise PortInUse(
            'port %d is already in use — another gauge is probably still '
            'running.\n  Free it with:  lsof -ti tcp:%d | xargs kill'
            % (port, port)) from exc
    t = threading.Thread(target=httpd.serve_forever, daemon=True)
    t.start()
    return httpd
