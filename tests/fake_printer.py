#!/usr/bin/env python3
"""Minimal fake Bambu printer for integration tests (stdlib only).

TLS MQTT on --mqtt-port (CONNACK 5 on wrong password, echoes pushall with a
report, acks project_file), the A1 camera protocol on --camera-port and, with
--h264, the X1-style RTSPS camera on --rtsp-port (Digest auth, H.264 over RTP
interleaved on the RTSP connection; SPS/PPS only in the SDP).
Certificate CN is the serial, like a real printer.

    fake_printer.py --cert c.pem --key k.pem --code 12345678 --serial 03900A000000001
"""
import argparse
import base64
import hashlib
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


def h264_nals(path):
    data = open(path, "rb").read()
    out, start, i = [], None, 0
    while i + 3 <= len(data):
        if data[i:i + 3] == b"\0\0\1":
            if start is not None:
                out.append(data[start:i].rstrip(b"\0"))
            start = i = i + 3
            continue
        i += 1
    if start is not None:
        out.append(data[start:])
    return out


def rtp_packets(nal, seq, ts, last, mtu=1000):
    """RFC 6184 packets for one NAL (single NAL unit or FU-A)."""
    def hdr(marker):
        return struct.pack(">BBHII", 0x80, (0x80 if marker else 0) | 96, seq[0] & 0xFFFF, ts, 0x1234)
    if len(nal) <= mtu:
        pkt = hdr(last) + nal
        seq[0] += 1
        return [pkt]
    pkts, body = [], nal[1:]
    for off in range(0, len(body), mtu):
        first, end = off == 0, off + mtu >= len(body)
        fu = bytes([(nal[0] & 0xE0) | 28, (0x80 if first else 0) | (0x40 if end else 0) | (nal[0] & 0x1F)])
        pkts.append(hdr(last and end) + fu + body[off:off + mtu])
        seq[0] += 1
    return pkts


def rtsp_read_request(conn, buf):
    while b"\r\n\r\n" not in buf[0]:
        chunk = conn.recv(4096)
        if not chunk:
            raise ConnectionError("rtsp client closed")
        buf[0] += chunk
    head, buf[0] = buf[0].split(b"\r\n\r\n", 1)
    lines = head.decode().split("\r\n")
    method, url, _ = lines[0].split(" ", 2)
    headers = {k.strip().lower(): v.strip() for k, v in (l.split(":", 1) for l in lines[1:] if ":" in l)}
    return method, url, headers


def digest_ok(headers, method, args, nonce, last_nc):
    auth = headers.get("authorization", "")
    if not auth.startswith("Digest "):
        return False
    f = dict((k.strip(), v.strip().strip('"')) for k, v in
             (p.split("=", 1) for p in auth[7:].split(",") if "=" in p))
    md5 = lambda s: hashlib.md5(s.encode()).hexdigest()
    ha1 = md5(f"{f.get('username')}:fake:{args.code}")
    ha2 = md5(f"{method}:{f.get('uri')}")
    if f.get("qop") == "auth":
        nc = int(f.get("nc", "0"), 16)
        if nc <= last_nc[0]:  # replayed nonce count
            return False
        last_nc[0] = nc
        want = md5(f"{ha1}:{nonce}:{f.get('nc')}:{f.get('cnonce')}:auth:{ha2}")
    else:
        want = md5(f"{ha1}:{nonce}:{ha2}")
    return f.get("username") == "bblp" and f.get("nonce") == nonce and f.get("response") == want


def handle_rtsp(conn, args):
    nals = h264_nals(args.h264)
    sprop = ",".join(base64.b64encode(n).decode() for n in nals if (n[0] & 0x1F) in (7, 8))
    frames, cur = [], []  # access units without SPS/PPS: those only go in the SDP
    for n in nals:
        if (n[0] & 0x1F) in (7, 8):
            continue
        cur.append(n)
        if (n[0] & 0x1F) in (1, 5):
            frames.append(cur)
            cur = []
    nonce, buf, last_nc = "f00dfeed", [b""], [0]
    seq, ts = [1], 0

    def send_au(au):
        for j, nal in enumerate(au):
            for pkt in rtp_packets(nal, seq, ts, j == len(au) - 1):
                conn.sendall(b"$\0" + struct.pack(">H", len(pkt)) + pkt)

    while True:
        method, url, headers = rtsp_read_request(conn, buf)
        cseq = headers.get("cseq", "0")
        if not digest_ok(headers, method, args, nonce, last_nc):
            print(f"rtsp: {method} unauthorized", flush=True)
            conn.sendall((f"RTSP/1.0 401 Unauthorized\r\nCSeq: {cseq}\r\n"
                          f'WWW-Authenticate: Digest realm="fake", nonce="{nonce}", qop="auth"\r\n\r\n').encode())
            continue
        print(f"rtsp: {method} ok", flush=True)
        if method == "DESCRIBE":
            sdp = ("v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=fake\r\nt=0 0\r\na=control:*\r\n"
                   "m=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\n"
                   f"a=fmtp:96 packetization-mode=1;sprop-parameter-sets={sprop}\r\na=control:trackID=1\r\n")
            conn.sendall((f"RTSP/1.0 200 OK\r\nCSeq: {cseq}\r\nContent-Type: application/sdp\r\n"
                          f"Content-Base: {url}/\r\nContent-Length: {len(sdp)}\r\n\r\n{sdp}").encode())
        elif method == "SETUP":
            conn.sendall((f"RTSP/1.0 200 OK\r\nCSeq: {cseq}\r\nSession: 5EC7;timeout=60\r\n"
                          "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n").encode())
        elif method == "PLAY":
            # Like some servers: the first keyframe goes out before the answer.
            send_au(frames[0])
            ts += 6000
            conn.sendall(f"RTSP/1.0 200 OK\r\nCSeq: {cseq}\r\nSession: 5EC7\r\n\r\n".encode())
            break
        else:
            conn.sendall(f"RTSP/1.0 200 OK\r\nCSeq: {cseq}\r\n\r\n".encode())
    for i in range(1, args.frames * 3):
        send_au(frames[i % len(frames)])
        ts += 6000  # 15 fps
        time.sleep(1 / 15)


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
    p.add_argument("--rtsp-port", type=int, default=8322)
    p.add_argument("--h264", help="Annex-B H.264 file for the RTSPS camera")
    p.add_argument("--frames", type=int, default=20)
    args = p.parse_args()
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(args.cert, args.key)
    threading.Thread(target=serve, args=(args.camera_port, ctx, handle_camera, args), daemon=True).start()
    if args.h264:
        threading.Thread(target=serve, args=(args.rtsp_port, ctx, handle_rtsp, args), daemon=True).start()
    print("fake printer ready", flush=True)
    serve(args.mqtt_port, ctx, handle_mqtt, args)


if __name__ == "__main__":
    main()
