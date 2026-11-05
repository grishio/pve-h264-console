# SPDX-License-Identifier: MIT
# Usage: h264-latency.py <vmid> <seconds>   (run on the PVE host as root)
# Connects straight to /run/pve-h264/<vmid>.sock, ACKs instantly and prints
# capture->client latency and frame interval percentiles.
import socket, struct, sys, os, time, secrets
vmid = int(sys.argv[1]); secs = float(sys.argv[2])
tok = secrets.token_hex(32); d = f"/run/pve-h264/tokens/{vmid}"; os.makedirs(d, mode=0o700, exist_ok=True)
open(f"{d}/{tok}", "w").close()
s = socket.socket(socket.AF_UNIX); s.connect(f"/run/pve-h264/{vmid}.sock")
s.sendall(struct.pack("<IB", 1 + len(tok), 1) + tok.encode())
buf = bytearray(); end = time.time() + secs; lat = []; gaps = []; last = None
while time.time() < end:
    data = s.recv(4 << 20); now = time.monotonic_ns() // 1000
    buf += data; pos = 0; acks = []
    while len(buf) - pos >= 5:
        (l,) = struct.unpack_from("<I", buf, pos)
        if len(buf) - pos < 4 + l: break
        if buf[pos + 4] == 0x83:
            fid, fl, eus, cap = struct.unpack_from("<IBIQ", buf, pos + 5)
            lat.append((now - cap) / 1000); 
            if last: gaps.append((cap - last) / 1000)
            last = cap
            acks.append(struct.pack("<IBII", 9, 6, fid, 0))
        pos += 4 + l
    del buf[:pos]
    if acks: s.sendall(b"".join(acks))
lat.sort(); gaps.sort()
q = lambda a, p: a[int(len(a) * p)]
print(f"frames={len(lat)} capture->client ms: p50 {q(lat,.5):.1f} p90 {q(lat,.9):.1f} max {lat[-1]:.1f} | capture interval ms: p10 {q(gaps,.1):.1f} p50 {q(gaps,.5):.1f} p90 {q(gaps,.9):.1f}")
