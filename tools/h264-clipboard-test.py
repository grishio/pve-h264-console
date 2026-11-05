# SPDX-License-Identifier: MIT
# Usage: [PVE_USER=root@pam PVE_PASS=...] h264-clipboard-test.py <vmid>
# Clipboard round trip with a Windows guest (spice agent + qemu-guest-agent),
# through pveproxy like a browser: login -> h264proxy -> wss -> C/S_CLIPBOARD.
#  1. viewer -> guest: send text, read it back in the logged-in user's session
#  2. guest -> viewer: set text in the user's session, expect S_CLIPBOARD
#  3. a large (900 KiB) text, split into websocket frames as the viewer does
# Without PVE_PASS (as root on the host) a ticket is made with PVE's own API.
# The guest side runs PowerShell in the user's session through a scheduled
# task "pve-h264-cliptest" (left registered; harmless).
import base64, hashlib, json, os, socket, ssl, struct, subprocess, sys, time, urllib.parse, urllib.request

vmid = sys.argv[1]
host, node = "127.0.0.1", socket.gethostname()
ctx = ssl.create_default_context(); ctx.check_hostname = False; ctx.verify_mode = ssl.CERT_NONE
C_AUTH, C_CLIPBOARD, S_CLIPBOARD, S_CLIPBOARD_STATE, S_ERROR = 0x01, 0x0C, 0x8F, 0x90, 0x88
GUEST_DIR = r"C:\ProgramData\pve-h264"

# ------------------------------------------------------------ login
user = os.environ.get("PVE_USER", "root@pam")
if os.environ.get("PVE_PASS"):
    r = urllib.request.Request(f"https://{host}:8006/api2/json/access/ticket", method="POST",
        data=urllib.parse.urlencode({"username": user, "password": os.environ["PVE_PASS"]}).encode())
    t = json.load(urllib.request.urlopen(r, context=ctx))["data"]
    ticket, csrf = t["ticket"], t["CSRFPreventionToken"]
else:
    ticket, csrf = subprocess.check_output(["perl", "-MPVE::AccessControl", "-e",
        "print PVE::AccessControl::assemble_ticket($ARGV[0]), qq(\\n), "
        "PVE::AccessControl::assemble_csrf_prevention_token($ARGV[0])", user], text=True).split("\n")
hdr = {"Cookie": "PVEAuthCookie=" + urllib.parse.quote(ticket), "CSRFPreventionToken": csrf}
r = urllib.request.Request(f"https://{host}:8006/api2/json/nodes/{node}/qemu/{vmid}/h264proxy",
                           method="POST", data=b"", headers=hdr)
p = json.load(urllib.request.urlopen(r, context=ctx))["data"]

# ------------------------------------------------------------ websocket
s = ctx.wrap_socket(socket.create_connection((host, 8006)))
path = f"/api2/json/nodes/{node}/qemu/{vmid}/h264websocket?vncticket={urllib.parse.quote(p['ticket'], safe='')}"
s.sendall((f"GET {path} HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
    f"Sec-WebSocket-Key: {base64.b64encode(os.urandom(16)).decode()}\r\nSec-WebSocket-Version: 13\r\n"
    f"Sec-WebSocket-Protocol: binary\r\nCookie: {hdr['Cookie']}\r\n\r\n").encode())
resp = b""
while b"\r\n\r\n" not in resp: resp += s.recv(4096)
head, wsbuf = resp.split(b"\r\n\r\n", 1)
assert b" 101 " in head.split(b"\r\n")[0], head

def wsframe(payload):
    m = os.urandom(4); l = len(payload)
    h = bytes([0x82]) + (bytes([0x80 | l]) if l < 126 else
        bytes([0x80 | 126]) + struct.pack(">H", l) if l < 65536 else bytes([0x80 | 127]) + struct.pack(">Q", l))
    mask = (m * (l // 4 + 1))[:l]
    s.sendall(h + m + (int.from_bytes(payload, "big") ^ int.from_bytes(mask, "big")).to_bytes(l, "big"))
def msg(t, pl=b""):
    b = struct.pack("<IB", 1 + len(pl), t) + pl
    for o in range(0, len(b), 65536): # pveproxy refuses frames > 128 KiB
        wsframe(b[o:o + 65536])

stream = b""
def pump(secs, want=None):
    """read messages for up to secs; return the first (type, payload) of type want"""
    global wsbuf, stream
    end = time.time() + secs
    s.settimeout(0.2)
    while time.time() < end:
        while len(wsbuf) >= 2:
            l = wsbuf[1] & 0x7f; o = 2
            if l == 126: l = struct.unpack(">H", wsbuf[2:4])[0]; o = 4
            elif l == 127: l = struct.unpack(">Q", wsbuf[2:10])[0]; o = 10
            if len(wsbuf) < o + l: break
            stream += wsbuf[o:o + l]; wsbuf = wsbuf[o + l:]
        while len(stream) >= 5:
            (l,) = struct.unpack_from("<I", stream)
            if len(stream) < 4 + l: break
            t, pl = stream[4], stream[5:4 + l]; stream = stream[4 + l:]
            if t == 0x83: # ack frames so the stream keeps going
                msg(0x06, struct.pack("<II", struct.unpack_from("<I", pl)[0], 500))
            if t == S_ERROR: sys.exit("server error: " + pl.decode())
            if t == want: return t, pl
        try:
            d = s.recv(1 << 20)
            if not d: sys.exit("connection closed")
            wsbuf += d
        except (socket.timeout, ssl.SSLWantReadError):
            pass
    return None

# ------------------------------------------------------------ guest side
def qga_ps(script, timeout=30):
    """run PowerShell as SYSTEM through qemu-guest-agent, return stdout"""
    enc = base64.b64encode(script.encode("utf-16-le")).decode()
    for attempt in range(3): # qm sometimes times out polling guest-exec-status
        out = subprocess.run(["qm", "guest", "exec", vmid, "--timeout", str(timeout), "--",
            "powershell.exe", "-NoProfile", "-NonInteractive", "-EncodedCommand", enc],
            capture_output=True, text=True)
        try:
            j = json.loads(out.stdout)
            break
        except json.JSONDecodeError:
            print(f"  (qm guest exec failed, retrying: {out.stderr.strip()[:120]})")
            time.sleep(2)
    else:
        sys.exit("qm guest exec keeps failing")
    if j.get("exitcode"): print("guest stderr:", j.get("err-data", "")[:500])
    return j.get("out-data", "")

def in_user_session(ps, timeout=20):
    """run ps in the logged-in user's session (clipboard is per session)"""
    b64 = base64.b64encode(ps.encode("utf-16-le")).decode()
    return qga_ps(f"""
$ErrorActionPreference='Stop'
$d='{GUEST_DIR}'; New-Item -ItemType Directory -Force $d | Out-Null
Remove-Item -Force -ErrorAction SilentlyContinue "$d\\cliptest.done"
$u=(Get-CimInstance Win32_ComputerSystem).UserName
if (-not $u) {{ Write-Output 'NO_USER'; exit 3 }}
$a=New-ScheduledTaskAction -Execute 'conhost.exe' -Argument '--headless powershell.exe -NoProfile -STA -ExecutionPolicy Bypass -EncodedCommand {b64}'
$p=New-ScheduledTaskPrincipal -UserId $u -LogonType Interactive
Register-ScheduledTask -TaskName pve-h264-cliptest -Action $a -Principal $p -Force | Out-Null
Start-ScheduledTask -TaskName pve-h264-cliptest
$t=[DateTime]::Now.AddSeconds({timeout})
while (-not (Test-Path "$d\\cliptest.done") -and [DateTime]::Now -lt $t) {{ Start-Sleep -Milliseconds 200 }}
if (Test-Path "$d\\cliptest.out") {{ [IO.File]::ReadAllText("$d\\cliptest.out") }}
""", timeout + 15)

def guest_get_clipboard():
    """(has CRLF, length, sha256 of the UTF-8 text with LF line ends, first 80 chars)"""
    out = in_user_session(f"""
$v = [string](Get-Clipboard -Raw)
$crlf = $v.Contains("`r`n")
$b = [Text.Encoding]::UTF8.GetBytes($v.Replace("`r`n", "`n"))
$h = -join ([Security.Cryptography.SHA256]::Create().ComputeHash($b) | ForEach-Object {{ $_.ToString('x2') }})
$head = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($v.Substring(0, [Math]::Min(80, $v.Length))))
[IO.File]::WriteAllText('{GUEST_DIR}\\cliptest.out', "CLIP $crlf $($b.Length) $h $head")
New-Item -Force '{GUEST_DIR}\\cliptest.done' | Out-Null
""")
    if "CLIP " not in out: sys.exit(f"could not read the guest clipboard: {out!r}")
    _, crlf, n, h, head = out[out.index("CLIP "):].split()
    return crlf == "True", int(n), h, base64.b64decode(head).decode()

def lf_sha(text):
    b = text.encode()
    return len(b), hashlib.sha256(b).hexdigest()

def guest_set_clipboard(text):
    b = base64.b64encode(text.encode()).decode()
    in_user_session(f"""
Set-Clipboard -Value ([Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('{b}')))
[IO.File]::WriteAllText('{GUEST_DIR}\\cliptest.out', 'ok')
New-Item -Force '{GUEST_DIR}\\cliptest.done' | Out-Null
""")

# ------------------------------------------------------------ test
ok = True
def check(name, cond, detail=""):
    global ok
    ok &= bool(cond)
    print(("PASS" if cond else "FAIL"), name, detail)

msg(C_AUTH, p["token"].encode())
state, end = {}, time.time() + 20 # the agent may answer a few seconds after attach
while not state.get("available") and time.time() < end:
    st = pump(end - time.time(), S_CLIPBOARD_STATE)
    state = json.loads(st[1]) if st else state
check("clipboard state", state.get("available"), json.dumps(state))
if not state.get("available"): sys.exit(1)

# 1. viewer -> guest (LF in, CRLF in the Windows guest, back to LF)
text = f"from the viewer {time.time():.0f}\nline 2: ünïcödé ✓ 🎉\n\ttabbed"
msg(C_CLIPBOARD, text.encode())
time.sleep(0.3)
crlf, n, h, head = guest_get_clipboard()
check("viewer -> guest", (n, h) == lf_sha(text) and crlf, repr(head))
# the guest reading its clipboard must not bounce it back to the viewer
check("no echo back to the viewer", pump(1.0, S_CLIPBOARD) is None)

# 2. guest -> viewer
gtext = f"from the guest {time.time():.0f}\r\nzweite Zeile äöü 中文"
guest_set_clipboard(gtext)
m = pump(10, S_CLIPBOARD)
got = m[1][1:].decode() if m else None
check("guest -> viewer", got == gtext.replace("\r\n", "\n"), repr(got))
check("  flags = 0 (live copy)", m and m[1][0] == 0)

# 3. large text, viewer -> guest
big = "".join(f"{i:06d} the quick brown fox jumps over the lazy dog\n" for i in range(18000))[:900 * 1024]
t0 = time.time()
msg(C_CLIPBOARD, big.encode())
time.sleep(0.5)
crlf, n, h, head = guest_get_clipboard()
check(f"large text ({len(big) >> 10} KiB) -> guest", (n, h) == lf_sha(big),
      f"guest has {n} bytes, read back in {time.time() - t0:.1f}s")

print("OK" if ok else "FAILED")
sys.exit(0 if ok else 1)
