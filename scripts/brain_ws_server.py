#!/usr/bin/env python3
"""
Minimal WSS (RFC6455) test endpoint for PoP -> brain publishing.

This server:
- accepts TLS connections (WSS)
- performs WebSocket handshake
- reads masked client text frames
- prints received JSON lines (or writes them to a file)

No external Python dependencies.

Example:
  # Generate a self-signed cert for localhost:
  openssl req -x509 -newkey rsa:2048 -nodes -days 365 \
    -keyout /tmp/brain_key.pem -out /tmp/brain_cert.pem \
    -subj "/CN=localhost"

  # Run the server:
  python3 scripts/brain_ws_server.py --host 127.0.0.1 --port 8443 \
    --certfile /tmp/brain_cert.pem --keyfile /tmp/brain_key.pem

  # Run the PoP (brain publisher must skip verification for self-signed):
  ./build/pop ... --brain_ws_host 127.0.0.1 --brain_ws_port 8443 --brain_ws_path / --brain_ws_insecure
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import socket
import ssl
import threading
from typing import Optional, Tuple


GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def _recv_until(sock: socket.socket, marker: bytes, max_bytes: int = 65536) -> bytes:
    data = b""
    while marker not in data:
        chunk = sock.recv(4096)
        if not chunk:
            break
        data += chunk
        if len(data) > max_bytes:
            raise ValueError("header too large")
    return data


def _parse_http_headers(raw: bytes) -> Tuple[str, dict]:
    head, _, _ = raw.partition(b"\r\n\r\n")
    lines = head.split(b"\r\n")
    if not lines:
        raise ValueError("empty request")
    request_line = lines[0].decode("ascii", errors="replace")
    headers: dict[str, str] = {}
    for ln in lines[1:]:
        if b":" not in ln:
            continue
        k, v = ln.split(b":", 1)
        headers[k.decode("ascii", errors="replace").strip().lower()] = v.decode("ascii", errors="replace").strip()
    return request_line, headers


def _ws_accept_value(sec_key: str) -> str:
    digest = hashlib.sha1((sec_key + GUID).encode("ascii")).digest()
    return base64.b64encode(digest).decode("ascii")


def _send_ws_frame(sock: socket.socket, opcode: int, payload: bytes) -> None:
    fin_opcode = 0x80 | (opcode & 0x0F)
    length = len(payload)
    if length < 126:
        header = bytes([fin_opcode, length])
    elif length < (1 << 16):
        header = bytes([fin_opcode, 126]) + length.to_bytes(2, "big")
    else:
        header = bytes([fin_opcode, 127]) + length.to_bytes(8, "big")
    sock.sendall(header + payload)


def _recv_exact(sock: socket.socket, n: int) -> bytes:
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("socket closed")
        buf += chunk
    return buf


def _recv_ws_frame_raw(sock: socket.socket) -> Tuple[bool, int, bytes]:
    hdr = _recv_exact(sock, 2)
    b0, b1 = hdr[0], hdr[1]
    fin = (b0 & 0x80) != 0
    opcode = b0 & 0x0F
    masked = (b1 & 0x80) != 0
    length = b1 & 0x7F

    if length == 126:
        length = int.from_bytes(_recv_exact(sock, 2), "big")
    elif length == 127:
        length = int.from_bytes(_recv_exact(sock, 8), "big")

    mask_key = b""
    if masked:
        mask_key = _recv_exact(sock, 4)

    payload = _recv_exact(sock, length) if length else b""
    if masked and payload:
        payload = bytes(b ^ mask_key[i % 4] for i, b in enumerate(payload))

    if not fin:
        # continuation handled by _recv_ws_message()
        return fin, opcode, payload
    return fin, opcode, payload


def _recv_ws_message(sock: socket.socket) -> Tuple[int, bytes]:
    """
    Return one complete WS message (text/binary) or a control frame (ping/pong/close).

    Supports fragmentation: opcode=0x1/0x2 start, opcode=0x0 continuation until FIN.
    Control frames may appear between fragments.
    """
    fin, opcode, payload = _recv_ws_frame_raw(sock)

    # Control frames are never fragmented (RFC6455).
    if opcode in (0x8, 0x9, 0xA):
        return opcode, payload

    if opcode == 0x0:
        raise ValueError("unexpected continuation frame")

    # Data message (possibly fragmented).
    if fin:
        return opcode, payload

    parts = [payload]
    msg_opcode = opcode
    while True:
        fin2, op2, pl2 = _recv_ws_frame_raw(sock)
        if op2 == 0x9:  # ping
            _send_ws_frame(sock, 0xA, pl2)
            continue
        if op2 == 0xA:  # pong
            continue
        if op2 == 0x8:  # close
            _send_ws_frame(sock, 0x8, b"")
            raise ConnectionError("peer closed during fragmented message")
        if op2 != 0x0:
            raise ValueError(f"unexpected opcode during fragmentation: {op2}")
        parts.append(pl2)
        if fin2:
            return msg_opcode, b"".join(parts)


def _handle_client(conn: socket.socket, addr: Tuple[str, int], out_path: Optional[str], pretty: bool) -> None:
    try:
        req = _recv_until(conn, b"\r\n\r\n")
        request_line, headers = _parse_http_headers(req)
        if not request_line.startswith("GET "):
            raise ValueError("not a websocket GET request")

        sec_key = headers.get("sec-websocket-key")
        if not sec_key:
            raise ValueError("missing sec-websocket-key")

        accept_val = _ws_accept_value(sec_key)
        resp = (
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Accept: {accept_val}\r\n"
            "\r\n"
        )
        conn.sendall(resp.encode("ascii"))
        print(f"[brain] connected addr={addr[0]}:{addr[1]}")

        fh = open(out_path, "a", encoding="utf-8") if out_path else None
        try:
            while True:
                opcode, payload = _recv_ws_message(conn)
                if opcode == 0x1:  # text
                    text = payload.decode("utf-8", errors="replace")
                    if pretty:
                        try:
                            obj = json.loads(text)
                            line = json.dumps(obj, indent=2, sort_keys=True)
                        except Exception:
                            line = text
                    else:
                        line = text

                    if fh:
                        fh.write(text)
                        fh.write("\n")
                        fh.flush()
                    print(f"[brain] msg_bytes={len(payload)} {line[:2000]}")
                elif opcode == 0x8:  # close
                    _send_ws_frame(conn, 0x8, b"")
                    return
                elif opcode == 0x9:  # ping
                    _send_ws_frame(conn, 0xA, payload)  # pong
                elif opcode == 0xA:  # pong
                    continue
                else:
                    # ignore binary/continuation/etc
                    continue
        finally:
            if fh:
                fh.close()
    except Exception as e:
        print(f"[brain] client_error addr={addr[0]}:{addr[1]} err={e}")
    finally:
        try:
            conn.close()
        except Exception:
            pass


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8443)
    ap.add_argument("--certfile", required=True, help="TLS cert PEM (self-signed ok)")
    ap.add_argument("--keyfile", required=True, help="TLS key PEM")
    ap.add_argument("--out", default=None, help="Append raw JSON messages to this file")
    ap.add_argument("--pretty", action="store_true", help="Pretty-print JSON to stdout")
    args = ap.parse_args()

    if not os.path.exists(args.certfile):
        raise SystemExit(f"certfile not found: {args.certfile}")
    if not os.path.exists(args.keyfile):
        raise SystemExit(f"keyfile not found: {args.keyfile}")

    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(certfile=args.certfile, keyfile=args.keyfile)

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind((args.host, args.port))
        s.listen(128)
        print(f"[brain] listening wss://{args.host}:{args.port}/ (cert={args.certfile})")

        while True:
            raw_conn, addr = s.accept()
            try:
                conn = ctx.wrap_socket(raw_conn, server_side=True)
            except Exception as e:
                print(f"[brain] tls_handshake_error addr={addr[0]}:{addr[1]} err={e}")
                raw_conn.close()
                continue

            t = threading.Thread(
                target=_handle_client,
                args=(conn, addr, args.out, args.pretty),
                daemon=True,
            )
            t.start()


if __name__ == "__main__":
    raise SystemExit(main())
