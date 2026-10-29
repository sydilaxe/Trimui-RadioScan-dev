#!/usr/bin/env python3
"""Local stand-in for GitHub's releases, for testing RadioScan's self-update without the internet.

Serves DIR/RadioScan-trimui.zip (or each --asset, which can be given more than once) as the assets
of a release tagged TAG, the way GitHub does: the API's latest-release JSON, then a download link
that redirects (with a ~1000-character signed URL) to the file itself.

  python3 tools/fake_github.py DIR [--port 8766] [--tag v9.9.9]
  RS_UPDATE_API=http://127.0.0.1:8766/latest.json RADIOSCAN_DIR=... radioscan --update

Endpoints:  /latest.json  /download/TAG/ASSET  /asset/ASSET?sig=...
"""
import argparse
import json
import os
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--port", type=int, default=8766)
    ap.add_argument("--tag", default="v9.9.9")
    ap.add_argument("--asset", action="append")
    args = ap.parse_args()
    assets = args.asset or ["RadioScan-trimui.zip"]
    base = f"http://127.0.0.1:{args.port}"

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def send(self, body, status=200):
            self.send_response(status)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            if self.path == "/latest.json":
                release = {"tag_name": args.tag, "assets": [
                    {"name": "notes.txt", "browser_download_url": f"{base}/download/{args.tag}/notes.txt"}] + [
                    {"name": a, "browser_download_url": f"{base}/download/{args.tag}/{a}"} for a in assets]}
                self.send(json.dumps(release, indent=2).encode())
            elif self.path.startswith(f"/download/{args.tag}/") and self.path.split("/")[-1] in assets:
                self.send_response(302)
                self.send_header("Location", f"{base}/asset/{self.path.split('/')[-1]}?sig=" + "x" * 1000)
                self.send_header("Content-Length", "0")
                self.end_headers()
            elif self.path.startswith("/asset/") and self.path[7:].split("?")[0] in assets:
                with open(os.path.join(args.dir, self.path[7:].split("?")[0]), "rb") as f:
                    self.send(f.read())
            else:
                self.send(b"not found", 404)

    ThreadingHTTPServer(("127.0.0.1", args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
