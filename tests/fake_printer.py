#!/usr/bin/env python3
"""Minimal fake Bambu printer for integration tests (stdlib only).

TLS MQTT on --mqtt-port (CONNACK 5 on wrong password, echoes pushall with a
report, acks project_file) and the A1 camera protocol on --camera-port.
Certificate CN is the serial, like a real printer.

    fake_printer.py --cert c.pem --key k.pem --code 12345678 --serial 03900A000000001
"""
import argparse
import json
import socket
import ssl
import struct
import threading
import time

TINY_JPEG = bytes.fromhex(
    "ffd8ffe000104a46494600010100000100010000"
    "ffc000110801e0028003012200021101031101"  # SOF0 height 480, width 640
    "ffd9"
)


def read_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("closed")
        buf += chunk
    return buf


def read_packet(sock):
    header = read_exact(sock, 1)[0]
    mult, length = 1, 0
    while True:
        b = read_exact(sock, 1)[0]
        length += (b & 0x7F) * mult
        mult *= 128
        if not b & 0x80:
            break
    return header, read_exact(sock, length) if length else b""


def encode_len(n):
    out = bytearray()
    while True:
        b = n % 128
        n //= 128
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


def mqtt_str(s):
    b = s.encode()
    return struct.pack(">H", len(b)) + b


def publish(sock, lock, topic, payload):
    body = mqtt_str(topic) + payload.encode()
    with lock:
        sock.sendall(bytes([0x30]) + encode_len(len(body)) + body)


def handle_mqtt(conn, args):
    lock = threading.Lock()
    header, body = read_packet(conn)
    assert header & 0xF0 == 0x10, "expected CONNECT"
    pos = 2 + 4 + 1  # "MQTT" + level
    flags = body[pos]
    pos += 1 + 2
    def take():
        nonlocal pos
        (n,) = struct.unpack(">H", body[pos:pos + 2])
        s = body[pos + 2:pos + 2 + n].decode()
        pos += 2 + n
        return s
    take()  # client id
    user = take() if flags & 0x80 else ""
    password = take() if flags & 0x40 else ""
    ok = user == "bblp" and password == args.code
    conn.sendall(bytes([0x20, 2, 0, 0 if ok else 5]))
    print(f"mqtt: CONNECT user={user} ok={ok}", flush=True)
    if not ok:
        return
    report = f"device/{args.serial}/report"
    while True:
        header, body = read_packet(conn)
        kind = header & 0xF0
        if kind == 0x80:  # SUBSCRIBE
            conn.sendall(bytes([0x90, 3]) + body[:2] + b"\x00")
        elif kind == 0xC0:  # PINGREQ
            conn.sendall(bytes([0xD0, 0]))
        elif kind == 0xE0:
            return
        elif kind == 0x30:
            qos = (header >> 1) & 3
            (tlen,) = struct.unpack(">H", body[:2])
            pos = 2 + tlen + (2 if qos else 0)
            if qos:
                conn.sendall(bytes([0x40, 2]) + body[2 + tlen:4 + tlen])
            msg = json.loads(body[pos:])
            print(f"mqtt: request {json.dumps(msg)[:200]}", flush=True)
            if "pushing" in msg:
                publish(conn, lock, report, json.dumps(
                    {"print": {"command": "push_status", "gcode_state": "IDLE", "nozzle_temper": 25.0,
                               "sequence_id": "1", "msg": 0}}))
            elif msg.get("print", {}).get("command") == "project_file":
                seq = msg["print"]["sequence_id"]
                publish(conn, lock, report, json.dumps(
                    {"print": {"command": "project_file", "sequence_id": seq, "result": "success",
                               "param": msg["print"]["param"]}}))


def handle_camera(conn, args):
    auth = read_exact(conn, 80)
    magic, kind = struct.unpack("<II", auth[:8])
    user = auth[16:48].rstrip(b"\0").decode()
    password = auth[48:80].rstrip(b"\0").decode()
    ok = magic == 0x40 and kind == 0x3000 and user == "bblp" and password == args.code
    print(f"camera: auth ok={ok}", flush=True)
    if not ok:
        return
    for _ in range(args.frames):
        conn.sendall(struct.pack("<IIII", len(TINY_JPEG), 0, 1, 0) + TINY_JPEG)
        time.sleep(0.5)


def serve(port, ctx, handler, args):
    srv = socket.create_server(("127.0.0.1", port), reuse_port=True)
    while True:
        raw, _ = srv.accept()
        def run(raw=raw):
            try:
                with ctx.wrap_socket(raw, server_side=True) as conn:
                    handler(conn, args)
            except (ConnectionError, ssl.SSLError, OSError, AssertionError) as exc:
                print(f"{handler.__name__}: {exc}", flush=True)
        threading.Thread(target=run, daemon=True).start()


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--cert", required=True)
    p.add_argument("--key", required=True)
    p.add_argument("--code", required=True)
    p.add_argument("--serial", required=True)
    p.add_argument("--mqtt-port", type=int, default=8883)
    p.add_argument("--camera-port", type=int, default=6000)
    p.add_argument("--frames", type=int, default=20)
    args = p.parse_args()
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(args.cert, args.key)
    threading.Thread(target=serve, args=(args.camera_port, ctx, handle_camera, args), daemon=True).start()
    print("fake printer ready", flush=True)
    serve(args.mqtt_port, ctx, handle_mqtt, args)


if __name__ == "__main__":
    main()
