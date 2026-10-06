#!/usr/bin/env python3
"""Serve a WiiCompiled web build.

    serve.py <web build directory> [--data <extracted DATA directory>] [--port 8000]
             [--host 127.0.0.1] [--cert server.pem --key server-key.pem [--ca ca.pem]]
             [--unlock-all] [--lowres] [--remote-log]

The build is served at /, and with --data the game's DATA directory at /data/ together with a
generated /data/manifest.json. / itself redirects to the game page, with ?data=data when there is
data (and ?unlock with --unlock-all, which unlocks everything in the save), so
http://127.0.0.1:8000/ starts the game without picking a folder. Byte-range requests are supported
(Python's stock server ignores them, and the game reads files in ranges).

By default it only listens on 127.0.0.1. --host 0.0.0.0 makes it reachable from other machines on
your network, for playing on your own devices; never expose it to the internet, since the build
and the data are derived from your copy of the game. Browsers only enable WebGPU on secure
origins (https:// or localhost), so another machine needs HTTPS: pass a certificate with --cert
and --key. make_cert.sh (next to this file) makes one signed by a local certificate authority;
with --ca, devices that refuse to go past the browser's warning (iPhones) can install and trust
that authority from http://<address>:<port>/ca.crt, which the plain-http page links to.
"""

import argparse
import email.utils
import gzip
import html
import http.server
import ipaddress
import shutil
import ssl
import json
import os
import re
import socket
import sys
import threading
import urllib.parse


def build_manifest(root):
    entries = []
    for directory, _, files in os.walk(root):
        for name in files:
            path = os.path.join(directory, name)
            relative = os.path.relpath(path, root).replace(os.sep, "/")
            entries.append({"path": relative, "size": os.path.getsize(path)})
    entries.sort(key=lambda entry: entry["path"])
    return json.dumps(entries).encode()


# The build's large outputs go out gzip-compressed to browsers that accept it (the .wasm shrinks
# about fourfold). The compressed copy is made on first request and kept next to the file, and
# remade when the file is rebuilt.
COMPRESSED_SUFFIXES = (".wasm", ".data", ".js")
compress_lock = threading.Lock()


def gzip_copy(path):
    compressed = path + ".gz"
    with compress_lock:
        if not os.path.exists(compressed) or os.path.getmtime(compressed) < os.path.getmtime(path):
            print(f"compressing {path}", flush=True)
            with open(path, "rb") as source, gzip.open(compressed + ".tmp", "wb", compresslevel=6) as target:
                shutil.copyfileobj(source, target, 1 << 20)
            os.replace(compressed + ".tmp", compressed)
    return compressed


def make_handler(build_root, data_root, unlock, remote_log, lowres=False):
    manifest = build_manifest(data_root) if data_root else None
    # / leads to the game, with the served data (and ?unlock, ?log), so the address to type is
    # just host:port.
    pages = sorted(name for name in os.listdir(build_root) if name.endswith(".html"))
    page = "WiiCompiled.html" if "WiiCompiled.html" in pages else pages[0] if len(pages) == 1 else None
    query = "&".join(["data=data"] * bool(data_root) + ["unlock"] * unlock + ["lowres"] * lowres +
                     ["log"] * remote_log)
    start_page = page and "/" + page + ("?" + query if query else "")

    class Handler(http.server.SimpleHTTPRequestHandler):
        # Keep-alive: the game reads the disc in many small ranges, and over HTTPS a new connection
        # per read would also mean a new TLS handshake per read.
        protocol_version = "HTTP/1.1"
        timeout = 60
        extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map,
                          ".wasm": "application/wasm", ".js": "text/javascript"}

        def translate_path(self, path):
            path = urllib.parse.unquote(urllib.parse.urlsplit(path).path)
            if data_root and (path == "/data" or path.startswith("/data/")):
                relative = path[len("/data"):].lstrip("/")
                base = data_root
            else:
                relative = path.lstrip("/")
                base = build_root
            resolved = os.path.realpath(os.path.join(base, relative))
            if resolved != base and not resolved.startswith(base + os.sep):
                return os.path.join(base, "__outside__")  # 404, never escape the root
            return resolved

        def do_GET(self):
            if start_page and urllib.parse.urlsplit(self.path).path == "/":
                # Options added to / (/?novsync) are passed on to the game page; /?pick leaves out
                # the served data, so the page offers its picker (a disc image, a folder) instead.
                extra = urllib.parse.urlsplit(self.path).query
                page_url = start_page
                if "pick" in urllib.parse.parse_qs(extra, keep_blank_values=True):
                    page_url = re.sub(r"(?<=[?&])data=data(&|$)", "", page_url).rstrip("?&")
                self.send_response(302)
                self.send_header("Location", page_url + (("&" if "?" in page_url else "?") + extra
                                                         if extra else ""))
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            if manifest is not None and urllib.parse.urlsplit(self.path).path == "/data/manifest.json":
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(manifest)))
                self.end_headers()
                self.wfile.write(manifest)
                return
            range_header = self.headers.get("Range")
            path = self.translate_path(self.path)
            if (not range_header and path.endswith(COMPRESSED_SUFFIXES) and os.path.isfile(path)
                    and path.startswith(build_root + os.sep)
                    and "gzip" in self.headers.get("Accept-Encoding", "")):
                return self.send_gzip(path)
            match = re.fullmatch(r"bytes=(\d+)-(\d*)", range_header or "")
            if not match or not os.path.isfile(path):
                return super().do_GET()
            size = os.path.getsize(path)
            start = int(match.group(1))
            end = int(match.group(2)) if match.group(2) else size - 1
            if start >= size or end < start:
                self.send_response(416)
                self.send_header("Content-Range", f"bytes */{size}")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            end = min(end, size - 1)
            self.send_response(206)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
            self.send_header("Content-Length", str(end - start + 1))
            self.end_headers()
            with open(path, "rb") as file:
                file.seek(start)
                remaining = end - start + 1
                while remaining:
                    chunk = file.read(min(remaining, 1 << 20))
                    if not chunk:
                        break  # the file shrank; the client sees a short body
                    self.wfile.write(chunk)
                    remaining -= len(chunk)

        def end_headers(self):
            # The page, its script and its wasm must come from the same build: a browser reusing a
            # cached .wasm next to a newer script crashes at startup. Revalidate every load (an
            # unchanged file costs a 304).
            if not self.path.startswith("/data/"):
                self.send_header("Cache-Control", "no-cache")
            super().end_headers()

        def send_gzip(self, path):
            modified = int(os.path.getmtime(path))
            since = self.headers.get("If-Modified-Since")
            try:
                unchanged = since and email.utils.parsedate_to_datetime(since).timestamp() >= modified
            except (TypeError, ValueError):
                unchanged = False
            if unchanged:
                self.send_response(304)
                self.end_headers()
                return
            compressed = gzip_copy(path)
            self.send_response(200)
            self.send_header("Content-Type", self.guess_type(path))
            self.send_header("Content-Encoding", "gzip")
            self.send_header("Vary", "Accept-Encoding")
            self.send_header("Content-Length", str(os.path.getsize(compressed)))
            self.send_header("Last-Modified", self.date_time_string(modified))
            self.end_headers()
            with open(compressed, "rb") as file:
                shutil.copyfileobj(file, self.wfile, 1 << 20)

        def do_POST(self):
            # With --remote-log, the page posts its output here (see ?log in shell.html).
            length = int(self.headers.get("Content-Length") or 0)
            body = self.rfile.read(length).decode("utf-8", "replace") if length else ""
            if not remote_log or urllib.parse.urlsplit(self.path).path != "/log":
                return self.send_error(404)
            for line in body.splitlines():
                print(f"[page {self.client_address[0]}] {line}", flush=True)
            self.send_response(204)
            self.send_header("Content-Length", "0")
            self.end_headers()

        def log_request(self, code="-", size="-"):
            if not (remote_log and self.command == "POST" and code == 204):
                super().log_request(code, size)

    return Handler


class Server(http.server.ThreadingHTTPServer):
    """Runs each connection on its own thread, TLS handshake included.

    Wrapping the listening socket instead would do every handshake inside accept(), on the one
    thread that accepts connections: a single client that connects and then stalls (a browser's
    speculative preconnect, a phone that rejects the certificate) freezes the server for everyone.
    """

    tls_context = None
    ca_certificate = None  # DER bytes of --ca, offered at http://<address>:<port>/ca.crt

    def handle_error(self, request, client_address):
        # A browser that closes a connection mid-reply (a reload, an idle keep-alive connection
        # dropped) is routine, not a server error worth a traceback.
        if isinstance(sys.exc_info()[1], (ConnectionResetError, BrokenPipeError)):
            return
        super().handle_error(request, client_address)

    def finish_request(self, request, client_address):
        if self.tls_context is None:
            return super().finish_request(request, client_address)
        request.settimeout(10)
        try:
            # A TLS connection opens with a handshake record (0x16). Anything else is plain
            # http:// sent to this port, which is what Safari falls back to when the address was
            # typed without https:// and the certificate was refused.
            if request.recv(1, socket.MSG_PEEK) != b"\x16":
                return self.point_to_https(request, client_address)
            tls = self.tls_context.wrap_socket(request, server_side=True)
        except OSError as error:
            print(f"{client_address[0]}: TLS handshake failed ({error}). If that device shows a "
                  "certificate warning, it has to accept the certificate first.")
            return
        try:
            super().finish_request(tls, client_address)
        finally:
            self.shutdown_request(tls)

    def point_to_https(self, request, client_address):
        head = request.recv(4096).decode("latin-1")
        if not head:
            return
        parts = head.split(" ", 2)
        target = parts[1] if len(parts) == 3 and parts[1].startswith("/") else "/"
        address = "%s:%d" % request.getsockname()[:2]
        if target == "/ca.crt" and self.ca_certificate:
            # DER with this type: iOS offers it as a configuration profile to install.
            print(f"{client_address[0]}: sent the CA certificate")
            request.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: application/x-x509-ca-cert\r\n"
                            b"Content-Disposition: attachment; filename=\"WiiCompiled-local-CA.crt\"\r\n"
                            b"Content-Length: %d\r\nConnection: close\r\n\r\n%s"
                            % (len(self.ca_certificate), self.ca_certificate))
            return
        url = "https://%s%s" % (address, target)
        print(f"{client_address[0]}: plain http:// on the https port; answered with a link to {url}")
        link = html.escape(url)
        body = (f'<!doctype html><meta charset="utf-8"><meta name="viewport" '
                f'content="width=device-width, initial-scale=1"><title>WiiCompiled</title>'
                f'<p>This server only answers https://. Open <a href="{link}">{link}</a> and, if '
                f'the browser warns about the certificate, accept it: it belongs to the computer '
                f'serving the game.</p>')
        if self.ca_certificate:
            ca = html.escape("http://%s/ca.crt" % address)
            body += (f'<p>If the browser does not let you past the warning (iPhone), install the '
                     f'certificate authority that signed it: open <a href="{ca}">{ca}</a> and allow '
                     f'the download, then Settings &gt; General &gt; VPN &amp; Device Management &gt; '
                     f'WiiCompiled local CA &gt; Install, then Settings &gt; General &gt; About &gt; '
                     f'Certificate Trust Settings &gt; turn on WiiCompiled local CA. Reopen the '
                     f'https:// address after that.</p>')
        body = body.encode()
        request.sendall(b"HTTP/1.1 400 Bad Request\r\nContent-Type: text/html; charset=utf-8\r\n"
                        b"Content-Length: %d\r\nConnection: close\r\n\r\n%s" % (len(body), body))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("build", help="web build directory (contains the .html/.js/.wasm)")
    parser.add_argument("--data", help="extracted Mario Kart Wii DATA directory (files/, sys/)")
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--host", default="127.0.0.1",
                        help="address to listen on (default 127.0.0.1; 0.0.0.0 for your local network)")
    parser.add_argument("--unlock-all", action="store_true",
                        help="make / open the game with ?unlock: everything unlocked in the save")
    parser.add_argument("--lowres", action="store_true",
                        help="make / open the game with ?lowres: 1x canvas and the Wii's own render resolution")
    parser.add_argument("--remote-log", action="store_true",
                        help="make / open the game with ?log and print what the page sends to /log "
                             "(page output, errors, startup steps), for devices without a console")
    parser.add_argument("--cert", help="TLS certificate (PEM); serves HTTPS together with --key")
    parser.add_argument("--key", help="TLS private key (PEM)")
    parser.add_argument("--ca", help="certificate authority that signed --cert (PEM), offered at "
                                     "http://<address>:<port>/ca.crt for devices to install")
    args = parser.parse_args()
    if bool(args.cert) != bool(args.key):
        parser.error("--cert and --key go together")
    if args.ca and not args.cert:
        parser.error("--ca needs --cert and --key")
    build_root = os.path.realpath(args.build)
    data_root = os.path.realpath(args.data) if args.data else None
    server = Server((args.host, args.port), make_handler(build_root, data_root, args.unlock_all, args.remote_log, args.lowres))
    scheme = "http"
    if args.cert:
        server.tls_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        server.tls_context.load_cert_chain(args.cert, args.key)
        if args.ca:
            with open(args.ca) as ca:
                server.ca_certificate = ssl.PEM_cert_to_DER_cert(ca.read())
        scheme = "https"
    if not ipaddress.ip_address(args.host).is_loopback:
        print("WARNING: listening beyond this machine. Keep it on your own network; do not expose it "
              "to the internet.")
        if scheme == "http":
            print("WARNING: without --cert/--key, other machines get no WebGPU (browsers require "
                  "https:// or localhost), so the game will not start there.")
    print(f"Serving {build_root} on {scheme}://{args.host}:{args.port}/ (/ opens the game"
          + (" with the served data)" if data_root else ")"))
    server.serve_forever()


if __name__ == "__main__":
    main()
