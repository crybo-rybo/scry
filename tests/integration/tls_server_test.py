#!/usr/bin/env python3

"""Exercise Scry against a deterministic self-signed HTTPS loopback server."""

from __future__ import annotations

import argparse
import socket
import ssl
import subprocess
import threading
from pathlib import Path


STREAM = """event: message_start
data: {"type":"message_start","message":{"type":"message","role":"assistant","content":[],"model":"tls-test-model","stop_reason":null,"usage":{"input_tokens":2,"output_tokens":0}}}

event: content_block_start
data: {"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}}

event: content_block_delta
data: {"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"TLS mock"}}

event: content_block_stop
data: {"type":"content_block_stop","index":0}

event: message_delta
data: {"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":2}}

event: message_stop
data: {"type":"message_stop"}

""".encode()


def response() -> bytes:
    headers = (
        b"HTTP/1.1 200 OK\r\n"
        b"Content-Type: text/event-stream\r\n"
        b"Connection: close\r\n"
        + f"Content-Length: {len(STREAM)}\r\n\r\n".encode()
    )
    return headers + STREAM


def receive_request(connection: ssl.SSLSocket) -> None:
    payload = b""
    while b"\r\n\r\n" not in payload:
        chunk = connection.recv(4096)
        if not chunk:
            return
        payload += chunk
    head, body = payload.split(b"\r\n\r\n", 1)
    content_length = 0
    for line in head.split(b"\r\n"):
        if line.lower().startswith(b"content-length:"):
            content_length = int(line.split(b":", 1)[1].strip())
    while len(body) < content_length:
        chunk = connection.recv(4096)
        if not chunk:
            return
        body += chunk


class SelfSignedServer:
    def __init__(self, certificate: Path, key: Path) -> None:
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(4)
        self.listener.settimeout(0.1)
        self.context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.context.load_cert_chain(certificate, key)
        self.accepted = 0
        self.errors: list[str] = []
        self.stopped = threading.Event()
        self.thread = threading.Thread(target=self.serve, daemon=True)

    @property
    def url(self) -> str:
        return f"https://127.0.0.1:{self.listener.getsockname()[1]}"

    def serve(self) -> None:
        while not self.stopped.is_set() and self.accepted < 2:
            try:
                raw, _ = self.listener.accept()
            except TimeoutError:
                continue
            self.accepted += 1
            try:
                with raw:
                    with self.context.wrap_socket(raw, server_side=True) as secured:
                        receive_request(secured)
                        secured.sendall(response())
            except ssl.SSLError:
                if self.accepted != 1:
                    self.errors.append("unexpected TLS handshake failure")
            except OSError as error:
                self.errors.append(str(error))

    def close(self) -> None:
        self.stopped.set()
        self.thread.join(timeout=2)
        self.listener.close()


def run_probe(probe: Path, url: str, mode: str) -> None:
    completed = subprocess.run(
        [probe, url, mode],
        check=False,
        capture_output=True,
        text=True,
        timeout=5,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"{mode} probe failed ({completed.returncode}): "
            f"{completed.stdout}{completed.stderr}"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--cert", type=Path, required=True)
    parser.add_argument("--key", type=Path, required=True)
    args = parser.parse_args()

    server = SelfSignedServer(args.cert, args.key)
    server.thread.start()
    try:
        run_probe(args.probe, server.url, "verify")
        run_probe(args.probe, server.url, "insecure")
    finally:
        server.close()
    if server.accepted != 2 or server.errors:
        raise RuntimeError(
            f"expected two TLS connections, got {server.accepted}: {server.errors}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
