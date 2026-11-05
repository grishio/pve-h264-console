# SPDX-License-Identifier: MIT
# Usage: start chromium --headless=new --no-sandbox --ignore-certificate-errors
#        --remote-debugging-port=9222 about:blank, then
#        PVE_USER=.. PVE_PASS=.. (or PVE_TICKET=..) OUT_DIR=/tmp h264-browser-test.py <vmid> view|esc|perf|ui|mouse|settings
# Minimal CDP driver: open the H.264 viewer in headless Chromium as the test user.
import json, os, socket, struct, sys, time, base64, ssl, urllib.request, urllib.parse
S = os.environ.get("OUT_DIR", "/tmp"); vmid = sys.argv[1]; action = sys.argv[2] if len(sys.argv) > 2 else "view"
host = socket.gethostname()
ctx = ssl.create_default_context(); ctx.check_hostname = False; ctx.verify_mode = ssl.CERT_NONE
ticket = os.environ.get("PVE_TICKET")  # or log in with PVE_USER / PVE_PASS
if not ticket:
    r = urllib.request.Request("https://127.0.0.1:8006/api2/json/access/ticket", method="POST",
        data=urllib.parse.urlencode({"username": os.environ.get("PVE_USER", "root@pam"), "password": os.environ["PVE_PASS"]}).encode())
    ticket = json.load(urllib.request.urlopen(r, context=ctx))["data"]["ticket"]
tabs = json.load(urllib.request.urlopen("http://127.0.0.1:9222/json"))
page = next(t for t in tabs if t["type"] == "page")
u = urllib.parse.urlparse(page["webSocketDebuggerUrl"])
s = socket.create_connection((u.hostname, u.port))
s.sendall((f"GET {u.path} HTTP/1.1\r\nHost: {u.netloc}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
           f"Sec-WebSocket-Key: {base64.b64encode(os.urandom(16)).decode()}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
buf = b""
while b"\r\n\r\n" not in buf: buf += s.recv(4096)
buf = buf.split(b"\r\n\r\n", 1)[1]
mid = 0; events = []
def wsend(d):
    m = os.urandom(4); l = len(d)
    h = b"\x81" + (bytes([0x80 | l]) if l < 126 else (bytes([0xfe]) + struct.pack(">H", l) if l < 65536 else bytes([0xff]) + struct.pack(">Q", l)))
    s.sendall(h + m + bytes(b ^ m[i % 4] for i, b in enumerate(d)))
def wrecv():
    global buf
    while True:
        if len(buf) >= 2:
            l = buf[1] & 0x7f; o = 2
            if l == 126: l = struct.unpack(">H", buf[2:4])[0]; o = 4
            elif l == 127: l = struct.unpack(">Q", buf[2:10])[0]; o = 10
            if len(buf) >= o + l:
                d = buf[o:o+l]; buf = buf[o+l:]; return json.loads(d)
        buf += s.recv(1 << 20)
def call(method, **params):
    global mid
    mid += 1; wsend(json.dumps({"id": mid, "method": method, "params": params}).encode())
    while True:
        m = wrecv()
        if m.get("id") == mid: return m.get("result", m.get("error"))
        events.append(m)
def pump(secs):
    s.settimeout(0.1); end = time.time() + secs
    while time.time() < end:
        try: events.append(wrecv())
        except (socket.timeout, BlockingIOError): pass
    s.settimeout(None)
def js(expr): return call("Runtime.evaluate", expression=expr, returnByValue=True, awaitPromise=True).get("result", {}).get("value")
def key(code, keyname, vk):
    for t in ("rawKeyDown", "keyUp"):
        call("Input.dispatchKeyEvent", type=t, code=code, key=keyname, windowsVirtualKeyCode=vk)
def shot(name):
    d = call("Page.captureScreenshot", format="png")["data"]
    open(f"{S}/{name}", "wb").write(base64.b64decode(d)); print("screenshot", name)
call("Runtime.enable"); call("Network.enable"); call("Page.enable")
call("Network.setCookie", name="PVEAuthCookie", value=ticket, domain="127.0.0.1", path="/", secure=True)
q = urllib.parse.urlencode({"console": "kvm", "h264": 1, "vmid": vmid, "node": host})
if action == "ui":
    call("Page.navigate", url=f"https://127.0.0.1:8006/#v1:0:=qemu%2F{vmid}:4:::::::")
    pump(6)
    print("openH264Viewer:", js("typeof PVE.Utils.openH264Viewer"))
    print("console menu:", js("Ext.ComponentQuery.query('pveConsoleButton').map(b => b.consoleType + ' -> ' + b.menu.items.items.map(i => i.text + (i.hidden ? ' (hidden)' : '')).join(', ')).join(' || ')"))
    for d in ("0", "1"):
        print(f"console tab iframe (default={d}):", js(f"""(() => {{ localStorage.setItem('pve-h264-default','{d}');
          let c = Ext.create('PVE.noVncConsole', {{nodename:'{host}', vmid:{vmid}, consoleType:'kvm', renderTo: document.body, width:300, height:200}});
          c.fireEvent('activate'); let src = c.down('[itemid=vncconsole]').getFrame().src; c.destroy(); return src; }})()"""))
    print("default viewer (kvm):", js("PVE.Utils.defaultViewer(true, 'kvm')"))
    # open the real dropdown (renders the menu) and read what is visible
    js("(() => { const b = Ext.ComponentQuery.query('pveConsoleButton').find(b => b.consoleType === 'kvm'); b.showMenu(); })()")
    pump(1)
    print("rendered dropdown:", js("Ext.ComponentQuery.query('pveConsoleButton').filter(b => b.consoleType === 'kvm').map(b => b.menu.items.items.map(i => i.text + (i.isVisible() ? '' : ' [HIDDEN]')).join(', ')).join(' || ')"))
    js("localStorage.setItem('pve-h264-default','0')")
    shot("ui.png"); sys.exit(0)
if os.environ.get("DPR"): # emulate a HiDPI screen
    call("Emulation.setDeviceMetricsOverride", width=1600, height=1000, deviceScaleFactor=float(os.environ["DPR"]), mobile=False)
call("Page.navigate", url=f"https://127.0.0.1:8006/?{q}")
pump(6)
print("status:", repr(js("document.getElementById('status').textContent + ' / ' + document.getElementById('status').style.display")))
print("encoder:", js("document.getElementById('encoder').textContent"))
print("canvas:", js("(()=>{const c=document.getElementById('screen');return c.width+'x'+c.height+' css '+c.style.width+'x'+c.style.height})()"))
print("dbg:", js("JSON.stringify({s:__h264.decoderState,q:__h264.queue,wk:__h264.waitKey,cfg:__h264.config,hw:__h264.hwAccel,l:__h264.local})"))
print("probe:", js("VideoDecoder.isConfigSupported({codec:'avc1.640020',codedWidth:640,codedHeight:480}).then(r=>JSON.stringify(r.supported))"))
print("stats:\n" + (js("document.getElementById('stats').textContent") or ""))
if action == "perf":
    for i in range(5):
        pump(2); print(f"t+{2*(i+1)}s:", (js("document.getElementById('stats').textContent") or "").replace("\n"," | "))
if action == "mouse":
    js("""(() => { const o = WebSocket.prototype.send; window.__sent = {};
        WebSocket.prototype.send = function (b) { const u = new Uint8Array(b.buffer || b);
            window.__sent[u[4]] = (window.__sent[u[4]] || 0) + 1; return o.call(this, b); }; })()""")
    call("Input.dispatchMouseEvent", type="mouseMoved", x=600, y=400)
    call("Input.dispatchMouseEvent", type="mousePressed", x=600, y=400, button="left", clickCount=1)
    call("Input.dispatchMouseEvent", type="mouseReleased", x=600, y=400, button="left", clickCount=1)
    for i in range(20):
        call("Input.dispatchMouseEvent", type="mouseMoved", x=600 + i * 7, y=400 + i * 3); pump(0.02)
    pump(0.3)
    print("pointer locked:", js("document.pointerLockElement !== null"))
    if os.environ.get("CAPTURE"):
        js("document.getElementById('capture').click()")
        call("Input.dispatchMouseEvent", type="mousePressed", x=600, y=400, button="left", clickCount=1)
        call("Input.dispatchMouseEvent", type="mouseReleased", x=600, y=400, button="left", clickCount=1)
        pump(0.5)
        print("with Capture mouse on -> pointer locked:", js("document.pointerLockElement !== null"))
    print("mouse label:", js("document.getElementById('mousemode').textContent"))
    print("msgs sent (type:count, 3=abs 4=button 5=rel):", js("JSON.stringify(window.__sent)"))
if action == "settings":
    js("""(() => { const o = WebSocket.prototype.send; window.__cfg = []; window.__rs = [];
        WebSocket.prototype.send = function (b) { const u = new Uint8Array(b.buffer || b);
            if (u[4] === 7) window.__cfg.push(new TextDecoder().decode(u.subarray(5)));
            if (u[4] === 10) { const d = new DataView(u.buffer, u.byteOffset); window.__rs.push(d.getUint32(5, true) + 'x' + d.getUint32(9, true)); }
            return o.call(this, b); }; })()""")
    js("document.getElementById('settingsbtn').click()")
    pump(0.5)
    print("panel rows:", js("document.querySelectorAll('#panel .p-row').length"), "encoder:", js("document.getElementById('p-enc').textContent"))
    shot("settings_panel.png")
    js("document.querySelector('#panel [data-profile=video]').click()")
    pump(3)
    print("sent config:", js("JSON.stringify(window.__cfg)"))
    print("server settings:", js("JSON.stringify(__h264.settings.server)"))
    print("active profile:", js("[...document.querySelectorAll('#panel [data-profile].active')].map(b => b.dataset.profile).join(',')"))
    # change one slider like a user would
    js("(() => { const r = document.getElementById('set-quality'); r.value = 22; r.dispatchEvent(new Event('input')); r.dispatchEvent(new Event('change')); })()")
    pump(2)
    print("after quality=22:", js("__h264.settings.server.quality"), js("document.querySelector('#set-quality + output').textContent"),
          "profile still active:", js("document.querySelectorAll('#panel [data-profile].active').length"))
    for v in ("native", "0.5"):
        js(f"(() => {{ const d = document.getElementById('set-dpi'); d.value = '{v}'; d.dispatchEvent(new Event('change')); }})()")
        pump(0.3)
        print(f"dpi {v}: resize sent", js("window.__rs[window.__rs.length - 1]"), "| wrap", js("(() => { const w = document.getElementById('wrap'); return w.clientWidth + 'x' + w.clientHeight + ' dpr ' + devicePixelRatio; })()"), "| info", js("document.getElementById('dpi-info').textContent"))
    shot("settings_after.png")
    js("document.querySelector('#panel .p-foot button').click()")
    pump(2)
    print("after reset:", js("JSON.stringify({o: __h264.settings.overrides, s: __h264.settings.server.preset + '/' + __h264.settings.server.quality})"))
    js("localStorage.removeItem('pve-h264-dpi'); localStorage.removeItem('pve-h264-autoresize')")
    print("stats:\n" + (js("document.getElementById('stats').textContent") or ""))
if action == "esc":
    call("Input.dispatchMouseEvent", type="mousePressed", x=640, y=400, button="left", clickCount=1)
    call("Input.dispatchMouseEvent", type="mouseReleased", x=640, y=400, button="left", clickCount=1)
    for i in range(30):
        key("Escape", "Escape", 27); pump(0.15)
    pump(1.0)
shot(f"viewer_{action}.png")
for e in events:
    if e.get("method") == "Runtime.consoleAPICalled":
        print("console.%s:" % e["params"]["type"], " ".join(str(a.get("value", a.get("description", ""))) for a in e["params"]["args"])[:300])
    elif e.get("method") == "Runtime.exceptionThrown":
        print("EXCEPTION:", e["params"]["exceptionDetails"].get("exception", {}).get("description", e["params"]["exceptionDetails"].get("text"))[:400])
