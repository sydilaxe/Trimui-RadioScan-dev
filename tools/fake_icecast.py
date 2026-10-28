#!/usr/bin/env python3
"""Local stand-in for internet radio, for testing RadioScan without the internet.

Generates two MP3 test streams (speech-like and music-like), then serves them the way
Icecast/Shoutcast do: endless MP3 with ICY metadata, plus a redirect, a .pls playlist,
a fake radio-browser directory and (optionally) HTTPS with a throwaway CA.

  python3 tools/fake_icecast.py --port 8765 [--tls-port 8766 --certdir DIR]

Endpoints:  /speech.mp3  /music.mp3  /redirect  /list.pls  /json/stations/search?...
"""
import argparse
import json
import math
import os
import socketserver
import ssl
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler

import lameenc
import numpy as np

SR = 44100


def encode(sig):
    enc = lameenc.Encoder()
    enc.set_bit_rate(128)
    enc.set_in_sample_rate(SR)
    enc.set_channels(2)
    enc.set_quality(5)
    pcm = np.clip(sig * 32767, -32768, 32767).astype(np.int16)
    st = np.repeat(pcm[:, None], 2, axis=1).reshape(-1)
    return enc.encode(st.tobytes()) + enc.flush()


def speech_like(seconds=30, seed=1):
    """Syllable bursts (~4/s) of a glottal pulse train through moving formants, with pauses and fricatives."""
    rng = np.random.default_rng(seed)
    n = SR * seconds
    out = np.zeros(n)
    t = 0
    while t < n:
        if rng.random() < 0.15:          # pause between phrases
            t += int(SR * rng.uniform(0.25, 0.6))
            continue
        dur = int(SR * rng.uniform(0.12, 0.25))
        seg = np.arange(dur) / SR
        f0 = rng.uniform(100, 180)
        src = (np.sin(2 * np.pi * f0 * seg) > 0.97).astype(float)  # pulse train
        if rng.random() < 0.3:          # fricative
            src = rng.normal(0, 0.3, dur)
        y = np.zeros(dur)
        for fc in (rng.uniform(300, 800), rng.uniform(900, 2200), rng.uniform(2400, 3200)):
            y += np.sin(2 * np.pi * fc * seg) * np.convolve(src, np.ones(20) / 20, "same")
        env = np.sin(np.pi * np.arange(dur) / dur) ** 2
        end = min(n, t + dur)
        out[t:end] += (y * env)[: end - t]
        t += dur + int(SR * rng.uniform(0.02, 0.08))
    return 0.5 * out / (np.max(np.abs(out)) + 1e-9)


def music_like(seconds=30):
    """Sustained chords with a steady pulse: dense, low-gap material."""
    n = SR * seconds
    t = np.arange(n) / SR
    chords = [(220, 277, 330), (196, 247, 294), (175, 220, 262), (165, 208, 247)]
    out = np.zeros(n)
    for i, ch in enumerate(chords * (seconds // 8 + 1)):
        a, b = int(i * 2 * SR), min(n, int((i + 1) * 2 * SR))
        if a >= n:
            break
        for f in ch:
            out[a:b] += np.sin(2 * np.pi * f * t[a:b]) + 0.3 * np.sin(4 * np.pi * f * t[a:b])
    beat = (np.sin(2 * np.pi * 2 * t) > 0.9) * np.sin(2 * np.pi * 60 * t)  # kick-ish pulse
    out = out / 6 + 0.4 * beat + 0.02 * np.random.default_rng(2).normal(size=n)
    return 0.6 * out / np.max(np.abs(out))


STREAMS = {}
META_INT = 16000


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, *a):
        pass

    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/redirect":
            self.send_response(302)
            self.send_header("Location", "/music.mp3")
            self.end_headers()
            return
        if path == "/list.pls":
            host = self.headers.get("Host")
            body = f"[playlist]\nNumberOfEntries=1\nFile1=http://{host}/speech.mp3\nTitle1=Speech\n".encode()
            self.send_response(200)
            self.send_header("Content-Type", "audio/x-scpls")
            self.end_headers()
            self.wfile.write(body)
            return
        if path.startswith("/json/stations/search"):
            host = self.headers.get("Host")
            data = [
                {"name": "Fake Talk é", "url": "x", "url_resolved": f"http://{host}/speech.mp3", "codec": "MP3",
                 "bitrate": 128, "tags": "talk,news", "countrycode": "NZ"},
                {"name": "Fake Music", "url_resolved": f"http://{host}/music.mp3", "codec": "MP3", "bitrate": 128,
                 "tags": "ambient", "countrycode": "IS", "votes": 3, "geo": {"lat": 1.5}},
                {"name": "AAC skip", "url_resolved": f"http://{host}/x.aac", "codec": "AAC", "bitrate": 64},
            ]
            body = json.dumps(data).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for i in range(0, len(body), 100):  # chunked, to exercise the decoder
                part = body[i:i + 100]
                self.wfile.write(b"%x\r\n" % len(part) + part + b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
            return
        name = path.strip("/")
        if name not in STREAMS:
            self.send_error(404)
            return
        icy = self.headers.get("Icy-MetaData") == "1"
        self.send_response(200)
        self.send_header("Content-Type", "audio/mpeg")
        self.send_header("icy-name", name)
        self.send_header("icy-br", "128")
        if icy:
            self.send_header("icy-metaint", str(META_INT))
        self.end_headers()
        data = STREAMS[name]
        pos, until, song = 0, META_INT, 0
        bytes_per_sec = 128000 // 8
        start = time.time()
        sent = 0
        try:
            while True:
                n = min(4096, until if icy else 4096)
                chunk = data[pos:pos + n]
                if len(chunk) < n:
                    chunk += data[: n - len(chunk)]
                pos = (pos + n) % len(data)
                self.wfile.write(chunk)
                sent += n
                if icy:
                    until -= n
                    if until == 0:
                        song += 1
                        meta = f"StreamTitle='{name} test tone {song}';".encode()
                        pad = (16 - len(meta) % 16) % 16
                        meta += b"\0" * pad
                        self.wfile.write(bytes([len(meta) // 16]) + meta)
                        until = META_INT
                # pace a little faster than real time after a 3 s burst, like real servers
                ahead = sent / bytes_per_sec - (time.time() - start) - 3
                if ahead > 0:
                    time.sleep(min(ahead, 0.2))
        except (BrokenPipeError, ConnectionResetError):
            pass


class Server(socketserver.ThreadingMixIn, socketserver.TCPServer):
    allow_reuse_address = True
    daemon_threads = True


def make_certs(d):
    os.makedirs(d, exist_ok=True)
    run = lambda *a: subprocess.run(a, check=True, capture_output=True)
    run("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", f"{d}/ca.key", "-out", f"{d}/ca.pem",
        "-days", "2", "-subj", "/CN=RadioScan Test CA")
    run("openssl", "req", "-newkey", "rsa:2048", "-nodes", "-keyout", f"{d}/srv.key", "-out", f"{d}/srv.csr",
        "-subj", "/CN=localhost")
    with open(f"{d}/ext.cnf", "w") as f:
        f.write("subjectAltName=DNS:localhost\n")
    run("openssl", "x509", "-req", "-in", f"{d}/srv.csr", "-CA", f"{d}/ca.pem", "-CAkey", f"{d}/ca.key",
        "-CAcreateserial", "-out", f"{d}/srv.pem", "-days", "2", "-extfile", f"{d}/ext.cnf")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--tls-port", type=int, default=0)
    ap.add_argument("--certdir", default="/tmp/radioscan-certs")
    a = ap.parse_args()
    STREAMS["speech.mp3"] = encode(speech_like())
    STREAMS["music.mp3"] = encode(music_like())
    srv = Server(("127.0.0.1", a.port), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    if a.tls_port:
        make_certs(a.certdir)
        tls = Server(("127.0.0.1", a.tls_port), Handler)
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(f"{a.certdir}/srv.pem", f"{a.certdir}/srv.key")
        tls.socket = ctx.wrap_socket(tls.socket, server_side=True)
        threading.Thread(target=tls.serve_forever, daemon=True).start()
    print("ready", flush=True)
    while True:
        time.sleep(3600)


if __name__ == "__main__":
    main()
