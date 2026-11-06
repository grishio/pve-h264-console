/* SPDX-License-Identifier: MIT */
/*
 * pve-h264d - resize Windows guests through the QEMU guest agent.
 *
 * The Windows virtio-gpu driver (viogpudo) takes the size from our
 * Console.SetUIInfo hint into its "custom" mode slot and signals a resolution
 * event, but nothing in Windows switches to it: viogpudo leaves that to its
 * own user-session helper (viogpuap.exe), and the spice agent only knows the
 * QXL way of adding modes. We do what viogpuap does (QueryDisplayConfig +
 * SetDisplayConfig on the source mode, in the user's session) without
 * installing anything:
 *
 *  - A small PowerShell *listener* runs in the logged-in user's session. It
 *    defines the two user32 calls in memory (Reflection.Emit, no compiler
 *    run), watches C:\ProgramData\pve-h264\resize.req and answers in a new
 *    resize.ack.<seq> per request; resize.alive exists while it runs. It
 *    exits after LISTENER_IDLE_MIN minutes without requests, or with the
 *    user's session. One per session (named mutex).
 *  - Per resize the daemon only writes resize.req with the agent's file
 *    commands (guest-file-open/write/close: no process is started in the
 *    guest) and polls resize.ack. Typically ~0.1-0.3 s.
 *  - No ack -> no listener: start one. guest-exec (SYSTEM, session 0, where
 *    display settings can't be changed) runs PowerShell that (re)registers a
 *    hidden scheduled task "pve-h264-resize" for the logged-in user
 *    (TASK_LOGON_INTERACTIVE_TOKEN: their existing session, no password;
 *    `conhost --headless`: no window) and runs it. The task definition stays
 *    registered until the next start replaces it (deleting it could take the
 *    running listener down with it).
 *
 * Runs in its own thread: agent round trips block. Only the latest requested
 * size is kept. Each operation uses a fresh, synced agent connection, because
 * the socket serves one client at a time and Proxmox uses it too.
 */
#include "pve-h264d.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <glib.h>

#define QGA_TIMEOUT_MS 3000
#define RESP_MAX (256 * 1024)
#define ACK_FAST_MS 2500      /* running listener: answer expected within */
#define ACK_START_MS 8000     /* freshly started listener */
#define ACK_POLL_MS 40
#define ALIVE_CHECK_MS 300   /* no ack yet: is a listener running at all? */
#define WRITE_TRIES 6
#define LISTENER_IDLE_MIN 30

#define GUEST_DIR "C:\\\\ProgramData\\\\pve-h264\\\\" /* JSON-escaped */
#define REQ_FILE GUEST_DIR "resize.req"
#define ACK_FILE GUEST_DIR "resize.ack.%llu" /* one per request: never rewritten */
#define ALIVE_FILE GUEST_DIR "resize.alive"   /* exists while a listener runs */

/* Exit code of the start script */
#define RC_NO_USER 10

/* ------------------------------------------------------------- scripts */

/* User session, long running. %d = idle minutes. Struct offsets:
 * DISPLAYCONFIG_PATH_INFO 72 bytes (source.modeInfoIdx at 12),
 * DISPLAYCONFIG_MODE_INFO 64 bytes (infoType at 0, 1 = source; source mode
 * width at 16, height at 20). SetDisplayConfig flags 0x6A0 = SDC_APPLY |
 * SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_SAVE_TO_DATABASE | SDC_ALLOW_CHANGES
 * (as viogpuap). The driver may not have taken the new size from the hint
 * yet when a request arrives: retry for ~1.5 s. Request line:
 * "<seq> <w> <h> end", ack: "<seq> <result> <pid> end". */
static const char *LISTENER_SCRIPT =
    "$ErrorActionPreference='Stop'; $ProgressPreference='SilentlyContinue'\n"
    "$created = $false\n"
    "$mtx = [Threading.Mutex]::new($true, 'Local\\pve-h264-resize', [ref]$created)\n"
    "if (-not $created) { exit 0 }\n"
    "$ab = [AppDomain]::CurrentDomain.DefineDynamicAssembly((New-Object Reflection.AssemblyName 'PveH264'), [Reflection.Emit.AssemblyBuilderAccess]::Run)\n"
    "$tb = $ab.DefineDynamicModule('PveH264').DefineType('PveH264Native', 'Public, Class')\n"
    "function Def($name, [Type[]]$params) {\n"
    "  $m = $tb.DefinePInvokeMethod($name, 'user32.dll', 'Public, Static, PinvokeImpl', 'Standard', [int], $params, 'Winapi', 'Auto')\n"
    "  $m.SetImplementationFlags('PreserveSig')\n"
    "}\n"
    "$u32r = [uint32].MakeByRefType()\n"
    "Def 'GetDisplayConfigBufferSizes' @([uint32], $u32r, $u32r)\n"
    "Def 'QueryDisplayConfig' @([uint32], $u32r, [byte[]], $u32r, [byte[]], [IntPtr])\n"
    "Def 'SetDisplayConfig' @([uint32], [byte[]], [uint32], [byte[]], [uint32])\n"
    "$N = $tb.CreateType()\n"
    "function Set-Res([uint32]$w, [uint32]$h) {\n"
    "  $np = [uint32]0; $nm = [uint32]0\n"
    "  $r = $N::GetDisplayConfigBufferSizes(2, [ref]$np, [ref]$nm)\n"
    "  if ($r) { return 0x10000 -bor $r }\n"
    "  $p = New-Object byte[] ([int]($np * 72)); $m = New-Object byte[] ([int]($nm * 64))\n"
    "  $r = $N::QueryDisplayConfig(2, [ref]$np, $p, [ref]$nm, $m, [IntPtr]::Zero)\n"
    "  if ($r) { return 0x20000 -bor $r }\n"
    "  if ($np -eq 0) { return 0x30000 }\n"
    "  $ix = [BitConverter]::ToUInt32($p, 12)\n"
    "  if ($ix -ge $nm -or [BitConverter]::ToUInt32($m, [int]$ix * 64) -ne 1) { return 0x40000 }\n"
    "  $o = [int]$ix * 64\n"
    "  if ([BitConverter]::ToUInt32($m, $o + 16) -eq $w -and [BitConverter]::ToUInt32($m, $o + 20) -eq $h) { return 0 }\n"
    "  [BitConverter]::GetBytes($w).CopyTo($m, $o + 16)\n"
    "  [BitConverter]::GetBytes($h).CopyTo($m, $o + 20)\n"
    "  $r = $N::SetDisplayConfig($np, $p, $nm, $m, 0x6A0)\n"
    "  if ($r) { return 0x50000 -bor $r }\n"
    "  return 0\n"
    "}\n"
    "$dir = Join-Path $env:ProgramData 'pve-h264'\n"
    "$req = Join-Path $dir 'resize.req'; $alive = Join-Path $dir 'resize.alive'\n"
    "function Read-Req { try { [IO.File]::ReadAllText($req) } catch { $null } }\n"
    "function Write-Retry($path, $text) {\n"
    "  for ($i = 0; $i -lt 50; $i++) { try { [IO.File]::WriteAllText($path, $text); return } catch { Start-Sleep -Milliseconds 20 } }\n"
    "}\n"
    "function Clean-Acks($keep) {\n"
    "  Get-ChildItem -Path $dir -Filter 'resize.ack.*' -ErrorAction SilentlyContinue | Where-Object { $_.Name -ne $keep } | Remove-Item -Force -ErrorAction SilentlyContinue\n"
    "}\n"
    "Write-Retry $alive \"$PID\"\n"
    "$fw = New-Object IO.FileSystemWatcher($dir, 'resize.req')\n"
    "$last = ''\n"
    "$idle = [Diagnostics.Stopwatch]::StartNew()\n"
    "while ($idle.Elapsed.TotalMinutes -lt %d) {\n"
    "  $t = Read-Req\n"
    "  if ($null -eq $t -and (Test-Path $req)) { Start-Sleep -Milliseconds 20; continue }\n"
    "  if ($t -match '^(\\d+) (\\d+) (\\d+) end' -and $Matches[1] -ne $last) {\n"
    "    $seq = $Matches[1]; $w = [uint32]$Matches[2]; $h = [uint32]$Matches[3]\n"
    "    $rc = 0x60000; $newer = $false\n"
    "    for ($i = 0; $i -lt 16; $i++) {\n"
    "      try { $rc = Set-Res $w $h } catch { $rc = 0x60000 }\n"
    "      if ($rc -eq 0) { break }\n"
    "      Start-Sleep -Milliseconds 100\n"
    "      $t2 = Read-Req\n"
    "      if ($t2 -match '^(\\d+) ' -and $Matches[1] -ne $seq) { $newer = $true; break }\n"
    "    }\n"
    "    if ($newer) { continue }\n"
    "    $name = \"resize.ack.$seq\"\n"
    "    Write-Retry (Join-Path $dir $name) \"$seq $rc $PID end\"\n"
    "    Clean-Acks $name\n"
    "    $last = $seq; $idle.Restart()\n"
    "  }\n"
    "  $null = $fw.WaitForChanged([IO.WatcherChangeTypes]::All, 250)\n"
    "}\n"
    "Clean-Acks ''\n"
    "Remove-Item -Force -ErrorAction SilentlyContinue $alive\n";

/* SYSTEM via guest-exec. %s = encoded LISTENER_SCRIPT. Exits right after
 * starting the task; the daemon waits for the listener's ack instead. */
static const char *START_SCRIPT =
    "$ErrorActionPreference='Stop'; $ProgressPreference='SilentlyContinue'\n"
    "$u = (Get-CimInstance Win32_ComputerSystem).UserName\n"
    "if (-not $u) { exit 10 }\n"
    "$dir = Join-Path $env:ProgramData 'pve-h264'\n"
    "$null = New-Item -ItemType Directory -Force $dir\n"
    "Get-ChildItem -Path $dir -Filter 'resize.a*' -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue\n"
    "$n = 'pve-h264-resize'\n"
    "$s = New-Object -ComObject Schedule.Service\n"
    "$s.Connect()\n"
    "$f = $s.GetFolder('\\')\n"
    "try { $f.DeleteTask($n, 0) } catch {}\n"
    "$d = $s.NewTask(0)\n"
    "$d.RegistrationInfo.Description = 'pve-h264-console: resizes the display to the H.264 console window (started on demand by the host; exits when idle)'\n"
    "$d.Principal.UserId = $u\n"
    "$d.Principal.LogonType = 3\n"
    "$d.Principal.RunLevel = 0\n"
    "$d.Settings.Hidden = $true\n"
    "$d.Settings.DisallowStartIfOnBatteries = $false\n"
    "$d.Settings.StopIfGoingOnBatteries = $false\n"
    "$d.Settings.ExecutionTimeLimit = 'PT0S'\n"
    "$d.Settings.MultipleInstances = 0\n"
    "$a = $d.Actions.Create(0)\n"
    "$a.Path = 'conhost.exe'\n"
    "$a.Arguments = '--headless powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand %s'\n"
    "$t = $f.RegisterTaskDefinition($n, $d, 6, $u, $null, 3)\n"
    "$null = $t.Run($null)\n"
    "exit 0\n";

/* ---------------------------------------------------------------- state */

static int g_vmid;
static bool g_enabled;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
static uint32_t g_pend_w, g_pend_h;
static bool g_pend;
static enum { OS_UNKNOWN, OS_WINDOWS, OS_OTHER } g_os;
static atomic_bool g_is_windows;
static char g_os_id[33];          /* written once, before g_os_known */
static atomic_bool g_os_known;
static uint64_t g_seq;
static char *g_start_cmd; /* guest-exec JSON for START_SCRIPT, built once */

/* ---------------------------------------------------------------- agent */

static bool send_all(int fd, const char *p, size_t n)
{
    while (n) {
        ssize_t r = send(fd, p, n, MSG_NOSIGNAL);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return false;
        p += r;
        n -= r;
    }
    return true;
}

/* Read one '\n'-terminated line (without it). With skip_to_ff, first discard
 * everything up to and including a 0xFF byte (guest-sync-delimited). */
static char *read_line(int fd, bool skip_to_ff)
{
    GString *s = g_string_new(NULL);
    bool synced = !skip_to_ff;
    char c;
    for (;;) {
        ssize_t r = recv(fd, &c, 1, 0);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0 || s->len > RESP_MAX) {
            g_string_free(s, TRUE);
            return NULL;
        }
        if (!synced) {
            synced = (unsigned char)c == 0xff;
            continue;
        }
        if (c == '\n') {
            if (!s->len)
                continue;
            return g_string_free(s, FALSE);
        }
        g_string_append_c(s, c);
    }
}

/* Connect and sync. Returns fd or -1 if the agent doesn't answer. */
static int qga_open(void)
{
    struct sockaddr_un sa = {.sun_family = AF_UNIX};
    snprintf(sa.sun_path, sizeof(sa.sun_path), "/var/run/qemu-server/%d.qga", g_vmid);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    struct timeval tv = {.tv_sec = QGA_TIMEOUT_MS / 1000,
                         .tv_usec = (QGA_TIMEOUT_MS % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
        goto fail;

    static unsigned sync_id;
    unsigned id = (unsigned)(now_us() & 0x7fffffff) ^ ++sync_id;
    char sync[128], want[32];
    int n = snprintf(sync, sizeof(sync),
                     "\xff{\"execute\":\"guest-sync-delimited\",\"arguments\":{\"id\":%u}}\n", id);
    snprintf(want, sizeof(want), "%u", id);
    if (!send_all(fd, sync, n))
        goto fail;
    /* stale replies a previous client left unread may come first */
    for (int i = 0; i < 8; i++) {
        char *line = read_line(fd, i == 0);
        if (!line)
            goto fail;
        bool ok = strstr(line, "\"return\"") && strstr(line, want);
        g_free(line);
        if (ok)
            return fd;
    }
fail:
    close(fd);
    return -1;
}

static char *qga_req(int fd, const char *json)
{
    if (!send_all(fd, json, strlen(json)) || !send_all(fd, "\n", 1))
        return NULL;
    return read_line(fd, false);
}

/* Tiny JSON field readers for the agent's flat replies. */
static const char *json_val(const char *js, const char *key)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(js, pat);
    if (!p)
        return NULL;
    p += strlen(pat);
    while (*p == ' ' || *p == ':')
        p++;
    return p;
}

static bool json_long(const char *js, const char *key, long *out)
{
    const char *p = json_val(js, key);
    char *end;
    if (!p)
        return false;
    *out = strtol(p, &end, 10);
    return end != p;
}

static char *json_str(const char *js, const char *key)
{
    const char *p = json_val(js, key);
    if (!p || *p != '"')
        return NULL;
    const char *e = strchr(p + 1, '"');
    return e ? g_strndup(p + 1, e - p - 1) : NULL;
}

/* {"return": <int>} */
static bool json_return_long(const char *js, long *out)
{
    return js && !strstr(js, "\"error\"") && json_long(js, "return", out);
}

/* PowerShell -EncodedCommand: base64 of UTF-16LE. */
static char *ps_encode(const char *script)
{
    glong n = 0;
    gunichar2 *u16 = g_utf8_to_utf16(script, -1, NULL, &n, NULL);
    if (!u16)
        return NULL;
    uint8_t *le = g_malloc(n * 2);
    for (glong i = 0; i < n; i++) {
        le[2 * i] = u16[i] & 0xff;
        le[2 * i + 1] = u16[i] >> 8;
    }
    char *b64 = g_base64_encode(le, n * 2);
    g_free(le);
    g_free(u16);
    return b64;
}

/* ------------------------------------------------------- guest files */

static bool guest_write(const char *path_json, const char *data)
{
    int fd = qga_open();
    if (fd < 0)
        return false;
    bool ok = false;
    char *cmd = g_strdup_printf(
        "{\"execute\":\"guest-file-open\",\"arguments\":{\"path\":\"%s\",\"mode\":\"w\"}}",
        path_json);
    char *r = qga_req(fd, cmd);
    g_free(cmd);
    long h;
    if (json_return_long(r, &h)) {
        char *b64 = g_base64_encode((const guchar *)data, strlen(data));
        cmd = g_strdup_printf(
            "{\"execute\":\"guest-file-write\",\"arguments\":{\"handle\":%ld,\"buf-b64\":\"%s\"}}",
            h, b64);
        g_free(b64);
        g_free(r);
        r = qga_req(fd, cmd);
        g_free(cmd);
        ok = r && !strstr(r, "\"error\"");
        g_free(r);
        cmd = g_strdup_printf("{\"execute\":\"guest-file-close\",\"arguments\":{\"handle\":%ld}}", h);
        r = qga_req(fd, cmd);
        g_free(cmd);
    }
    g_free(r);
    close(fd);
    return ok;
}

/* Returns file content (g_free) or NULL if missing/unreadable. */
static char *guest_read(const char *path_json)
{
    int fd = qga_open();
    if (fd < 0)
        return NULL;
    char *out = NULL;
    char *cmd = g_strdup_printf(
        "{\"execute\":\"guest-file-open\",\"arguments\":{\"path\":\"%s\",\"mode\":\"r\"}}",
        path_json);
    char *r = qga_req(fd, cmd);
    g_free(cmd);
    long h;
    if (json_return_long(r, &h)) {
        g_free(r);
        cmd = g_strdup_printf(
            "{\"execute\":\"guest-file-read\",\"arguments\":{\"handle\":%ld,\"count\":512}}", h);
        r = qga_req(fd, cmd);
        g_free(cmd);
        char *b64 = r ? json_str(r, "buf-b64") : NULL;
        if (b64) {
            gsize n;
            guchar *d = g_base64_decode(b64, &n);
            out = g_strndup((char *)d, n);
            g_free(d);
            g_free(b64);
        }
        g_free(r);
        cmd = g_strdup_printf("{\"execute\":\"guest-file-close\",\"arguments\":{\"handle\":%ld}}", h);
        r = qga_req(fd, cmd);
        g_free(cmd);
    }
    g_free(r);
    close(fd);
    return out;
}

/* --------------------------------------------------------------- resize */

static bool detect_os(void)
{
    int fd = qga_open();
    if (fd < 0)
        return false;
    char *r = qga_req(fd, "{\"execute\":\"guest-get-osinfo\"}");
    close(fd);
    if (!r)
        return false;
    char *id = json_str(r, "id");
    g_os = id && !strcmp(id, "mswindows") ? OS_WINDOWS : OS_OTHER;
    /* the id comes from the guest: keep a safe subset, it goes to viewers */
    size_t n = 0;
    for (const char *p = id ? id : "unknown"; *p && n < sizeof(g_os_id) - 1; p++)
        if (g_ascii_isalnum(*p) || *p == '.' || *p == '_' || *p == '-')
            g_os_id[n++] = g_ascii_tolower(*p);
    g_os_id[n] = 0;
    atomic_store(&g_os_known, true);
    atomic_store(&g_is_windows, g_os == OS_WINDOWS);
    if (g_os == OS_WINDOWS)
        LOG("guest-exec resize: Windows guest, resizing through the guest agent");
    else
        LOG("guest-exec resize: guest OS is '%s', not Windows: relying on "
            "the resize hint", id ? id : "?");
    g_free(id);
    g_free(r);
    return true;
}

/* Run START_SCRIPT and wait for it (it only registers and starts the task).
 * Returns its exit code, or -1. */
static long start_listener(void)
{
    int fd = qga_open();
    if (fd < 0)
        return -1;
    char *r = qga_req(fd, g_start_cmd);
    close(fd);
    long pid;
    if (!r || !json_long(r, "pid", &pid)) {
        LOG("guest-exec resize: guest-exec failed: %s", r ? r : "no reply");
        g_free(r);
        return -1;
    }
    g_free(r);
    char status[96];
    snprintf(status, sizeof(status),
             "{\"execute\":\"guest-exec-status\",\"arguments\":{\"pid\":%ld}}", pid);
    int64_t t0 = now_us();
    while (now_us() - t0 < 20 * 1000000LL) {
        g_usleep(100 * 1000);
        if ((fd = qga_open()) < 0)
            continue;
        r = qga_req(fd, status);
        close(fd);
        if (r && (strstr(r, "\"exited\":true") || strstr(r, "\"exited\": true"))) {
            long code = -1;
            json_long(r, "exitcode", &code);
            if (code != 0 && code != RC_NO_USER) {
                char *err = json_str(r, "err-data");
                gsize n = 0;
                guchar *d = err ? g_base64_decode(err, &n) : NULL;
                LOG("guest-exec resize: starting the listener failed (exit %ld)%s%.*s",
                    code, n ? ": " : "", (int)MIN(n, 300), d ? (char *)d : "");
                g_free(d);
                g_free(err);
            }
            g_free(r);
            return code;
        }
        g_free(r);
    }
    return -1;
}

static bool newer_pending(void)
{
    pthread_mutex_lock(&g_lock);
    bool p = g_pend;
    pthread_mutex_unlock(&g_lock);
    return p;
}

#define ACK_NONE (-1)       /* no answer */
#define ACK_SUPERSEDED (-2) /* a newer size is waiting: stop on this one */

/* Poll this request's ack file. Each request gets its own file, written once
 * by the listener and never rewritten: the agent's handles (it opens files
 * without write sharing) can't make the listener's write fail. With
 * check_alive, give up early when no listener is running (resize.alive is
 * missing; the listener deletes it when it exits idle). */
static long wait_ack(uint64_t seq, int timeout_ms, bool check_alive)
{
    char path[160], want[32];
    snprintf(path, sizeof(path), ACK_FILE, (unsigned long long)seq);
    snprintf(want, sizeof(want), "%llu ", (unsigned long long)seq);
    int64_t t0 = now_us();
    bool alive_checked = !check_alive;
    do {
        char *a = guest_read(path);
        if (a && g_str_has_prefix(a, want) && strstr(a, " end")) {
            long rc = strtol(a + strlen(want), NULL, 10);
            g_free(a);
            return rc;
        }
        g_free(a);
        if (newer_pending())
            return ACK_SUPERSEDED;
        if (!alive_checked && now_us() - t0 > ALIVE_CHECK_MS * 1000LL) {
            alive_checked = true;
            char *al = guest_read(ALIVE_FILE);
            g_free(al);
            if (!al)
                return ACK_NONE;
        }
        g_usleep(ACK_POLL_MS * 1000);
    } while (now_us() - t0 < timeout_ms * 1000LL);
    return ACK_NONE;
}

/* The listener may be reading the request file at that moment (the agent
 * then can't open it): retry briefly. False = directory missing or the
 * agent refuses. */
static bool write_req(const char *req)
{
    for (int i = 0; i < WRITE_TRIES; i++) {
        if (guest_write(REQ_FILE, req))
            return true;
        g_usleep(30 * 1000);
    }
    return false;
}

static void log_result(uint32_t w, uint32_t h, long rc, int64_t t0, bool started)
{
    long ms = (long)((now_us() - t0) / 1000);
    if (rc == 0)
        LOG("guest-exec resize: Windows switched to %ux%u in %ld ms%s", w, h, ms,
            started ? " (listener started)" : "");
    else if (rc >= 0x10000 && rc < 0x60000)
        LOG("guest-exec resize: %s failed with Win32 error %ld",
            rc >> 16 == 5 ? "SetDisplayConfig"
            : rc >> 16 <= 2 ? "QueryDisplayConfig" : "finding the display path",
            rc & 0xffff);
    else
        LOG("guest-exec resize: listener failed (result 0x%lx)", (unsigned long)rc);
}

static void do_resize(uint32_t w, uint32_t h)
{
    if (g_os == OS_UNKNOWN && !detect_os()) {
        LOG("guest-exec resize: QEMU guest agent not responding (installed "
            "and running in the guest? 'agent: 1' in the VM config?)");
        return;
    }
    if (g_os != OS_WINDOWS)
        return;

    int64_t t0 = now_us();
    uint64_t seq = ++g_seq;
    char req[64];
    snprintf(req, sizeof(req), "%llu %u %u end\n", (unsigned long long)seq, w, h);

    /* fast path: a listener is already running */
    bool written = write_req(req);
    long rc = written ? wait_ack(seq, ACK_FAST_MS, true) : ACK_NONE;
    if (rc == ACK_SUPERSEDED)
        return;
    if (rc >= 0) {
        log_result(w, h, rc, t0, false);
        return;
    }

    /* none (first resize, user logged off/on, idle exit): start one; it picks
     * up the pending request when it starts */
    LOG("guest-exec resize: starting the resize listener in the guest");
    long code = start_listener();
    if (code == RC_NO_USER) {
        LOG("guest-exec resize: nobody is logged in on the guest console; "
            "Windows can only be resized inside a user session");
        return;
    }
    if (code != 0)
        return;
    if (!write_req(req)) {
        LOG("guest-exec resize: can't write the request file in the guest "
            "(guest-file-* commands blocked in the agent config?)");
        return;
    }
    rc = wait_ack(seq, ACK_START_MS, false);
    if (rc == ACK_SUPERSEDED)
        return;
    if (rc < 0)
        LOG("guest-exec resize: the listener didn't answer within %d s",
            ACK_START_MS / 1000);
    else
        log_result(w, h, rc, t0, true);
}

static void *worker(void *arg)
{
    /* learn the guest OS early, so the first resize doesn't wait for it */
    detect_os();
    for (;;) {
        pthread_mutex_lock(&g_lock);
        while (!g_pend)
            pthread_cond_wait(&g_cond, &g_lock);
        uint32_t w = g_pend_w, h = g_pend_h;
        g_pend = false;
        pthread_mutex_unlock(&g_lock);
        do_resize(w, h);
    }
    return NULL;
}

void guestexec_init(int vmid, bool enabled)
{
    g_vmid = vmid;
    g_enabled = enabled;
    if (!enabled) {
        LOG("guest-exec resize disabled (guest_exec_resize = 0)");
        return;
    }
    /* ms since the epoch: unique across daemon restarts, so a stale ack of a
     * previous daemon never matches */
    g_seq = (uint64_t)g_get_real_time() / 1000;

    char *listener = g_strdup_printf(LISTENER_SCRIPT, LISTENER_IDLE_MIN);
    char *listener_b64 = ps_encode(listener);
    char *start = g_strdup_printf(START_SCRIPT, listener_b64);
    char *start_b64 = ps_encode(start);
    g_start_cmd = g_strdup_printf(
        "{\"execute\":\"guest-exec\",\"arguments\":{\"path\":\"powershell.exe\","
        "\"arg\":[\"-NoProfile\",\"-NonInteractive\",\"-ExecutionPolicy\",\"Bypass\","
        "\"-EncodedCommand\",\"%s\"],\"capture-output\":true}}", start_b64);
    g_free(listener);
    g_free(listener_b64);
    g_free(start);
    g_free(start_b64);

    pthread_t t;
    if (pthread_create(&t, NULL, worker, NULL) == 0)
        pthread_detach(t);
    else
        g_enabled = false;
}

const char *guestexec_os_id(void)
{
    return atomic_load(&g_os_known) ? g_os_id : "";
}

bool guestexec_is_windows(void)
{
    return g_enabled && atomic_load(&g_is_windows);
}

void guestexec_request_size(uint32_t w, uint32_t h)
{
    if (!g_enabled)
        return;
    pthread_mutex_lock(&g_lock);
    g_pend_w = w;
    g_pend_h = h;
    g_pend = true;
    pthread_cond_signal(&g_cond);
    pthread_mutex_unlock(&g_lock);
}
