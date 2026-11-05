/* SPDX-License-Identifier: MIT */
/*
 * pve-h264d - spice-vdagent client over QEMU's D-Bus chardev.
 *
 * The Windows virtio-gpu driver ignores Console.SetUIInfo, and the SPICE
 * server turns VD_AGENT_MONITORS_CONFIG into that same hint instead of
 * forwarding it. With the agent's virtio-serial port (com.redhat.spice.0) on a
 * `-chardev dbus` (see patches/pve-h264-patch), we register a socket on it and
 * play the SPICE server+client side of the agent protocol ourselves: announce
 * capabilities, then send the wanted resolution as a monitors config. The
 * agent in the guest applies it with ChangeDisplaySettingsEx.
 *
 * The same channel shares the clipboard (UTF-8 text) with the viewers, the
 * way spice-gtk does it ("by demand"): whoever copies announces a GRAB with
 * the types it has; the other side REQUESTs the data when it needs it, and
 * the owner answers with a CLIPBOARD message. Guest -> viewers: we request
 * the text as soon as the guest grabs (browsers can't be asked later) and
 * hand it to pve-h264d.c. Viewer -> guest: we grab for the guest and keep the
 * text until the agent requests it (when a guest program pastes). Line ends
 * are converted for guests that announce CRLF (Windows), as spice-gtk does.
 *
 * Protocol (spice-protocol vd_agent.h, all little endian, packed):
 *   VDIChunkHeader  { u32 port; u32 size; }              port 1 = client
 *   VDAgentMessage  { u32 protocol=1; u32 type; u64 opaque; u32 size; data }
 * A message may span several chunks.
 *
 * Main thread only.
 */
#include "pve-h264d.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glib-unix.h>

#define CHARDEV_PATH "/org/qemu/Display1/Chardev_vdagent"
#define CHARDEV_IFACE "org.qemu.Display1.Chardev"

#define VDP_CLIENT_PORT 1
#define VD_AGENT_PROTOCOL 1
#define CHUNK_HDR 8
#define MSG_HDR 20
#define CHUNK_DATA 2048              /* VD_AGENT_MAX_DATA_SIZE */
#define MAX_MSG (CLIPBOARD_MAX + 256)
#define TX_MAX (4 * CLIPBOARD_MAX)    /* queued for a slow (or stopped) agent */

enum {
    VD_AGENT_MONITORS_CONFIG = 2,
    VD_AGENT_REPLY = 3,
    VD_AGENT_CLIPBOARD = 4,
    VD_AGENT_ANNOUNCE_CAPABILITIES = 6,
    VD_AGENT_CLIPBOARD_GRAB = 7,
    VD_AGENT_CLIPBOARD_REQUEST = 8,
    VD_AGENT_CLIPBOARD_RELEASE = 9,
    VD_AGENT_MAX_CLIPBOARD = 14,
};
enum { VD_AGENT_SUCCESS = 1, VD_AGENT_ERROR = 2 }; /* VDAgentReply.error */
enum {
    VD_AGENT_CAP_MONITORS_CONFIG = 1,
    VD_AGENT_CAP_REPLY = 2,
    VD_AGENT_CAP_CLIPBOARD_BY_DEMAND = 5,
    /* not announced: without CLIPBOARD_SELECTION the messages carry no
     * selection header and only the CLIPBOARD selection is shared */
    VD_AGENT_CAP_GUEST_LINEEND_LF = 8,
    VD_AGENT_CAP_GUEST_LINEEND_CRLF = 9,
    VD_AGENT_CAP_MAX_CLIPBOARD = 10,
};
enum { VD_AGENT_CLIPBOARD_NONE = 0, VD_AGENT_CLIPBOARD_UTF8_TEXT = 1 };

static GDBusConnection *g_conn;
static int g_fd = -1;
static guint g_watch, g_out_watch;
static bool g_no_port;              /* agent port not on D-Bus */
static int g_clip_mode;             /* CLIP_TO_GUEST | CLIP_FROM_GUEST */
static bool g_agent_ready;          /* agent announced its capabilities */
static uint32_t g_agent_caps;
static uint32_t g_want_w, g_want_h; /* pending/last requested size, 0 = none */

/* receive state */
static GByteArray *g_rx;            /* raw stream bytes */
static GByteArray *g_msg;           /* message being reassembled */
static uint32_t g_chunk_left;       /* bytes of the current chunk still due */
static uint32_t g_skip;             /* bytes of an oversized message to drop */
static GByteArray *g_tx;            /* not yet written to the port */

/* clipboard */
static bool g_clip_owner;           /* we grabbed the guest clipboard */
static GByteArray *g_clip_offer;    /* viewer text the guest may request */
static bool g_clip_requested;       /* asked the guest for its text */
static char *g_clip_state;          /* last state handed out (json) */

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}
static uint32_t get32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static void tx_stop(void)
{
    if (g_out_watch)
        g_source_remove(g_out_watch);
    g_out_watch = 0;
    g_byte_array_set_size(g_tx, 0);
}

static gboolean on_writable(gint fd, GIOCondition cond, gpointer ud);

static void tx_flush(void)
{
    while (g_tx->len) {
        ssize_t r = send(g_fd, g_tx->data, g_tx->len, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0 && errno == EAGAIN) {
            if (!g_out_watch)
                g_out_watch = g_unix_fd_add(g_fd, G_IO_OUT, on_writable, NULL);
            return;
        }
        if (r < 0) {
            LOG("vdagent: write failed (%s), %u bytes dropped", strerror(errno),
                g_tx->len);
            tx_stop();
            return;
        }
        g_byte_array_remove_range(g_tx, 0, r);
    }
    if (g_out_watch)
        g_source_remove(g_out_watch);
    g_out_watch = 0;
}

static gboolean on_writable(gint fd, GIOCondition cond, gpointer ud)
{
    g_out_watch = 0; /* tx_flush adds a new watch if it still can't finish */
    tx_flush();
    return G_SOURCE_REMOVE;
}

/* Queue one message (split into chunks of at most CHUNK_DATA) and write what
 * the socket takes now. data2 is appended to data (payload header + body). */
static void agent_send2(uint32_t type, const void *data, uint32_t len,
                        const void *data2, uint32_t len2)
{
    if (g_fd < 0)
        return;
    uint32_t total = MSG_HDR + len + len2;
    if (g_tx->len + total + total / CHUNK_DATA * CHUNK_HDR + CHUNK_HDR > TX_MAX) {
        LOG("vdagent: guest agent not reading, message 0x%x dropped", type);
        return;
    }
    uint8_t m[MSG_HDR];
    put32(m, VD_AGENT_PROTOCOL);
    put32(m + 4, type);
    memset(m + 8, 0, 8); /* opaque */
    put32(m + 16, len + len2);
    const uint8_t *parts[3] = {m, data, data2};
    uint32_t plen[3] = {MSG_HDR, len, len2};
    int pi = 0;
    uint32_t poff = 0;
    while (total) {
        uint32_t n = MIN(total, CHUNK_DATA);
        uint8_t ch[CHUNK_HDR];
        put32(ch, VDP_CLIENT_PORT);
        put32(ch + 4, n);
        g_byte_array_append(g_tx, ch, CHUNK_HDR);
        total -= n;
        while (n) {
            uint32_t take = MIN(n, plen[pi] - poff);
            if (take)
                g_byte_array_append(g_tx, parts[pi] + poff, take);
            poff += take;
            n -= take;
            if (poff == plen[pi]) {
                pi++;
                poff = 0;
            }
        }
    }
    if (!g_out_watch)
        tx_flush();
}

static void agent_send(uint32_t type, const void *data, uint32_t len)
{
    agent_send2(type, data, len, NULL, 0);
}

static bool clip_capable(void)
{
    return g_clip_mode && g_agent_ready &&
           (g_agent_caps & 1u << VD_AGENT_CAP_CLIPBOARD_BY_DEMAND);
}

/* Tell pve-h264d.c (and through it the viewers) when availability changes. */
static void clip_state_update(void)
{
    char *js = vdagent_clipboard_state_json();
    if (!g_clip_state || strcmp(js, g_clip_state)) {
        g_free(g_clip_state);
        g_clip_state = js;
        clipboard_state_changed();
    } else {
        g_free(js);
    }
}

char *vdagent_clipboard_state_json(void)
{
    const char *reason = "";
    if (!g_clip_mode)
        reason = "clipboard sharing is disabled on the host (clipboard = off)";
    else if (g_no_port)
        reason = "no spice agent port: needs vga virtio/qxl, and a VM restart "
                 "after installing pve-h264-console";
    else if (g_fd < 0)
        reason = "connecting to the spice agent port";
    else if (!g_agent_ready)
        reason = "the spice agent in the guest is not running (install "
                 "spice-vdagent / the virtio-win guest tools)";
    else if (!clip_capable())
        reason = "the guest's spice agent has no clipboard support";
    bool ok = !*reason;
    return g_strdup_printf(
        "{\"available\":%s,\"to_guest\":%s,\"from_guest\":%s,"
        "\"max\":%d,\"reason\":\"%s\"}",
        ok ? "true" : "false",
        ok && (g_clip_mode & CLIP_TO_GUEST) ? "true" : "false",
        ok && (g_clip_mode & CLIP_FROM_GUEST) ? "true" : "false", CLIPBOARD_MAX,
        reason);
}

/* Convert line ends for the guest (to_guest) or from it. Viewers use LF. */
static GByteArray *convert_eol(const uint8_t *s, size_t n, bool to_guest)
{
    bool crlf = g_agent_caps & 1u << VD_AGENT_CAP_GUEST_LINEEND_CRLF;
    GByteArray *o = g_byte_array_sized_new(n + n / 16 + 1);
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\r' && i + 1 < n && s[i + 1] == '\n') {
            if (to_guest && crlf)
                g_byte_array_append(o, (const uint8_t *)"\r", 1);
            continue; /* the \n follows */
        }
        if (s[i] == '\n' && to_guest && crlf)
            g_byte_array_append(o, (const uint8_t *)"\r", 1);
        g_byte_array_append(o, s + i, 1);
    }
    return o;
}

static void clip_reset(void)
{
    g_clip_owner = false;
    g_clip_requested = false;
    g_byte_array_set_size(g_clip_offer, 0);
}

static void send_clip_type(uint32_t msg, uint32_t type)
{
    uint8_t d[4];
    put32(d, type);
    agent_send(msg, d, sizeof(d));
}

static void clip_on_grab(const uint8_t *d, uint32_t len)
{
    /* the guest copied something: whatever a viewer offered is stale */
    g_clip_owner = false;
    g_byte_array_set_size(g_clip_offer, 0);
    bool text = false;
    for (uint32_t i = 0; i + 4 <= len; i += 4)
        text |= get32(d + i) == VD_AGENT_CLIPBOARD_UTF8_TEXT;
    DBG("vdagent: guest clipboard grab (%u types%s)", len / 4, text ? ", text" : "");
    if (!text || !(g_clip_mode & CLIP_FROM_GUEST))
        return;
    g_clip_requested = true;
    send_clip_type(VD_AGENT_CLIPBOARD_REQUEST, VD_AGENT_CLIPBOARD_UTF8_TEXT);
}

static void clip_on_data(const uint8_t *d, uint32_t len)
{
    if (len < 4 || !g_clip_requested)
        return;
    g_clip_requested = false;
    if (get32(d) != VD_AGENT_CLIPBOARD_UTF8_TEXT)
        return;
    d += 4;
    len -= 4;
    while (len && !d[len - 1]) /* some agents send the terminating NUL */
        len--;
    GByteArray *t = convert_eol(d, len, false);
    if (!g_utf8_validate((const char *)t->data, t->len, NULL)) {
        LOG("vdagent: guest clipboard text is not valid UTF-8, ignored");
    } else {
        DBG("vdagent: guest clipboard text, %u bytes", t->len);
        clipboard_from_guest((const char *)t->data, t->len);
    }
    g_byte_array_unref(t);
}

static void clip_on_request(const uint8_t *d, uint32_t len)
{
    if (len < 4)
        return;
    uint32_t type = get32(d);
    if (!g_clip_owner || type != VD_AGENT_CLIPBOARD_UTF8_TEXT) {
        /* nothing to give: answer anyway so the guest program doesn't wait */
        send_clip_type(VD_AGENT_CLIPBOARD, VD_AGENT_CLIPBOARD_NONE);
        return;
    }
    uint8_t h[4];
    put32(h, VD_AGENT_CLIPBOARD_UTF8_TEXT);
    agent_send2(VD_AGENT_CLIPBOARD, h, 4, g_clip_offer->data, g_clip_offer->len);
    DBG("vdagent: gave the guest %u bytes of clipboard text", g_clip_offer->len);
}

bool vdagent_clipboard_set(const char *text, size_t len)
{
    if (!clip_capable() || !(g_clip_mode & CLIP_TO_GUEST) || len > CLIPBOARD_MAX)
        return false;
    GByteArray *t = convert_eol((const uint8_t *)text, len, true);
    g_byte_array_set_size(g_clip_offer, 0);
    g_byte_array_append(g_clip_offer, t->data, t->len);
    g_byte_array_unref(t);
    g_clip_owner = true;
    g_clip_requested = false;
    send_clip_type(VD_AGENT_CLIPBOARD_GRAB, VD_AGENT_CLIPBOARD_UTF8_TEXT);
    DBG("vdagent: offered %u bytes of viewer clipboard text", g_clip_offer->len);
    return true;
}

static void send_caps(bool request)
{
    uint8_t d[8];
    put32(d, request);
    uint32_t caps = 1u << VD_AGENT_CAP_MONITORS_CONFIG | 1u << VD_AGENT_CAP_REPLY;
    if (g_clip_mode)
        caps |= 1u << VD_AGENT_CAP_CLIPBOARD_BY_DEMAND |
                1u << VD_AGENT_CAP_MAX_CLIPBOARD;
    put32(d + 4, caps);
    agent_send(VD_AGENT_ANNOUNCE_CAPABILITIES, d, sizeof(d));
}

static void send_monitors(void)
{
    if (!g_agent_ready || !g_want_w)
        return;
    if (!(g_agent_caps & 1u << VD_AGENT_CAP_MONITORS_CONFIG)) {
        LOG("vdagent: guest agent does not support monitors config");
        return;
    }
    uint8_t d[8 + 20];
    put32(d, 1);      /* num_of_monitors */
    put32(d + 4, 0);  /* flags */
    put32(d + 8, g_want_h);
    put32(d + 12, g_want_w);
    put32(d + 16, 32); /* depth */
    put32(d + 20, 0);  /* x */
    put32(d + 24, 0);  /* y */
    agent_send(VD_AGENT_MONITORS_CONFIG, d, sizeof(d));
    LOG("vdagent: requested guest resolution %ux%u", g_want_w, g_want_h);
}

static void handle_message(const uint8_t *m, uint32_t n)
{
    uint32_t type = get32(m + 4);
    const uint8_t *d = m + MSG_HDR;
    uint32_t len = n - MSG_HDR;
    switch (type) {
    case VD_AGENT_ANNOUNCE_CAPABILITIES:
        if (len < 8)
            return;
        g_agent_caps = get32(d + 4);
        if (!g_agent_ready)
            LOG("vdagent: guest agent connected (caps 0x%x)", g_agent_caps);
        g_agent_ready = true;
        if (get32(d))
            send_caps(false);
        if (clip_capable() && (g_agent_caps & 1u << VD_AGENT_CAP_MAX_CLIPBOARD)) {
            uint8_t mx[4];
            put32(mx, CLIPBOARD_MAX);
            agent_send(VD_AGENT_MAX_CLIPBOARD, mx, sizeof(mx));
        }
        clip_state_update();
        send_monitors();
        break;
    case VD_AGENT_CLIPBOARD_GRAB:
        if (clip_capable())
            clip_on_grab(d, len);
        break;
    case VD_AGENT_CLIPBOARD:
        clip_on_data(d, len);
        break;
    case VD_AGENT_CLIPBOARD_REQUEST:
        clip_on_request(d, len);
        break;
    case VD_AGENT_CLIPBOARD_RELEASE:
        DBG("vdagent: guest clipboard released");
        g_clip_owner = false;
        break;
    case VD_AGENT_REPLY:
        if (len >= 8 && get32(d) == VD_AGENT_MONITORS_CONFIG)
            LOG("vdagent: guest agent reply to monitors config: %s (%u)",
                get32(d + 4) == VD_AGENT_SUCCESS ? "success" : "error",
                get32(d + 4));
        break;
    default:
        DBG("vdagent: ignoring message type %u (%u bytes)", type, len);
    }
}

/* Feed stream bytes: strip chunk headers, reassemble and dispatch messages. */
static bool parse_rx(void)
{
    for (;;) {
        if (!g_chunk_left) {
            if (g_rx->len < CHUNK_HDR)
                return true;
            g_chunk_left = get32(g_rx->data + 4);
            g_byte_array_remove_range(g_rx, 0, CHUNK_HDR);
            continue;
        }
        if (!g_rx->len)
            return true;
        guint take = MIN(g_chunk_left, g_rx->len);
        g_chunk_left -= take;
        if (g_skip) {
            guint s = MIN(take, g_skip);
            g_skip -= s;
            g_byte_array_remove_range(g_rx, 0, s);
            g_chunk_left += take - s;
            continue;
        }
        g_byte_array_append(g_msg, g_rx->data, take);
        g_byte_array_remove_range(g_rx, 0, take);
        while (g_msg->len >= MSG_HDR) {
            uint32_t full = MSG_HDR + get32(g_msg->data + 16);
            if (get32(g_msg->data) != VD_AGENT_PROTOCOL) {
                LOG("vdagent: protocol error from guest agent, resyncing");
                g_byte_array_set_size(g_msg, 0);
                break;
            }
            if (full > MAX_MSG) {
                LOG("vdagent: dropping a %u byte message 0x%x from the guest agent",
                    full, get32(g_msg->data + 4));
                if (get32(g_msg->data + 4) == VD_AGENT_CLIPBOARD)
                    g_clip_requested = false;
                guint have = MIN(g_msg->len, full);
                g_skip = full - have;
                g_byte_array_remove_range(g_msg, 0, have);
                continue;
            }
            if (g_msg->len < full)
                break;
            handle_message(g_msg->data, full);
            g_byte_array_remove_range(g_msg, 0, full);
        }
    }
}

static void reset_rx(void)
{
    g_byte_array_set_size(g_rx, 0);
    g_byte_array_set_size(g_msg, 0);
    g_chunk_left = 0;
    g_skip = 0;
}

static gboolean on_io(gint fd, GIOCondition cond, gpointer ud)
{
    uint8_t buf[4096];
    ssize_t r = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
    if (r < 0 && (errno == EAGAIN || errno == EINTR))
        return G_SOURCE_CONTINUE;
    if (r <= 0) {
        LOG("vdagent: agent port closed by QEMU");
        tx_stop();
        close(g_fd);
        g_fd = -1;
        g_watch = 0;
        g_agent_ready = false;
        g_no_port = true;
        clip_reset();
        clip_state_update();
        return G_SOURCE_REMOVE;
    }
    g_byte_array_append(g_rx, buf, r);
    parse_rx();
    return G_SOURCE_CONTINUE;
}

static void on_props(GDBusConnection *c, const gchar *sender, const gchar *path,
                     const gchar *iface, const gchar *signal, GVariant *params,
                     gpointer ud)
{
    const gchar *ifname;
    g_autoptr(GVariant) changed = NULL;
    g_autoptr(GVariant) inval = NULL;
    g_variant_get(params, "(&s@a{sv}@as)", &ifname, &changed, &inval);
    gboolean open;
    if (strcmp(ifname, CHARDEV_IFACE) ||
        !g_variant_lookup(changed, "FEOpened", "b", &open))
        return;
    /* The agent (re)opened or closed its port: start over with a fresh
     * capabilities handshake; it answers with its own and we resend the size. */
    g_agent_ready = false;
    reset_rx();
    tx_stop();
    clip_reset();
    clip_state_update();
    DBG("vdagent: guest agent port %s", open ? "opened" : "closed");
    if (open)
        send_caps(true);
}

static int g_pending_fd = -1;

static void on_registered(GObject *src, GAsyncResult *res, gpointer ud)
{
    GError *err = NULL;
    GVariant *v = g_dbus_connection_call_with_unix_fd_list_finish(
        G_DBUS_CONNECTION(src), NULL, res, &err);
    int fd = g_pending_fd;
    g_pending_fd = -1;
    if (!v) {
        LOG("vdagent: spice agent port not on D-Bus (%s): guest resize relies "
            "on the GPU driver only (Windows guests won't follow). Restart the "
            "VM after installing/updating pve-h264-console to enable it.",
            err->message);
        g_error_free(err);
        close(fd);
        g_no_port = true;
        clip_state_update();
        return;
    }
    g_variant_unref(v);
    g_fd = fd;
    g_watch = g_unix_fd_add(g_fd, G_IO_IN | G_IO_HUP | G_IO_ERR, on_io, NULL);
    g_dbus_connection_signal_subscribe(
        g_conn, NULL, "org.freedesktop.DBus.Properties", "PropertiesChanged",
        CHARDEV_PATH, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_props, NULL, NULL);
    LOG("vdagent: attached to spice agent port");
    /* If the agent is already running it answers this; if not, it announces
     * itself when it starts. */
    send_caps(true);
}

/* Asynchronous on purpose: right after RegisterListener QEMU makes blocking
 * calls into us (ScanoutMap), so a sync call here would deadlock until timeout. */
void vdagent_attach(void *conn, int clip_mode)
{
    g_conn = conn;
    g_clip_mode = clip_mode;
    g_rx = g_byte_array_new();
    g_msg = g_byte_array_new();
    g_tx = g_byte_array_new();
    g_clip_offer = g_byte_array_new();

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
        g_no_port = true;
        return;
    }
    GUnixFDList *fdl = g_unix_fd_list_new();
    int idx = g_unix_fd_list_append(fdl, sv[1], NULL);
    close(sv[1]);
    g_pending_fd = sv[0];
    g_dbus_connection_call_with_unix_fd_list(
        g_conn, NULL, CHARDEV_PATH, CHARDEV_IFACE, "Register",
        g_variant_new("(h)", idx), NULL, G_DBUS_CALL_FLAGS_NONE, 5000, fdl,
        NULL, on_registered, NULL);
    g_object_unref(fdl);
}

void vdagent_request_size(uint32_t w, uint32_t h)
{
    g_want_w = w;
    g_want_h = h;
    if (g_fd >= 0)
        send_monitors();
}
