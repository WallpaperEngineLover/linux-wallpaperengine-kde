#!/usr/bin/env python3
"""Stands in for api.github.com inside a test VM, for we_manager's update checks and downloads.

    fake_github.py <dir> [--release REPO=TAG:FILE[,FILE...]]... [--port 443]

A repo can have several --release options, newest first (the first one is its "latest"). A tag starting with "!" is only
listed, never "latest" (a helper release like libnode-arch, or a pre-release).

<dir> gets a CA and a certificate for api.github.com on the first run (ca.pem, cert.pem, key.pem; needs openssl, OPENSSL names another binary, openssl_proot.sh is used when there is none).
The VM resolves api.github.com to this server (/etc/hosts) and trusts ca.pem, e.g. NODE_EXTRA_CA_CERTS for
Electron's main process. GET /repos/<REPO>/releases/latest, /releases?per_page=N and /releases/tags/<TAG> answer with the releases given for it
(assets with size and a sha256 digest like GitHub's, downloaded from this server), every other repo with 404. Requests are logged to
stdout.
"""
import argparse
import hashlib
import http.server
import json
import os
import shutil
import ssl
import subprocess
import sys
import urllib.parse
from pathlib import Path

HOST = "api.github.com"


def make_certificate(folder):
    if (folder / "cert.pem").exists():
        return
    folder.mkdir(parents=True, exist_ok=True)

    def openssl(*args):
        subprocess.run([os.environ.get("OPENSSL") or shutil.which("openssl") or str(Path(__file__).with_name("openssl_proot.sh")), *args], check=True, cwd=folder, capture_output=True)

    openssl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "30", "-subj", "/CN=fake github test CA",
            "-keyout", "ca-key.pem", "-out", "ca.pem")
    openssl("req", "-newkey", "rsa:2048", "-nodes", "-subj", f"/CN={HOST}", "-keyout", "key.pem", "-out", "cert.csr")
    (folder / "ext.cnf").write_text(f"subjectAltName=DNS:{HOST}\nextendedKeyUsage=serverAuth\n")
    openssl("x509", "-req", "-in", "cert.csr", "-CA", "ca.pem", "-CAkey", "ca-key.pem", "-CAcreateserial", "-days", "30",
            "-extfile", "ext.cnf", "-out", "cert.pem")


def release(repo, spec, age):
    tag, _, files = spec.partition(":")
    listed_only = tag.startswith("!")
    tag = tag.lstrip("!")
    assets = []
    for name in filter(None, files.split(",")):
        path = Path(name).resolve()
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        assets.append({"name": path.name, "size": path.stat().st_size, "digest": f"sha256:{digest}",
                       "browser_download_url": f"https://{HOST}/download/{tag}/{path.name}", "path": str(path)})
    return {"tag_name": tag, "html_url": f"https://github.com/{repo}/releases/tag/{tag}", "body": f"Test release {tag}",
            "published_at": f"2026-10-{15 - age:02d}T12:00:00Z", "draft": False, "prerelease": False,
            "listed_only": listed_only, "assets": assets}


def public(entry):
    return {**{key: value for key, value in entry.items() if key != "listed_only"},
            "assets": [{key: value for key, value in asset.items() if key != "path"} for asset in entry["assets"]]}


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("dir", type=Path)
    parser.add_argument("--release", action="append", default=[], metavar="REPO=TAG:FILE[,FILE...]")
    parser.add_argument("--port", type=int, default=443)
    args = parser.parse_args()
    make_certificate(args.dir)

    releases = {}
    for item in args.release:
        repo, _, spec = item.partition("=")
        releases.setdefault(repo, []).append(release(repo, spec, len(releases.get(repo, []))))
    files = {f"/download/{entry['tag_name']}/{asset['name']}": asset["path"]
             for entries in releases.values() for entry in entries for asset in entry["assets"]}

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            print(self.command, self.path, flush=True)
            if self.path in files:
                data = Path(files[self.path]).read_bytes()
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)
                return
            parts = urllib.parse.urlsplit(self.path).path.strip("/").split("/")
            repo = "/".join(parts[1:3])
            entries = releases.get(repo, []) if parts[:1] == ["repos"] else []
            found = None
            if parts[3:] == ["releases", "latest"]:
                found = next((entry for entry in entries if not entry["listed_only"]), None)
            elif parts[3:] == ["releases"]:
                found = [public(entry) for entry in entries] if entries else None
            elif parts[3:5] == ["releases", "tags"] and len(parts) == 6:
                found = next((entry for entry in entries if entry["tag_name"] == urllib.parse.unquote(parts[5])), None)
            if found is not None:
                body = json.dumps(found if isinstance(found, list) else public(found)).encode()
                self.send_response(200)
            else:
                body = b'{"message": "Not Found"}'
                self.send_response(404)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *args):
            pass

    server = http.server.ThreadingHTTPServer(("0.0.0.0", args.port), Handler)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(args.dir / "cert.pem", args.dir / "key.pem")
    server.socket = context.wrap_socket(server.socket, server_side=True)
    print(f"serving {', '.join(releases) or 'no releases'} on port {args.port}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    sys.exit(main())
