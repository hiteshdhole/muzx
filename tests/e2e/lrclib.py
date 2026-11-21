import json, sys
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import urlparse, parse_qs
LOG = open(sys.argv[1], 'a', buffering=1)
L = "[ar:x]\n[00:00.50]First line\n[00:02.00]Second line\n[00:04.00]Third line\n[00:06.00]\n[00:08.00]Fourth 日本語"
class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        u = urlparse(self.path); q = parse_qs(u.query); LOG.write(self.path + "\n")
        body = None
        if u.path == "/api/get" and q.get("track_name") == ["Test Song"] and q.get("artist_name") == ["Test Artist"]:
            body = {"trackName": "Test Song", "artistName": "Test Artist", "duration": 200, "syncedLyrics": L}
        elif u.path == "/api/search":
            qq = (q.get("q") or [""])[0].lower()
            if "yt song" in qq:
                body = [{"trackName": "Other", "artistName": "Z", "duration": 10, "syncedLyrics": "[00:01.00]WRONG"},
                        {"trackName": "YT Song", "artistName": "YT Artist", "duration": 199, "syncedLyrics": "[00:01.00]From search\n[00:03.00]Found by search"}]
            else: body = []
        if body is None: self.send_response(404); self.end_headers(); return
        d = json.dumps(body).encode(); self.send_response(200); self.send_header("Content-Type", "application/json"); self.end_headers(); self.wfile.write(d)
HTTPServer(("127.0.0.1", 18767), H).serve_forever()
