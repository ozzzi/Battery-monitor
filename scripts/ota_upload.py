#!/usr/bin/env python3
"""OTA firmware upload via the ArduinoOTA protocol.

    PYLONMON_OTA_PASS=... scripts/ota_upload.py [.pio/build/probe_ota/firmware.bin] [host]
"""
import hashlib
import os
import socket
import sys
import time

FW = sys.argv[1] if len(sys.argv) > 1 else ".pio/build/probe_ota/firmware.bin"
HOST = sys.argv[2] if len(sys.argv) > 2 else os.environ.get("PYLONMON_HOST", "battery.local")
OTA_PORT, HOST_PORT = 3232, 45678
U_FLASH, U_AUTH = 0, 200


def main() -> int:
    pw = os.environ.get("PYLONMON_OTA_PASS")
    if not pw:
        print("set PYLONMON_OTA_PASS", file=sys.stderr)
        return 2
    ip = socket.gethostbyname(HOST)
    data = open(FW, "rb").read()
    size, md5 = len(data), hashlib.md5(data).hexdigest()
    print(f"{FW}: {size} bytes -> {HOST} ({ip})")

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", HOST_PORT))
    srv.listen(1)

    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.settimeout(10)
    udp.sendto(f"{U_FLASH} {HOST_PORT} {size} {md5}\n".encode(), (ip, OTA_PORT))
    reply = udp.recv(128).decode()
    if not reply.startswith("AUTH "):
        print(f"unexpected invitation reply: {reply!r}", file=sys.stderr)
        return 1
    nonce = reply.split()[1]
    cnonce = hashlib.sha256(f"{FW}{size}{md5}{ip}".encode()).hexdigest()
    pw_hash = hashlib.sha256(pw.encode()).hexdigest()
    key = hashlib.pbkdf2_hmac("sha256", pw_hash.encode(), f"{nonce}:{cnonce}".encode(), 10000).hex()
    response = hashlib.sha256(f"{key}:{nonce}:{cnonce}".encode()).hexdigest()
    udp.sendto(f"{U_AUTH} {cnonce} {response}\n".encode(), (ip, OTA_PORT))
    print("auth sent, waiting for device TCP connect...")

    srv.settimeout(20)
    try:
        conn, addr = srv.accept()
    except socket.timeout:
        print("device did not connect (wrong password or firewall on TCP 45678)", file=sys.stderr)
        return 1
    print(f"device connected from {addr[0]}")
    conn.settimeout(15)
    t0 = time.time()
    last = ""
    for off in range(0, size, 1024):
        conn.sendall(data[off:off + 1024])
        last = conn.recv(32).decode(errors="replace").strip()
        if off % (100 * 1024) < 1024:
            print(f"  {off * 100 // size:3d}%  {off // 1024} KiB")
    deadline = time.time() + 15
    while "OK" not in last and time.time() < deadline:
        try:
            chunk = conn.recv(32).decode(errors="replace")
        except socket.timeout:
            break
        if not chunk:
            break
        last += chunk
    conn.close()
    srv.close()
    ok = "OK" in last
    print(f"done in {time.time() - t0:.1f}s: {'OK' if ok else 'no final OK: ' + repr(last[-40:])}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
