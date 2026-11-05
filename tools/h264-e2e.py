# SPDX-License-Identifier: MIT
# Usage: PVE_USER=root@pam PVE_PASS=... h264-e2e.py <vmid> <seconds>
# Full path test: login -> h264proxy -> wss h264websocket (pveproxy) -> frames.
# End-to-end: login -> h264proxy -> wss h264websocket via pveproxy -> frames
import ssl, socket, json, base64, os, struct, sys, time, urllib.request, urllib.parse
host, vmid, secs = "127.0.0.1", sys.argv[1], float(sys.argv[2])
pw = os.environ["PVE_PASS"]
ctx = ssl.create_default_context(); ctx.check_hostname = False; ctx.verify_mode = ssl.CERT_NONE
def req(method, path, data=None, hdr={}):
    r = urllib.request.Request(f"https://{host}:8006/api2/json{path}", method=method,
        data=urllib.parse.urlencode(data).encode() if data is not None else None, headers=hdr)
    try:
        return json.load(urllib.request.urlopen(r, context=ctx))["data"]
    except urllib.error.HTTPError as e:
        print("HTTP", e.code, e.reason, e.read()[:300]); sys.exit(1)
t = req("POST", "/access/ticket", {"username": os.environ.get("PVE_USER", "root@pam"), "password": pw})
hdr = {"Cookie": "PVEAuthCookie=" + urllib.parse.quote(t["ticket"]), "CSRFPreventionToken": t["CSRFPreventionToken"]}
t0 = time.time(); p = req("POST", f"/nodes/{socket.gethostname()}/qemu/{vmid}/h264proxy", {}, hdr)
print(f"h264proxy ok in {(time.time()-t0)*1000:.0f} ms, token={p['token'][:8]}…")
s = ctx.wrap_socket(socket.create_connection((host, 8006)))
s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
key = base64.b64encode(os.urandom(16)).decode()
path = f"/api2/json/nodes/{socket.gethostname()}/qemu/{vmid}/h264websocket?vncticket={urllib.parse.quote(p['ticket'], safe='')}"
s.sendall((f"GET {path} HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
    f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Protocol: binary\r\n"
    f"Cookie: {hdr['Cookie']}\r\n\r\n").encode())
resp = b""
while b"\r\n\r\n" not in resp: resp += s.recv(4096)
head, rest = resp.split(b"\r\n\r\n", 1); print(head.split(b"\r\n")[0].decode())
def wsend(payload):
    m = os.urandom(4); l = len(payload)
    h = bytes([0x82]) + (bytes([0x80 | l]) if l < 126 else bytes([0x80 | 126]) + struct.pack(">H", l))
    s.sendall(h + m + bytes(b ^ m[i % 4] for i, b in enumerate(payload)))
def msg(t, p=b""): wsend(struct.pack("<IB", 1 + len(p), t) + p)
msg(1, p["token"].encode())
wsbuf, stream, frames, keys, tot, pings = rest, b"", 0, 0, 0, {}
s.settimeout(0.3); end = time.time() + secs; lastping = 0; rtts = []
while time.time() < end:
    if time.time() - lastping > 0.5:
        lastping = time.time(); msg(9, struct.pack("<d", time.perf_counter()))
    try:
        d = s.recv(1 << 20)
        if not d: print("EOF"); break
        wsbuf += d
    except (socket.timeout, ssl.SSLWantReadError): pass
    while len(wsbuf) >= 2:
        l = wsbuf[1] & 0x7f; o = 2
        if l == 126: l = struct.unpack(">H", wsbuf[2:4])[0]; o = 4
        elif l == 127: l = struct.unpack(">Q", wsbuf[2:10])[0]; o = 10
        if len(wsbuf) < o + l: break
        stream += wsbuf[o:o+l]; wsbuf = wsbuf[o+l:]
    while len(stream) >= 5:
        (l,) = struct.unpack_from("<I", stream)
        if len(stream) < 4 + l: break
        t, pl = stream[4], stream[5:4+l]; stream = stream[4+l:]
        if t == 0x83:
            fid, fl = struct.unpack_from("<IB", pl); frames += 1; keys += fl & 1; tot += len(pl) - 17
            msg(6, struct.pack("<II", fid, 500))
        elif t == 0x86: rtts.append((time.perf_counter() - struct.unpack("<d", pl)[0]) * 1000)
        elif t in (0x81, 0x82, 0x88): print(hex(t), pl.decode()[:160])
print(f"frames={frames} keyframes={keys} bytes={tot}  ping rtt via pveproxy: min {min(rtts):.2f} ms avg {sum(rtts)/len(rtts):.2f} ms (n={len(rtts)})")
