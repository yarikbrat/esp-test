#!/usr/bin/env python3
# Raw-socket checks against the bench emulator (or the real bench): partial requests, junk,
# stalled requests, two clients on the merged stream, and log interval timing.
import argparse
import json
import socket
import sys
import time

FAILS = []


def check(cond, what):
    print(("ok    " if cond else "FAIL  ") + what)
    if not cond:
        FAILS.append(what)


def connect(host, port):
    s = socket.create_connection((host, port), timeout=5)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return s


def read_response(s, timeout=5.0):
    # Reads one response framed by Content-Length (the bridge never closes the connection).
    s.settimeout(timeout)
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = s.recv(4096)
        if not chunk:
            raise ConnectionError("closed")
        buf += chunk
    head, rest = buf.split(b"\r\n\r\n", 1)
    lines = head.decode("latin-1").split("\r\n")
    status = int(lines[0].split()[1])
    hdr = {k.lower(): v.strip() for k, v in (l.split(":", 1) for l in lines[1:])}
    n = int(hdr["content-length"])
    while len(rest) < n:
        chunk = s.recv(4096)
        if not chunk:
            raise ConnectionError("closed")
        rest += chunk
    return status, hdr, rest[:n], rest[n:]


def get(host, port, path):
    s = connect(host, port)
    s.sendall(f"GET {path} HTTP/1.1\r\nHost: x\r\n\r\n".encode())
    st, _, body, _ = read_response(s)
    s.close()
    return st, json.loads(body)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--log-interval", type=int, default=0, help="expected logging interval, 0 = skip")
    a = ap.parse_args()
    H, P = a.host, a.port

    s = connect(H, P)
    req = b"GET /api/state HTTP/1.1\r\nHost: x\r\n\r\n"
    for part in (req[:5], req[5:20], req[20:]):
        s.sendall(part)
        time.sleep(0.3)
    st, _, body, _ = read_response(s)
    check(st == 200 and json.loads(body)["baro"][0]["name"] == "BMP585", "request split in 3 TCP packets")

    s.sendall(b"\x00\xffgarbage\r\n\r\nGET /api/state HTTP/1.1\r\n\r\n")
    st1, _, _, rest = read_response(s)
    check(st1 == 400, "junk before a request gets 400")
    # The 200 may already be in the leftover buffer.
    s.settimeout(5)
    while b"\r\n\r\n" not in rest:
        rest += s.recv(4096)
    check(rest.startswith(b"HTTP/1.1 200"), "request after junk still served")
    s.close()

    s = connect(H, P)
    s.sendall(b"GET /api/sta")
    time.sleep(2.6)  # longer than HTTP_IDLE_TIMEOUT_MS
    s.sendall(b"GET /api/state HTTP/1.1\r\n\r\n")
    try:
        st, _, _, _ = read_response(s, timeout=6)
    except Exception as e:
        st = str(e)
    # After the timeout the stale prefix is dropped, so the full request parses on its own.
    check(st == 200, "stalled request dropped by timeout, next one served")
    s.close()

    a1, a2 = connect(H, P), connect(H, P)
    a1.sendall(b"GET /api/state HTTP/1.1\r\n\r\n")
    st, _, _, _ = read_response(a1)
    a2.settimeout(1.5)
    try:
        leaked = len(a2.recv(4096))
    except socket.timeout:
        leaked = 0
    print(f"info  second idle client received {leaked} bytes of the first client's reply "
          f"({'broadcast' if leaked else 'direct'} behaviour)")
    a1.close()
    a2.close()

    st, state = get(H, P, "/api/state")
    check(st == 200, "state readable")
    for b in state["baro"]:
        if not b["fresh"]:
            check(b["hpa"] is None, f"{b['name']}: stale pressure is null, not 0")

    if a.log_interval:
        st, log = get(H, P, "/api/log?limit=20")
        recs = [r for r in log["records"] if r["session"] == state["session"]]
        ts = [r["t_s"] for r in recs]
        gaps = [x - y for x, y in zip(ts, ts[1:])]
        print(f"info  {len(recs)} records this session, gaps {gaps}")
        check(len(gaps) >= 5, "enough records to judge the interval")
        check(all(abs(g - a.log_interval) <= 1 for g in gaps), f"log gaps within {a.log_interval}±1 s")
        both = all(len(r["ch"]) == 2 for r in recs)
        check(both, "each record carries both barometers")

    print("\n%d failure(s)" % len(FAILS))
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()
