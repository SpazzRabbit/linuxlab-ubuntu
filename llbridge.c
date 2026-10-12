/*
 * llbridge: puts the screen of a headless GNOME Shell into Honor's X server and passes the input back.
 *
 * Linux Lab shows one thing only: the framebuffer of its own Xvfb. GNOME Shell 50 is a Wayland
 * compositor and cannot draw into an X server. So the shell runs with no screen at all
 * (gnome-shell --headless --virtual-monitor WxH), and this program is the screen and the input devices:
 *
 *   picture   org.gnome.Mutter.ScreenCast records the virtual monitor into a PipeWire stream.
 *             With --framebuffer, the damaged parts of every frame go straight into the screen file
 *             that Linux Lab shows (see "picture" below); otherwise into a window over the X screen
 *   touches   XInput2 touch events of that window (the lltouch module makes them real) go to
 *             org.gnome.Mutter.RemoteDesktop as touch points, so GNOME sees a touchscreen
 *   pointer   motion, buttons and wheel of the X pointer become an absolute pointer
 *   keys      X keycodes are evdev codes plus 8 here, they are passed as key codes;
 *             layouts are GNOME's business
 *   turning   Linux Lab's window is always landscape (Honor locked it), so a desktop turned upright
 *             is GNOME's doing: the monitor gets a rotation, its picture arrives tall, and it is
 *             written into the wide screen turned back; touches go the other way (see "turning")
 *   clipboard Honor's X server keeps the CLIPBOARD selection of its display and the clipboard of
 *             Android the same. A text copied in GNOME is offered on the X display, a text that
 *             appears there is offered to GNOME (see "clipboard" below)
 *
 * The cursor is part of the recorded picture, the X cursor is hidden.
 * Frames that keep a rhythm, a video, are shown on its beat (see "pacing"); --no-pacing shows every
 * frame the moment it comes.
 * kill -USR1 turns on a line of frame statistics per second (in ~/.cache/linuxlab-gnome/bridge.log).
 *
 * Build: gcc -O2 -o llbridge llbridge.c \
 *        $(pkg-config --cflags --libs gio-unix-2.0 libpipewire-0.3 x11 xi xfixes)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#include <glib-unix.h>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XInput2.h>
#include <X11/extensions/Xfixes.h>

#include "llcapture.h"

#define RD_NAME "org.gnome.Mutter.RemoteDesktop"
#define RD_PATH "/org/gnome/Mutter/RemoteDesktop"
#define RD_IFACE "org.gnome.Mutter.RemoteDesktop"
#define RD_SESSION_IFACE "org.gnome.Mutter.RemoteDesktop.Session"
#define SC_NAME "org.gnome.Mutter.ScreenCast"
#define SC_PATH "/org/gnome/Mutter/ScreenCast"
#define SC_IFACE "org.gnome.Mutter.ScreenCast"
#define SC_SESSION_IFACE "org.gnome.Mutter.ScreenCast.Session"
#define SC_STREAM_IFACE "org.gnome.Mutter.ScreenCast.Stream"
#define DC_NAME "org.gnome.Mutter.DisplayConfig"
#define DC_PATH "/org/gnome/Mutter/DisplayConfig"
#define DC_IFACE "org.gnome.Mutter.DisplayConfig"

#define CURSOR_EMBEDDED 1
#define EXIT_TURNED 75 /* the monitors were changed: start the bridge again, nothing is wrong */
#define MAX_SLOTS 16
#define MAX_RECTS 32
/* evdev button codes */
#define BTN_LEFT 0x110
#define BTN_RIGHT 0x111
#define BTN_MIDDLE 0x112
#define BTN_SIDE 0x113
#define BTN_EXTRA 0x114

#define FB_BUFFERS 3
#define FB_HISTORY 8

/* the kinds of text a clipboard may hold, the best first; the names are the same on both sides */
static const char *const text_types[] = {"text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "STRING", "TEXT"};
#define TEXT_TYPES G_N_ELEMENTS(text_types)
#define CLIP_MAX (4 * 1024 * 1024)

struct rect {
    int x, y, w, h;
};

/* what changed in one presented frame */
struct damage {
    guint64 frame;
    gboolean all;
    int count;
    struct rect rects[MAX_RECTS];
};

/* the screen file of Linux Lab, with three buffers of our own swapped in and out under its name */
struct fb {
    int dir;
    char *name;                /* Xvfb_screen0 */
    char *saved;               /* the name Xvfb's own file waits under while the bridge runs */
    char *spare[2];            /* names of the two buffers not on screen */
    size_t size;
    int stride;
    int width, height;
    uint8_t *map[FB_BUFFERS];
    guint64 holds[FB_BUFFERS]; /* frame each buffer shows, 0 for none */
    int front;
    int at_spare[2];
    int next_spare;
    int pending[2];            /* spare slots (0 or 1) holding frames not shown yet, oldest first */
    gint64 due[2];             /* when each is to be shown */
    int waiting;               /* how many of them */
    guint64 frame;
    struct damage history[FB_HISTORY];
};

/*
 * The rhythm of the frames, for showing a video evenly (see "pacing" below).
 * period: the interval the frames keep, in microseconds; expected: when the last frame was due by
 * that rhythm; intervals: the last ones, from which the rhythm is read.
 */
#define PACE_WINDOW 16

struct pace {
    gint64 period;
    gint64 expected;
    gint64 last;
    gint64 intervals[PACE_WINDOW]; /* the last ones, a ring */
    int seen;                      /* frames counted into the ring */
    GSource *timer;
};

struct bridge {
    GMainLoop *loop;
    int exit_code;
    gboolean verbose;

    GDBusConnection *bus;
    char *rd_session;
    char *sc_session;
    char *stream_path;
    const char *connector;

    Display *dpy;
    Window win;
    GC gc;
    Visual *visual;
    int depth;
    int width, height;
    int xi_opcode;

    struct pw_loop *pw_loop;
    struct pw_context *pw_context;
    struct pw_core *pw_core;
    struct pw_stream *stream;
    struct spa_hook stream_listener;
    struct spa_hook core_listener;
    struct spa_video_info_raw format;
    gboolean have_format;
    struct pw_buffer *held; /* the frame that is on screen, kept for repaints */
    gboolean first_frame;
    struct fb *fb;          /* NULL: frames go to the X window */
    struct pace pace;
    gboolean no_pacing;
    int turn;               /* the rotation of the monitor as Mutter numbers it: 0, 1 (90), 2, 3 (270) */
    gboolean turn_known;    /* asked and answered since the format of the stream last changed */
    gboolean turn_asked;
    const struct llcapture_shared *capture; /* the true lists of changes, from llcapture.so */
    guint64 capture_frame;                  /* the last frame of the shell that reached the screen */

    int xfixes_event;
    Atom atom_clipboard, atom_targets, atom_incr, atom_transfer;
    Atom atom_text[TEXT_TYPES];
    GBytes *clip;            /* the text both sides have now */
    gboolean clip_from_x;    /* it came from the X side (Android); GNOME has it from the bridge */
    GCancellable *clip_read; /* a text of GNOME on its way here */
    GByteArray *clip_pieces; /* a text of the X side arriving in pieces */

    int touch_ids[MAX_SLOTS]; /* X touch id per GNOME slot, -1 when free */
    gboolean keys[256];       /* X keycodes that are down */
    gboolean buttons[16];

    gint64 stat_since;
    int stat_frames;
    int stat_paced;    /* frames shown on the beat */
    int stat_late;     /* frames that missed their beat and were shown at once */
    int stat_replaced; /* frames a later one took the place of while they waited */
    gint64 stat_timer_late; /* the most the beat came late by, µs */
    gint64 stat_put_us;
    gint64 stat_pixels;
};

static void say(const char *format, ...) G_GNUC_PRINTF(1, 2);

static void say(const char *format, ...)
{
    va_list ap;

    fputs("llbridge: ", stderr);
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void fail(struct bridge *b, const char *what)
{
    say("%s", what);
    b->exit_code = 1;
    g_main_loop_quit(b->loop);
}

/* ---------- GNOME side: D-Bus ---------- */

static void notify_done(GObject *source, GAsyncResult *result, gpointer data)
{
    struct bridge *b = data;
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);

    if (!reply && b->verbose)
        say("input was not accepted: %s", error->message);
}

/* one input event; the calls of one connection keep their order */
static void notify(struct bridge *b, const char *method, GVariant *args)
{
    g_dbus_connection_call(b->bus, RD_NAME, b->rd_session, RD_SESSION_IFACE, method, args, NULL,
                           G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, NULL, notify_done, b);
}

static void on_session_closed(GDBusConnection *bus, const char *sender, const char *path, const char *iface,
                              const char *signal, GVariant *args, gpointer data)
{
    struct bridge *b = data;

    say("GNOME closed the session");
    b->exit_code = 1;
    g_main_loop_quit(b->loop);
}

static void connect_stream(struct bridge *b, uint32_t node_id);
static void on_monitors_changed(GDBusConnection *bus, const char *sender, const char *path, const char *iface,
                                const char *signal, GVariant *args, gpointer data);

/* a shell that was killed never says that the session closed */
static void on_shell_vanished(GDBusConnection *bus, const char *name, gpointer data)
{
    struct bridge *b = data;

    say("GNOME Shell is gone");
    b->exit_code = 1;
    g_main_loop_quit(b->loop);
}

static void on_stream_added(GDBusConnection *bus, const char *sender, const char *path, const char *iface,
                            const char *signal, GVariant *args, gpointer data)
{
    struct bridge *b = data;
    guint32 node_id;

    g_variant_get(args, "(u)", &node_id);
    say("screen is PipeWire node %u", node_id);
    connect_stream(b, node_id);
}

/* GNOME Shell may still be starting: wait for its two services */
static GVariant *call_patiently(struct bridge *b, const char *name, const char *path, const char *iface,
                                const char *method, GVariant *args, const char *reply_type, GError **error)
{
    GVariant *reply = NULL;

    if (args)
        g_variant_ref_sink(args);
    for (int attempt = 0; attempt < 120; attempt++) {
        g_clear_error(error);
        reply = g_dbus_connection_call_sync(b->bus, name, path, iface, method, args, G_VARIANT_TYPE(reply_type),
                                            G_DBUS_CALL_FLAGS_NONE, 10000, NULL, error);
        if (reply || !g_error_matches(*error, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN))
            break;
        g_usleep(500 * 1000);
    }
    if (args)
        g_variant_unref(args);
    return reply;
}

static gboolean start_session(struct bridge *b)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) reply = NULL;
    g_autoptr(GVariant) value = NULL;
    GVariantBuilder options;
    const char *session_id;

    reply = call_patiently(b, RD_NAME, RD_PATH, RD_IFACE, "CreateSession", NULL, "(o)", &error);
    if (!reply) {
        say("no remote desktop session: %s", error->message);
        return FALSE;
    }
    g_variant_get(reply, "(o)", &b->rd_session);
    g_clear_pointer(&reply, g_variant_unref);

    reply = g_dbus_connection_call_sync(b->bus, RD_NAME, b->rd_session, "org.freedesktop.DBus.Properties", "Get",
                                        g_variant_new("(ss)", RD_SESSION_IFACE, "SessionId"), G_VARIANT_TYPE("(v)"),
                                        G_DBUS_CALL_FLAGS_NONE, 10000, NULL, &error);
    if (!reply) {
        say("no session id: %s", error->message);
        return FALSE;
    }
    g_variant_get(reply, "(v)", &value);
    session_id = g_variant_get_string(value, NULL);
    g_clear_pointer(&reply, g_variant_unref);

    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&options, "{sv}", "remote-desktop-session-id", g_variant_new_string(session_id));
    reply = call_patiently(b, SC_NAME, SC_PATH, SC_IFACE, "CreateSession", g_variant_new("(a{sv})", &options), "(o)",
                           &error);
    if (!reply) {
        say("no screen cast session: %s", error->message);
        return FALSE;
    }
    g_variant_get(reply, "(o)", &b->sc_session);
    g_clear_pointer(&reply, g_variant_unref);

    /* the monitor may appear a moment after the services */
    for (int attempt = 0; attempt < 40 && !reply; attempt++) {
        g_clear_error(&error);
        g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&options, "{sv}", "cursor-mode", g_variant_new_uint32(CURSOR_EMBEDDED));
        reply = g_dbus_connection_call_sync(b->bus, SC_NAME, b->sc_session, SC_SESSION_IFACE, "RecordMonitor",
                                            g_variant_new("(sa{sv})", b->connector, &options), G_VARIANT_TYPE("(o)"),
                                            G_DBUS_CALL_FLAGS_NONE, 10000, NULL, &error);
        if (!reply)
            g_usleep(250 * 1000);
    }
    if (!reply) {
        say("cannot record the monitor: %s", error->message);
        return FALSE;
    }
    g_variant_get(reply, "(o)", &b->stream_path);
    g_clear_pointer(&reply, g_variant_unref);

    g_dbus_connection_signal_subscribe(b->bus, SC_NAME, SC_STREAM_IFACE, "PipeWireStreamAdded", b->stream_path, NULL,
                                       G_DBUS_SIGNAL_FLAGS_NONE, on_stream_added, b, NULL);
    g_dbus_connection_signal_subscribe(b->bus, RD_NAME, RD_SESSION_IFACE, "Closed", b->rd_session, NULL,
                                       G_DBUS_SIGNAL_FLAGS_NONE, on_session_closed, b, NULL);
    g_bus_watch_name_on_connection(b->bus, RD_NAME, G_BUS_NAME_WATCHER_FLAGS_NONE, NULL, on_shell_vanished, b, NULL);
    g_dbus_connection_signal_subscribe(b->bus, DC_NAME, DC_IFACE, "MonitorsChanged", DC_PATH, NULL,
                                       G_DBUS_SIGNAL_FLAGS_NONE, on_monitors_changed, b, NULL);

    reply = g_dbus_connection_call_sync(b->bus, RD_NAME, b->rd_session, RD_SESSION_IFACE, "Start", NULL, NULL,
                                        G_DBUS_CALL_FLAGS_NONE, 10000, NULL, &error);
    if (!reply) {
        say("the session does not start: %s", error->message);
        return FALSE;
    }
    say("session %s started", b->rd_session);
    return TRUE;
}

static void stop_session(struct bridge *b)
{
    if (b->rd_session && b->bus && !g_dbus_connection_is_closed(b->bus)) {
        g_autoptr(GVariant) reply = g_dbus_connection_call_sync(b->bus, RD_NAME, b->rd_session, RD_SESSION_IFACE, "Stop",
                                                                 NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL);
    }
}

/* ---------- picture: PipeWire to X ---------- */

/* rectangles that overlap or nearly touch are copied as one: a window being dragged changes
 * almost the same place in every frame */
static gboolean near_rects(const struct rect *a, const struct rect *b)
{
    return a->x < b->x + b->w + 16 && b->x < a->x + a->w + 16 && a->y < b->y + b->h + 16 && b->y < a->y + a->h + 16;
}

static struct rect around(struct rect a, struct rect b)
{
    int x2 = MAX(a.x + a.w, b.x + b.w), y2 = MAX(a.y + a.h, b.y + b.h);

    a.x = MIN(a.x, b.x);
    a.y = MIN(a.y, b.y);
    a.w = x2 - a.x;
    a.h = y2 - a.y;
    return a;
}

static void add_rect(struct rect *rects, int *count, int x, int y, int w, int h, int max_w, int max_h)
{
    struct rect r;

    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > max_w)
        w = max_w - x;
    if (y + h > max_h)
        h = max_h - y;
    if (w <= 0 || h <= 0)
        return;
    r = (struct rect) {x, y, w, h};
    for (int i = 0; i < *count;) {
        if (near_rects(&rects[i], &r)) {
            r = around(rects[i], r);
            rects[i] = rects[--*count];
            i = 0;
        } else {
            i++;
        }
    }
    if (*count == MAX_RECTS) {
        /* too many pieces: one rectangle around all of them */
        for (int i = 0; i < MAX_RECTS; i++)
            r = around(rects[i], r);
        *count = 0;
    }
    rects[(*count)++] = r;
}

/* ---------- turning ---------- */

/*
 * The screen of Linux Lab is wide and stays wide. When GNOME turns the monitor (Settings, or the
 * tablet held upright, see llandroid.py), the picture of the stream is the desktop as the user is to
 * see it, tall for a quarter turn, and it has to lie on its side in the wide screen. Mutter numbers
 * the turns the Wayland way: for 1 (90) the picture goes into the screen turned a quarter against the
 * clock, for 3 (270) with the clock, for 2 upside down.
 *
 * "Stream" below is the picture as it arrives, "screen" the X screen and its file.
 */

/* a rectangle of the stream, where it lies on the screen */
static struct rect turn_rect(const struct bridge *b, struct rect r)
{
    int ws = b->format.size.width, hs = b->format.size.height;

    switch (b->turn) {
    case 1:
        return (struct rect) {r.y, ws - r.x - r.w, r.h, r.w};
    case 2:
        return (struct rect) {ws - r.x - r.w, hs - r.y - r.h, r.w, r.h};
    case 3:
        return (struct rect) {hs - r.y - r.h, r.x, r.h, r.w};
    default:
        return r;
    }
}

/* the size of the stream as it lies on the screen */
static void turned_size(const struct bridge *b, int *w, int *h)
{
    gboolean quarter = b->turn == 1 || b->turn == 3;

    *w = quarter ? (int) b->format.size.height : (int) b->format.size.width;
    *h = quarter ? (int) b->format.size.width : (int) b->format.size.height;
}

/* a point of the X screen, in the stream */
static void to_stream(const struct bridge *b, double x, double y, double *sx, double *sy)
{
    double ws, hs, u, v;

    if (!b->have_format) {
        *sx = x;
        *sy = y;
        return;
    }
    ws = b->format.size.width;
    hs = b->format.size.height;
    u = x / b->width;
    v = y / b->height;
    switch (b->turn) {
    case 1:
        *sx = (1 - v) * ws;
        *sy = u * hs;
        break;
    case 2:
        *sx = (1 - u) * ws;
        *sy = (1 - v) * hs;
        break;
    case 3:
        *sx = v * ws;
        *sy = (1 - u) * hs;
        break;
    default:
        *sx = u * ws;
        *sy = v * hs;
        break;
    }
    *sx = CLAMP(*sx, 0, ws - 1);
    *sy = CLAMP(*sy, 0, hs - 1);
}

static void on_turn(GObject *source, GAsyncResult *result, gpointer data)
{
    struct bridge *b = data;
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    g_autoptr(GVariant) monitors = NULL;
    guint32 turn = 0;

    b->turn_asked = FALSE;
    if (!reply) {
        say("Mutter does not say how the monitor is turned: %s", error->message);
        return;
    }
    /* (serial, monitors, logical monitors, properties); a logical monitor is (x, y, scale, transform, ...) */
    monitors = g_variant_get_child_value(reply, 2);
    if (g_variant_n_children(monitors) > 0) {
        g_autoptr(GVariant) first = g_variant_get_child_value(monitors, 0);

        g_variant_get_child(first, 3, "u", &turn);
    }
    turn &= 3; /* mirrored ones are not offered anywhere */
    if (!b->turn_known && turn)
        say("the monitor is turned: %u", turn * 90);
    b->turn = turn;
    b->turn_known = TRUE;
    b->first_frame = TRUE;
}

static void ask_turn(struct bridge *b)
{
    if (b->turn_asked)
        return;
    b->turn_asked = TRUE;
    g_dbus_connection_call(b->bus, DC_NAME, DC_PATH, DC_IFACE, "GetCurrentState", NULL, NULL,
                           G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, NULL, on_turn, b);
}

static void on_monitors_changed(GDBusConnection *bus, const char *sender, const char *path, const char *iface,
                                const char *signal, GVariant *args, gpointer data)
{
    struct bridge *b = data;

    if (b->turn_known) {
        /*
         * A turn, another scale, another size: the shape of the monitor may be a different one, and
         * Mutter then closes the stream without a word to anybody; no frame comes any more. A new
         * stream is the cure, and the plainest way to one is to start over: gnome-session.sh takes
         * this exit code for "again".
         */
        say("the monitors were changed, starting over");
        b->exit_code = EXIT_TURNED;
        g_main_loop_quit(b->loop);
        return;
    }
    ask_turn(b);
}

/* does the stream, turned as Mutter says, cover the screen? Not for a moment while a turn is under way */
static gboolean turn_fits(const struct bridge *b)
{
    int w, h;

    if (!b->turn_known)
        return FALSE;
    turned_size(b, &w, &h);
    return w <= b->width && h <= b->height;
}

/* the picture of a buffer, NULL when it carries none */
static void *frame_data(struct pw_buffer *buffer, int *stride)
{
    struct spa_data *d = &buffer->buffer->datas[0];

    if (!d->data || !d->chunk || d->chunk->size == 0)
        return NULL;
    *stride = d->chunk->stride;
    return SPA_PTROFF(d->data, d->chunk->offset, void);
}

static void put_rects(struct bridge *b, struct pw_buffer *buffer, const struct rect *rects, int count)
{
    int stride = 0;
    void *data = frame_data(buffer, &stride);
    int w = MIN((int) b->format.size.width, b->width);
    int h = MIN((int) b->format.size.height, b->height);
    XImage *image;
    gint64 start;

    if (!data || count == 0)
        return;
    image = XCreateImage(b->dpy, b->visual, b->depth, ZPixmap, 0, data, b->format.size.width, b->format.size.height, 32,
                         stride);
    if (!image)
        return;
    start = g_get_monotonic_time();
    for (int i = 0; i < count; i++) {
        struct rect r = rects[i];

        if (r.x + r.w > w)
            r.w = w - r.x;
        if (r.y + r.h > h)
            r.h = h - r.y;
        if (r.w <= 0 || r.h <= 0)
            continue;
        XPutImage(b->dpy, b->win, b->gc, image, r.x, r.y, r.x, r.y, r.w, r.h);
        b->stat_pixels += (gint64) r.w * r.h;
    }
    XFlush(b->dpy);
    b->stat_put_us += g_get_monotonic_time() - start;
    image->data = NULL; /* the pixels belong to PipeWire */
    XDestroyImage(image);
}

/* ---------- picture: straight into the screen file ---------- */

/*
 * Linux Lab's app copies the screen file of Xvfb into a texture for every frame it draws, with no
 * lock of any kind, and the X server writes into the same memory whenever it likes. A frame that is
 * still being written when the copy starts shows half old and half new: a moving window tears.
 *
 * The app opens the file by name before every copy and maps it again when the inode is a different one.
 * So the bridge keeps three buffers of its own, brings one that is not on screen up to date, and swaps
 * it in under the name with renameat2(RENAME_EXCHANGE): the app always gets one whole frame. The
 * buffer written next is the one that left the screen two swaps earlier, which gives a copy that is
 * still running time to finish. Xvfb's own file waits under another name and is put back at exit.
 *
 * The app copies only when Honor's Xvfb tells it that something was drawn (message 0x3005 in the app).
 * So every swap ends with one pixel drawn through X: Xvfb reports it, and the app takes the new frame.
 */
static char *sibling(const char *name, const char *suffix)
{
    return g_strdup_printf("%s.llbridge-%s", name, suffix);
}

static int exchange(struct fb *fb, const char *a, const char *c)
{
    return renameat2(fb->dir, a, fb->dir, c, RENAME_EXCHANGE);
}

/*
 * The inode of the file the X server keeps its screen in, from the memory map of the server itself;
 * 0 when it cannot be told. @connection is the socket to the server, @wanted the end of the path of
 * the file without what the bridge may have added to its name.
 */
static ino_t server_screen(int connection, const char *wanted)
{
    struct ucred server;
    socklen_t length = sizeof server;
    char path[64], line[1024];
    ino_t found = 0;
    FILE *maps;

    if (getsockopt(connection, SOL_SOCKET, SO_PEERCRED, &server, &length) < 0 || server.pid <= 0)
        return 0;
    snprintf(path, sizeof path, "/proc/%d/maps", (int) server.pid);
    maps = fopen(path, "r");
    if (!maps)
        return 0;
    while (fgets(line, sizeof line, maps)) {
        unsigned long long inode;
        char access[8];
        int rest = 0;

        /* 72e47b6000-72e5db0000 rw-s 00000000 fe:24 402935    /home/XvfbScreen/Xvfb_screen0 */
        if (sscanf(line, "%*s %7s %*s %*s %llu %n", access, &inode, &rest) >= 2 && access[3] == 's' && rest &&
            strstr(line + rest, wanted)) {
            found = inode;
            break;
        }
    }
    fclose(maps);
    return found;
}

/*
 * A bridge that was killed could not put the file of the X server back under its name. Which of the
 * files lying here is that file is not a matter of names: a bridge killed together with Linux Lab
 * leaves the file of an X server that is gone, and the next X server draws into what it found under
 * the name. @live is the inode the running server draws into (0: not known, then the names are believed).
 */
static void fb_recover(struct fb *fb, ino_t live)
{
    const char *others[] = {fb->saved, fb->spare[0], fb->spare[1]};
    struct stat st;
    gboolean named = live && fstatat(fb->dir, fb->name, &st, 0) == 0 && st.st_ino == live;

    for (int i = 0; i < 3 && !named; i++) {
        if (fstatat(fb->dir, others[i], &st, 0) < 0 || (live ? st.st_ino != live : i != 0))
            continue;
        if (exchange(fb, others[i], fb->name) == 0 || renameat(fb->dir, others[i], fb->dir, fb->name) == 0) {
            say("put back the screen file of the X server, left aside by an earlier run");
            named = TRUE;
        }
    }
    if (live && !named)
        say("the screen file of the X server has no name any more: Honor's desktop shows nothing until "
            "Linux Lab is started again");
    for (int i = 0; i < 3; i++)
        unlinkat(fb->dir, others[i], 0);
}

static void fb_close(struct fb *fb)
{
    if (!fb)
        return;
    if (exchange(fb, fb->saved, fb->name) == 0) {
        unlinkat(fb->dir, fb->saved, 0);
        unlinkat(fb->dir, fb->spare[0], 0);
        unlinkat(fb->dir, fb->spare[1], 0);
    }
    for (int i = 0; i < FB_BUFFERS; i++)
        if (fb->map[i])
            munmap(fb->map[i], fb->size);
    close(fb->dir);
    g_free(fb->name);
    g_free(fb->saved);
    g_free(fb->spare[0]);
    g_free(fb->spare[1]);
    g_free(fb);
}

static uint8_t *fb_create(struct fb *fb, const char *name, const uint8_t *initial)
{
    int fd = openat(fb->dir, name, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    uint8_t *map;

    if (fd < 0)
        return NULL;
    /* the app opens the file from Android, as the same user but without the group of the X server */
    fchmod(fd, 0666);
    if (ftruncate(fd, fb->size) < 0) {
        close(fd);
        return NULL;
    }
    map = mmap(NULL, fb->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED)
        return NULL;
    memcpy(map, initial, fb->size);
    return map;
}

static struct fb *fb_open(const char *path, int width, int height, int connection)
{
    struct fb *fb = g_new0(struct fb, 1);
    g_autofree char *dir = g_path_get_dirname(path);
    const char *names[FB_BUFFERS];
    uint8_t *original;
    struct stat st;
    int fd;

    fb->name = g_path_get_basename(path);
    fb->saved = sibling(fb->name, "xvfb");
    fb->spare[0] = sibling(fb->name, "a");
    fb->spare[1] = sibling(fb->name, "b");
    fb->dir = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    fb->width = width;
    fb->height = height;
    fb->stride = width * 4;
    if (fb->dir < 0) {
        say("no directory %s: %s", dir, g_strerror(errno));
        goto fail;
    }
    {
        g_autofree char *parent = g_path_get_basename(dir);
        g_autofree char *wanted = g_strdup_printf("/%s/%s", parent, fb->name);

        fb_recover(fb, server_screen(connection, wanted));
    }

    fd = openat(fb->dir, fb->name, O_RDONLY | O_CLOEXEC);
    if (fd < 0 || fstat(fd, &st) < 0 || (size_t) st.st_size < (size_t) fb->stride * height) {
        say("%s is not a screen of %dx%d", path, width, height);
        if (fd >= 0)
            close(fd);
        goto fail;
    }
    fb->size = st.st_size;
    original = mmap(NULL, fb->size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (original == MAP_FAILED) {
        say("cannot map %s: %s", path, g_strerror(errno));
        goto fail;
    }

    names[0] = fb->saved;
    names[1] = fb->spare[0];
    names[2] = fb->spare[1];
    for (int i = 0; i < FB_BUFFERS; i++) {
        fb->map[i] = fb_create(fb, names[i], original);
        if (!fb->map[i]) {
            say("cannot make a screen buffer in %s: %s", dir, g_strerror(errno));
            munmap(original, fb->size);
            goto fail;
        }
    }
    munmap(original, fb->size);

    /* buffer 0 goes on screen, Xvfb's file takes its name */
    if (exchange(fb, fb->saved, fb->name) < 0) {
        say("cannot swap screen files (%s), drawing through X", g_strerror(errno));
        goto fail;
    }
    fb->front = 0;
    fb->at_spare[0] = 1;
    fb->at_spare[1] = 2;
    fb->waiting = 0;
    say("drawing into %s", path);
    return fb;

fail:
    if (fb->dir >= 0) {
        unlinkat(fb->dir, fb->saved, 0);
        unlinkat(fb->dir, fb->spare[0], 0);
        unlinkat(fb->dir, fb->spare[1], 0);
    }
    for (int i = 0; i < FB_BUFFERS; i++)
        if (fb->map[i])
            munmap(fb->map[i], fb->size);
    fb->map[0] = fb->map[1] = fb->map[2] = NULL;
    if (fb->dir >= 0)
        close(fb->dir);
    g_free(fb->name);
    g_free(fb->saved);
    g_free(fb->spare[0]);
    g_free(fb->spare[1]);
    g_free(fb);
    return NULL;
}

/* Copies a rectangle of the screen out of the stream, which is @ws by @hs and turned by @turn. */
static gint64 fb_copy_rect(struct fb *fb, uint8_t *dst, const uint8_t *src, int src_stride, struct rect r,
                           int max_w, int max_h, int turn, int ws, int hs)
{
    /* a quarter turn reads the stream down its columns: in tiles, so that both sides stay in the cache */
    enum { TILE = 32 };

    if (r.x + r.w > max_w)
        r.w = max_w - r.x;
    if (r.y + r.h > max_h)
        r.h = max_h - r.y;
    if (r.w <= 0 || r.h <= 0)
        return 0;
    if (turn == 0) {
        for (int y = r.y; y < r.y + r.h; y++)
            memcpy(dst + (size_t) y * fb->stride + (size_t) r.x * 4, src + (size_t) y * src_stride + (size_t) r.x * 4,
                   (size_t) r.w * 4);
        return (gint64) r.w * r.h;
    }
    if (turn == 2) {
        for (int y = r.y; y < r.y + r.h; y++) {
            uint32_t *to = (uint32_t *) (dst + (size_t) y * fb->stride) + r.x;
            const uint32_t *from = (const uint32_t *) (src + (size_t) (hs - 1 - y) * src_stride) + (ws - 1 - r.x);

            for (int x = 0; x < r.w; x++)
                *to++ = *from--;
        }
        return (gint64) r.w * r.h;
    }
    for (int ty = r.y; ty < r.y + r.h; ty += TILE) {
        for (int tx = r.x; tx < r.x + r.w; tx += TILE) {
            int y_end = MIN(ty + TILE, r.y + r.h), x_end = MIN(tx + TILE, r.x + r.w);

            for (int y = ty; y < y_end; y++) {
                uint32_t *to = (uint32_t *) (dst + (size_t) y * fb->stride) + tx;
                /* turn 1: stream (ws - 1 - y, x); turn 3: stream (y, hs - 1 - x) */
                const uint8_t *from = turn == 1 ? src + (size_t) tx * src_stride + (size_t) (ws - 1 - y) * 4
                                                : src + (size_t) (hs - 1 - tx) * src_stride + (size_t) y * 4;
                ptrdiff_t step = turn == 1 ? src_stride : -src_stride;

                for (int x = tx; x < x_end; x++, from += step)
                    *to++ = *(const uint32_t *) from;
            }
        }
    }
    return (gint64) r.w * r.h;
}

/* one pixel through X, the same as on screen, so that Xvfb reports a change */
static void tell_x(struct bridge *b, const uint8_t *pixel_on_screen)
{
    static uint32_t pixel;
    static XImage *image;

    if (!image)
        image = XCreateImage(b->dpy, b->visual, b->depth, ZPixmap, 0, (char *) &pixel, 1, 1, 32, 4);
    if (!image)
        return;
    memcpy(&pixel, pixel_on_screen, 4);
    XPutImage(b->dpy, b->win, b->gc, image, 0, 0, 0, 0, 1, 1);
    XFlush(b->dpy);
}

/* ---------- pacing ---------- */

/*
 * A video arrives at 60 frames a second, but not evenly: the player draws a frame in 5 to 10 ms
 * after its tick, and the shell paints it the moment it comes. The intervals land anywhere between
 * 9 and 28 ms, and the screen, which refreshes at 120 Hz, holds the frames for one, two or three
 * refreshes in turn: the video judders, although every frame is shown. Honor's desktop looks even
 * because its X server collects such frames into a steady 60 a second.
 *
 * So frames that keep a rhythm are shown on the beat of that rhythm: a frame is written into a
 * spare screen file as soon as it comes, and swapped on screen three quarters of a period after
 * the moment it was due: late by up to that much it is still on time, and a frame that comes early
 * waits in the second spare for its own beat. One later than that is shown at once. The rhythm is
 * read from the last sixteen intervals: their median is the period, and the frames keep the rhythm
 * when three quarters of the intervals are within half a period of it. The moment the next frame is
 * due moves a period on with every frame and a little towards the moment the frame really came.
 * Frames that keep no rhythm (the first of a video, a drag, a shell animation at 100 frames a
 * second, and everything faster than 80 or slower than 20 frames a second) are shown at once.
 * A frame that comes while two still wait takes the place and the moment of the later one.
 */
#define PACE_MIN_PERIOD 12500 /* µs: 80 frames a second; faster than that is not video */
#define PACE_MAX_PERIOD 50000 /* 20 frames a second */
static const double PACE_RATES[] = {60, 50, 30, 25, 24};

static void fb_show(struct bridge *b);
static void pace_arm(struct bridge *b);

static gboolean on_pace_timer(gpointer data)
{
    struct bridge *b = data;
    gint64 late = g_get_monotonic_time() - b->fb->due[0];

    if (late > b->stat_timer_late)
        b->stat_timer_late = late;
    b->pace.timer = NULL;
    fb_show(b);
    pace_arm(b);
    return G_SOURCE_REMOVE;
}

/* the timer for the oldest waiting frame; a timeout source is good for one firing only: once it has
 * fired, GLib would make it ready again at once */
static void pace_arm(struct bridge *b)
{
    struct pace *p = &b->pace;
    struct fb *fb = b->fb;

    if (fb->waiting == 0) {
        if (p->timer)
            g_source_destroy(p->timer);
        p->timer = NULL;
        return;
    }
    if (!p->timer) {
        p->timer = g_timeout_source_new(0);
        g_source_set_priority(p->timer, G_PRIORITY_HIGH);
        g_source_set_callback(p->timer, on_pace_timer, b, NULL);
        g_source_set_ready_time(p->timer, fb->due[0]);
        g_source_attach(p->timer, NULL);
        g_source_unref(p->timer);
    } else {
        g_source_set_ready_time(p->timer, fb->due[0]);
    }
}

/* everything waiting goes on screen now */
static void pace_flush(struct bridge *b)
{
    while (b->fb->waiting)
        fb_show(b);
    pace_arm(b);
}

static int compare_times(const void *a, const void *c)
{
    gint64 x = *(const gint64 *) a, y = *(const gint64 *) c;

    return x < y ? -1 : x > y;
}

/* the period of the frames, or 0 when they keep none */
static gint64 pace_rhythm(struct pace *p)
{
    gint64 sorted[PACE_WINDOW];
    gint64 period;
    int fit = 0;

    if (p->seen < PACE_WINDOW)
        return 0;
    memcpy(sorted, p->intervals, sizeof sorted);
    qsort(sorted, PACE_WINDOW, sizeof sorted[0], compare_times);
    period = (sorted[PACE_WINDOW / 2 - 1] + sorted[PACE_WINDOW / 2]) / 2;
    if (period < PACE_MIN_PERIOD || period > PACE_MAX_PERIOD)
        return 0;
    for (int i = 0; i < PACE_WINDOW; i++)
        fit += llabs(p->intervals[i] - period) <= period / 2;
    if (fit < PACE_WINDOW * 3 / 4)
        return 0;
    /* the rates videos come in; the beat is the exact one, so that Android sees a steady 60 frames
     * a second and takes the screen to 60 Hz, as it does for Honor's desktop (at 120 Hz the app
     * cannot always get a frame onto the screen in time for the next refresh) */
    for (int i = 0; i < G_N_ELEMENTS(PACE_RATES); i++) {
        gint64 exact = G_USEC_PER_SEC / PACE_RATES[i];

        if (llabs(period - exact) * 12 <= exact)
            return exact;
    }
    return period;
}

static void pace_frame(struct bridge *b, gint64 came)
{
    struct pace *p = &b->pace;
    struct fb *fb = b->fb;
    gint64 interval = came - p->last;
    gint64 period, due;

    p->last = came;
    if (b->no_pacing) {
        pace_flush(b);
        return;
    }
    p->intervals[p->seen++ % PACE_WINDOW] = interval;
    period = pace_rhythm(p);
    if (!period) {
        p->period = 0;
        pace_flush(b);
        return;
    }
    if (!p->period) {
        p->expected = came; /* the rhythm starts: the beat starts from this frame */
    } else {
        p->expected += period;
        p->expected += (came - p->expected) / 8;
    }
    p->period = period;
    due = p->expected + period * 3 / 4;
    if (due <= came) {
        b->stat_late++;
        p->expected = came; /* the beat has lost the frames: it starts anew from this one */
        pace_flush(b);
        return;
    }
    if (due > came + period)
        due = came + period;
    b->stat_paced++;
    fb->due[fb->waiting - 1] = due;
    pace_arm(b);
}

static void fb_present(struct bridge *b, struct pw_buffer *buffer, const struct rect *rects, int count, gboolean all)
{
    struct fb *fb = b->fb;
    int stride = 0;
    const uint8_t *src = frame_data(buffer, &stride);
    int w, h; /* of the stream as it lies on the screen */
    int slot = fb->waiting == 2 ? fb->pending[1] : fb->waiting == 1 ? !fb->pending[0] : fb->next_spare;
    int back = fb->at_spare[slot];
    struct rect todo[MAX_RECTS];
    int todo_count = 0;
    gboolean whole;
    struct damage *now;
    gint64 start;

    if (!src)
        return;
    turned_size(b, &w, &h);
    w = MIN(w, fb->width);
    h = MIN(h, fb->height);
    start = g_get_monotonic_time();
    fb->frame++;
    now = &fb->history[fb->frame % FB_HISTORY];
    now->frame = fb->frame;
    now->all = all;
    now->count = all ? 0 : count;
    if (!all)
        memcpy(now->rects, rects, sizeof(struct rect) * count);

    /* the buffer shows an older frame: everything that changed since then, each place once */
    whole = fb->holds[back] == 0 || fb->frame - fb->holds[back] > FB_HISTORY;
    for (guint64 f = fb->holds[back] + 1; f <= fb->frame && !whole; f++) {
        struct damage *d = &fb->history[f % FB_HISTORY];

        whole = d->all;
        for (int i = 0; i < d->count && !whole; i++)
            add_rect(todo, &todo_count, d->rects[i].x, d->rects[i].y, d->rects[i].w, d->rects[i].h, w, h);
    }
    if (whole) {
        todo[0] = (struct rect) {0, 0, w, h};
        todo_count = 1;
    }
    for (int i = 0; i < todo_count; i++)
        b->stat_pixels += fb_copy_rect(fb, fb->map[back], src, stride, todo[i], w, h, b->turn,
                                       b->format.size.width, b->format.size.height);
    fb->holds[back] = fb->frame;
    if (fb->waiting < 2)
        fb->pending[fb->waiting++] = slot;
    else
        b->stat_replaced++;
    b->stat_put_us += g_get_monotonic_time() - start;
    pace_frame(b, start);
}

/* the oldest prepared frame goes on screen */
static void fb_show(struct bridge *b)
{
    struct fb *fb = b->fb;
    int slot, back;

    if (fb->waiting == 0)
        return;
    slot = fb->pending[0];
    back = fb->at_spare[slot];
    fb->pending[0] = fb->pending[1];
    fb->due[0] = fb->due[1];
    fb->waiting--;
    if (exchange(fb, fb->spare[slot], fb->name) < 0) {
        say("cannot swap screen files: %s", g_strerror(errno));
        return;
    }
    fb->at_spare[slot] = fb->front;
    fb->front = back;
    fb->next_spare = !slot;
    tell_x(b, fb->map[back]);
}

/* ---------- what changed: the lists of llcapture.so ---------- */

static gboolean capture_open(struct bridge *b)
{
    g_autofree char *path = g_build_filename(g_get_user_runtime_dir(), LLCAPTURE_FILE, NULL);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    void *map;

    if (fd < 0)
        return FALSE;
    if (fstat(fd, &st) < 0 || (size_t) st.st_size < sizeof(struct llcapture_shared)) {
        close(fd);
        return FALSE;
    }
    map = mmap(NULL, sizeof(struct llcapture_shared), PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (map == MAP_FAILED)
        return FALSE;
    b->capture = map;
    say("taking the lists of changes from %s", path);
    return TRUE;
}

/*
 * What changed between the last frame that reached the screen and the frame this buffer holds.
 * FALSE when llcapture.so does not know the buffer: the list Mutter attached is used then.
 */
static gboolean capture_damage(struct bridge *b, const struct spa_meta_header *header, struct rect *rects,
                               int *count, gboolean *all)
{
    const struct llcapture_shared *c;
    gint64 stamped, best = G_MAXINT64;
    guint64 records, frame = 0;

    if (!header || header->pts <= 0 || (!b->capture && !capture_open(b)))
        return FALSE;
    c = b->capture;
    if (c->magic != LLCAPTURE_MAGIC)
        return FALSE;
    /* the record of this buffer is the first one made after Mutter stamped it */
    stamped = header->pts / 1000;
    records = __atomic_load_n(&c->records, __ATOMIC_ACQUIRE);
    for (guint64 i = 0; i < LLCAPTURE_RECORDS && i < records; i++) {
        const struct llcapture_record *record = &c->record[(records - 1 - i) % LLCAPTURE_RECORDS];

        if (record->time_us >= stamped && record->time_us - stamped < 200000 && record->time_us < best) {
            best = record->time_us;
            frame = record->frame;
        }
    }
    if (best == G_MAXINT64)
        return FALSE;
    if (b->capture_frame == 0 || frame < b->capture_frame || frame - b->capture_frame >= LLCAPTURE_FRAMES)
        *all = TRUE;
    for (guint64 f = b->capture_frame + 1; f <= frame && !*all; f++) {
        const struct llcapture_frame *changed = &c->frames[f % LLCAPTURE_FRAMES];

        if (changed->count < 0 || changed->count > LLCAPTURE_RECTS)
            *all = TRUE;
        for (int i = 0; i < changed->count && !*all; i++)
            add_rect(rects, count, changed->rects[i].x, changed->rects[i].y, changed->rects[i].w,
                     changed->rects[i].h, b->format.size.width, b->format.size.height);
    }
    b->capture_frame = frame;
    return TRUE;
}

static void on_process(void *data)
{
    struct bridge *b = data;
    struct pw_buffer *newest = NULL, *buffer;
    struct rect rects[MAX_RECTS];
    int count = 0;
    gboolean all = b->first_frame;
    int stride;

    /* Frames may arrive faster than X takes them. Only the newest one is drawn,
     * with the damage of the skipped ones added to its own. */
    while ((buffer = pw_stream_dequeue_buffer(b->stream))) {
        struct spa_meta_header *header = spa_buffer_find_meta_data(buffer->buffer, SPA_META_Header, sizeof(*header));
        struct spa_meta *damage = spa_buffer_find_meta(buffer->buffer, SPA_META_VideoDamage);

        if ((header && (header->flags & SPA_META_HEADER_FLAG_CORRUPTED)) || !frame_data(buffer, &stride)) {
            pw_stream_queue_buffer(b->stream, buffer);
            continue;
        }
        if (capture_damage(b, header, rects, &count, &all)) {
            /* the exact list */
        } else if (damage) {
            struct spa_meta_region *region;

            spa_meta_for_each(region, damage) {
                if (!spa_meta_region_is_valid(region))
                    break;
                add_rect(rects, &count, region->region.position.x, region->region.position.y,
                         region->region.size.width, region->region.size.height, b->format.size.width,
                         b->format.size.height);
            }
        } else {
            all = TRUE;
        }
        if (newest)
            pw_stream_queue_buffer(b->stream, newest);
        newest = buffer;
    }
    if (!newest || !b->have_format)
        return;
    if (!turn_fits(b) || (b->turn && !b->fb)) {
        /* a turn is under way and Mutter has not said which; or a turned picture and no screen file,
         * which is not catered for. This frame is not shown, the next one that is shown is whole. */
        static gint64 said;
        gint64 now = g_get_monotonic_time();

        if (now - said > G_USEC_PER_SEC) {
            struct spa_data *d = &newest->buffer->datas[0];

            said = now;
            say("a frame of %ux%u (stride %d, %u bytes of %u) does not go with a turn of %d%s",
                b->format.size.width, b->format.size.height, d->chunk->stride, d->chunk->size, d->maxsize,
                b->turn * 90, b->turn_known ? "" : ", not confirmed yet");
        }
        if (!b->turn_known)
            ask_turn(b);
        b->first_frame = TRUE;
        pw_stream_queue_buffer(b->stream, newest);
        return;
    }
    for (int i = 0; i < count; i++)
        rects[i] = turn_rect(b, rects[i]);
    if (b->fb) {
        fb_present(b, newest, rects, count, all);
    } else {
        if (all) {
            rects[0] = (struct rect) {0, 0, b->format.size.width, b->format.size.height};
            count = 1;
        }
        put_rects(b, newest, rects, count);
    }
    b->first_frame = FALSE;
    if (b->held)
        pw_stream_queue_buffer(b->stream, b->held);
    b->held = newest;

    if (b->verbose) {
        gint64 now = g_get_monotonic_time();

        b->stat_frames++;
        if (now - b->stat_since >= G_USEC_PER_SEC) {
            say("%.1f frames/s, %.1f ms to the screen per frame, %.1f Mpx per frame; on the beat %d, late %d, replaced %d, beat late up to %.1f ms",
                b->stat_frames * 1e6 / (now - b->stat_since), b->stat_put_us / 1e3 / b->stat_frames,
                b->stat_pixels / 1e6 / b->stat_frames, b->stat_paced, b->stat_late, b->stat_replaced,
                b->stat_timer_late / 1e3);
            b->stat_since = now;
            b->stat_frames = 0;
            b->stat_paced = 0;
            b->stat_late = 0;
            b->stat_replaced = 0;
            b->stat_timer_late = 0;
            b->stat_put_us = 0;
            b->stat_pixels = 0;
        }
    }
}

static void on_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
    struct bridge *b = data;
    uint8_t space[1024];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(space, sizeof(space));
    const struct spa_pod *params[3];
    uint32_t media_type, media_subtype;

    if (!param || id != SPA_PARAM_Format)
        return;
    if (spa_format_parse(param, &media_type, &media_subtype) < 0 || media_type != SPA_MEDIA_TYPE_video ||
        media_subtype != SPA_MEDIA_SUBTYPE_raw || spa_format_video_raw_parse(param, &b->format) < 0) {
        fail(b, "the stream has a format this program does not know");
        return;
    }
    b->have_format = TRUE;
    b->first_frame = TRUE;
    ask_turn(b);
    say("screen %ux%u, window %dx%d", b->format.size.width, b->format.size.height, b->width, b->height);

    /* few buffers: each comes round again soon, and llcapture.so then has little to bring up to date in it */
    params[0] = spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
                                           SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 3, 8),
                                           SPA_PARAM_BUFFERS_dataType,
                                           SPA_POD_Int((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr)));
    params[1] = spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type,
                                           SPA_POD_Id(SPA_META_Header), SPA_PARAM_META_size,
                                           SPA_POD_Int(sizeof(struct spa_meta_header)));
    params[2] = spa_pod_builder_add_object(
        &builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoDamage),
        SPA_PARAM_META_size,
        SPA_POD_CHOICE_RANGE_Int(sizeof(struct spa_meta_region) * 16, sizeof(struct spa_meta_region) * 1,
                                 sizeof(struct spa_meta_region) * 16));
    pw_stream_update_params(b->stream, params, 3);
}

static void on_state_changed(void *data, enum pw_stream_state old, enum pw_stream_state state, const char *error)
{
    struct bridge *b = data;

    if (b->verbose)
        say("stream: %s", pw_stream_state_as_string(state));
    if (state == PW_STREAM_STATE_ERROR)
        fail(b, error ? error : "stream error");
    else if (state == PW_STREAM_STATE_UNCONNECTED && old >= PW_STREAM_STATE_PAUSED)
        fail(b, "the stream ended");
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_state_changed,
    .param_changed = on_param_changed,
    .process = on_process,
};

static void connect_stream(struct bridge *b, uint32_t node_id)
{
    uint8_t space[1024];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(space, sizeof(space));
    const struct spa_pod *params[1];

    if (b->stream)
        return;
    b->stream = pw_stream_new(b->pw_core, "llbridge",
                              pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture",
                                                PW_KEY_MEDIA_ROLE, "Screen", NULL));
    pw_stream_add_listener(b->stream, &b->stream_listener, &stream_events, b);
    /* 32 bits per pixel, blue first: what a 24 bit deep X image is on a little-endian machine */
    params[0] = spa_pod_builder_add_object(
        &builder, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), SPA_FORMAT_VIDEO_format,
        SPA_POD_CHOICE_ENUM_Id(3, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA),
        SPA_FORMAT_VIDEO_size,
        SPA_POD_CHOICE_RANGE_Rectangle(&SPA_RECTANGLE(b->width, b->height), &SPA_RECTANGLE(1, 1),
                                       &SPA_RECTANGLE(16384, 16384)),
        SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&SPA_FRACTION(0, 1)), SPA_FORMAT_VIDEO_maxFramerate,
        SPA_POD_CHOICE_RANGE_Fraction(&SPA_FRACTION(240, 1), &SPA_FRACTION(1, 1), &SPA_FRACTION(240, 1)));
    if (pw_stream_connect(b->stream, PW_DIRECTION_INPUT, node_id, PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
                          params, 1) < 0)
        fail(b, "cannot connect to the PipeWire stream");
}

static void on_core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
    struct bridge *b = data;

    say("PipeWire: %s", message);
    if (id == PW_ID_CORE && res == -EPIPE)
        fail(b, "PipeWire is gone");
}

static const struct pw_core_events core_events = {
    PW_VERSION_CORE_EVENTS,
    .error = on_core_error,
};

struct pw_source {
    GSource base;
    struct pw_loop *loop;
};

static gboolean pw_source_dispatch(GSource *source, GSourceFunc callback, gpointer data)
{
    pw_loop_iterate(((struct pw_source *) source)->loop, 0);
    return G_SOURCE_CONTINUE;
}

static GSourceFuncs pw_source_funcs = {
    .dispatch = pw_source_dispatch,
};

static gboolean start_pipewire(struct bridge *b)
{
    GSource *source;

    pw_init(NULL, NULL);
    b->pw_loop = pw_loop_new(NULL);
    if (!b->pw_loop)
        return FALSE;
    source = g_source_new(&pw_source_funcs, sizeof(struct pw_source));
    ((struct pw_source *) source)->loop = b->pw_loop;
    g_source_add_unix_fd(source, pw_loop_get_fd(b->pw_loop), G_IO_IN | G_IO_ERR);
    pw_loop_enter(b->pw_loop);
    g_source_attach(source, NULL);

    b->pw_context = pw_context_new(b->pw_loop, NULL, 0);
    if (!b->pw_context)
        return FALSE;
    /* PipeWire may come up a moment after the desktop session */
    for (int attempt = 0; attempt < 40 && !b->pw_core; attempt++) {
        b->pw_core = pw_context_connect(b->pw_context, NULL, 0);
        if (!b->pw_core)
            g_usleep(250 * 1000);
    }
    if (!b->pw_core)
        return FALSE;
    pw_core_add_listener(b->pw_core, &b->core_listener, &core_events, b);
    return TRUE;
}

/* ---------- input: X to GNOME ---------- */

static int slot_of(struct bridge *b, int touch_id, gboolean take)
{
    int free_slot = -1;

    for (int i = 0; i < MAX_SLOTS; i++) {
        if (b->touch_ids[i] == touch_id)
            return i;
        if (b->touch_ids[i] < 0 && free_slot < 0)
            free_slot = i;
    }
    if (take && free_slot >= 0)
        b->touch_ids[free_slot] = touch_id;
    return take ? free_slot : -1;
}

/* a touch or a pointer position of the X screen as the variant GNOME takes: stream, [slot,] x, y */
static GVariant *position(struct bridge *b, int slot, double x, double y)
{
    double sx, sy;

    to_stream(b, x, y, &sx, &sy);
    return slot < 0 ? g_variant_new("(sdd)", b->stream_path, sx, sy)
                    : g_variant_new("(sudd)", b->stream_path, (guint32) slot, sx, sy);
}

static void release_everything(struct bridge *b)
{
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (b->touch_ids[i] >= 0) {
            notify(b, "NotifyTouchUp", g_variant_new("(u)", (guint32) i));
            b->touch_ids[i] = -1;
        }
    }
    for (int code = 8; code < 256; code++) {
        if (b->keys[code]) {
            notify(b, "NotifyKeyboardKeycode", g_variant_new("(ub)", (guint32) (code - 8), FALSE));
            b->keys[code] = FALSE;
        }
    }
}

static void pointer_button(struct bridge *b, int button, gboolean down)
{
    static const int codes[] = {0, BTN_LEFT, BTN_MIDDLE, BTN_RIGHT, 0, 0, 0, 0, BTN_SIDE, BTN_EXTRA};

    if (button >= 4 && button <= 7) {
        /* the wheel is buttons in X: 4 up, 5 down, 6 left, 7 right */
        if (down)
            notify(b, "NotifyPointerAxisDiscrete",
                   g_variant_new("(ui)", (guint32) (button >= 6), (gint32) (button == 4 || button == 6 ? -1 : 1)));
        return;
    }
    if (button < 1 || button >= (int) G_N_ELEMENTS(codes) || !codes[button] || b->buttons[button] == down)
        return;
    b->buttons[button] = down;
    notify(b, "NotifyPointerButton", g_variant_new("(ib)", (gint32) codes[button], down));
}

static void handle_xi(struct bridge *b, int type, XIDeviceEvent *event)
{
    int slot;

    switch (type) {
    case XI_TouchBegin:
        slot = slot_of(b, event->detail, TRUE);
        if (slot >= 0)
            notify(b, "NotifyTouchDown", position(b, slot, event->event_x, event->event_y));
        break;
    case XI_TouchUpdate:
        slot = slot_of(b, event->detail, FALSE);
        if (slot >= 0)
            notify(b, "NotifyTouchMotion", position(b, slot, event->event_x, event->event_y));
        break;
    case XI_TouchEnd:
        slot = slot_of(b, event->detail, FALSE);
        if (slot >= 0) {
            notify(b, "NotifyTouchUp", g_variant_new("(u)", (guint32) slot));
            b->touch_ids[slot] = -1;
        }
        break;
    case XI_Motion:
        if (event->flags & XIPointerEmulated)
            break; /* a touch seen as a pointer: the touch itself is already sent */
        notify(b, "NotifyPointerMotionAbsolute", position(b, -1, event->event_x, event->event_y));
        break;
    case XI_ButtonPress:
    case XI_ButtonRelease:
        if ((event->flags & XIPointerEmulated) && event->detail <= 3)
            break;
        if (type == XI_ButtonPress)
            notify(b, "NotifyPointerMotionAbsolute", position(b, -1, event->event_x, event->event_y));
        pointer_button(b, event->detail, type == XI_ButtonPress);
        break;
    }
}

static void handle_key(struct bridge *b, XKeyEvent *event, gboolean down)
{
    unsigned int code = event->keycode;

    /* held keys repeat in X as presses without releases; GNOME repeats on its own */
    if (code < 8 || code > 255 || b->keys[code] == down)
        return;
    b->keys[code] = down;
    notify(b, "NotifyKeyboardKeycode", g_variant_new("(ub)", (guint32) (code - 8), down));
}

/* ---------- clipboard ---------- */

/*
 * Honor's X server watches the CLIPBOARD selection of its display: when a program takes it, the server
 * asks that program for the text and hands it to Android, and when Android has something new, the
 * server takes the selection itself. GNOME's clipboard lives in the shell, which gives it to a
 * remote desktop session. The bridge is both: owner of the selection for GNOME's text, and a
 * remote desktop that "has" the text of the X side. Text only.
 */

static int text_type_of(struct bridge *b, Atom atom)
{
    for (int i = 0; i < (int) TEXT_TYPES; i++)
        if (b->atom_text[i] == atom)
            return i;
    return -1;
}

/* a program of the X side (Honor's server, for Android) asks for what the bridge offers */
static void clip_answer_x(struct bridge *b, XSelectionRequestEvent *request)
{
    XSelectionEvent answer = {0};
    Atom property = request->property != None ? request->property : request->target;
    long limit = XExtendedMaxRequestSize(b->dpy) ? XExtendedMaxRequestSize(b->dpy) : XMaxRequestSize(b->dpy);

    answer.type = SelectionNotify;
    answer.display = request->display;
    answer.requestor = request->requestor;
    answer.selection = request->selection;
    answer.target = request->target;
    answer.time = request->time;
    answer.property = None; /* "no" until proven otherwise */
    if (request->selection == b->atom_clipboard && b->clip && !b->clip_from_x) {
        gsize size;
        const unsigned char *text = g_bytes_get_data(b->clip, &size);

        if (request->target == b->atom_targets) {
            Atom targets[1 + TEXT_TYPES];

            targets[0] = b->atom_targets;
            for (int i = 0; i < (int) TEXT_TYPES; i++)
                targets[1 + i] = b->atom_text[i];
            XChangeProperty(b->dpy, request->requestor, property, XA_ATOM, 32, PropModeReplace,
                            (unsigned char *) targets, 1 + TEXT_TYPES);
            answer.property = property;
        } else if (text_type_of(b, request->target) >= 0 && (long) size < limit * 4 - 64) {
            /* "TEXT" means "in whatever encoding you have" */
            Atom type = request->target == b->atom_text[4] ? b->atom_text[1] : request->target;

            XChangeProperty(b->dpy, request->requestor, property, type, 8, PropModeReplace, text, (int) size);
            answer.property = property;
        }
    }
    XSendEvent(b->dpy, request->requestor, False, 0, (XEvent *) &answer);
    XFlush(b->dpy);
}

/* the text of the X side is here: GNOME is told that the session has text */
static void clip_offer_to_gnome(struct bridge *b, GBytes *text)
{
    GVariantBuilder options, types;

    /* the text GNOME gave, back from Android: nothing new */
    if (b->clip && !b->clip_from_x && g_bytes_equal(b->clip, text)) {
        g_bytes_unref(text);
        return;
    }
    g_clear_pointer(&b->clip, g_bytes_unref);
    b->clip = text;
    b->clip_from_x = TRUE;
    g_variant_builder_init(&types, G_VARIANT_TYPE("as"));
    for (int i = 0; i < (int) TEXT_TYPES; i++)
        g_variant_builder_add(&types, "s", text_types[i]);
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&options, "{sv}", "mime-types", g_variant_builder_end(&types));
    notify(b, "SetSelection", g_variant_new("(a{sv})", &options));
}

/* reads what the owner of the selection put on our window; FALSE when that was the last of it */
static gboolean clip_read_x(struct bridge *b, GByteArray *into, gboolean *in_pieces)
{
    unsigned long count = 0, left = 0;
    unsigned char *data = NULL;
    gboolean more = FALSE;
    int format = 0;
    Atom type = None;

    if (XGetWindowProperty(b->dpy, b->win, b->atom_transfer, 0, CLIP_MAX / 4, True, AnyPropertyType, &type, &format,
                           &count, &left, &data) != Success)
        return FALSE;
    if (in_pieces)
        *in_pieces = type == b->atom_incr;
    if (type != b->atom_incr && format == 8 && count > 0 && into->len + count <= CLIP_MAX) {
        g_byte_array_append(into, data, count);
        more = TRUE;
    }
    if (data)
        XFree(data);
    return more;
}

/* the owner of the X selection answered our request for its text */
static void clip_from_x(struct bridge *b, XSelectionEvent *event)
{
    GByteArray *text;
    gboolean in_pieces = FALSE;

    if (event->selection != b->atom_clipboard || event->property != b->atom_transfer)
        return; /* no property: the owner has no text */
    text = g_byte_array_new();
    clip_read_x(b, text, &in_pieces);
    g_clear_pointer(&b->clip_pieces, g_byte_array_unref);
    if (in_pieces) /* a long one: each piece comes as a new value of the property, an empty one ends it */
        b->clip_pieces = text;
    else
        clip_offer_to_gnome(b, g_byte_array_free_to_bytes(text));
}

static void clip_piece_from_x(struct bridge *b)
{
    if (clip_read_x(b, b->clip_pieces, NULL))
        return;
    clip_offer_to_gnome(b, g_byte_array_free_to_bytes(g_steal_pointer(&b->clip_pieces)));
}

/* somebody on the X side took the selection: Honor's server for Android, or an X program */
static void clip_x_owner_changed(struct bridge *b, XFixesSelectionNotifyEvent *event)
{
    if (event->selection != b->atom_clipboard || event->owner == b->win || event->owner == None)
        return;
    XConvertSelection(b->dpy, b->atom_clipboard, b->atom_text[1], b->atom_transfer, b->win, event->timestamp);
    XFlush(b->dpy);
}

static void on_gnome_text(GObject *source, GAsyncResult *result, gpointer data)
{
    struct bridge *b = data;
    g_autoptr(GOutputStream) memory = G_OUTPUT_STREAM(source);
    g_autoptr(GError) error = NULL;

    if (g_output_stream_splice_finish(memory, result, &error) < 0) {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED) && b->verbose)
            say("the text of GNOME did not arrive: %s", error->message);
        return;
    }
    g_clear_pointer(&b->clip, g_bytes_unref);
    b->clip = g_memory_output_stream_steal_as_bytes(G_MEMORY_OUTPUT_STREAM(memory));
    b->clip_from_x = FALSE;
    /* Honor's server sees the new owner and asks for the text */
    XSetSelectionOwner(b->dpy, b->atom_clipboard, b->win, CurrentTime);
    XFlush(b->dpy);
}

static void on_gnome_read(GObject *source, GAsyncResult *result, gpointer data)
{
    struct bridge *b = data;
    g_autoptr(GError) error = NULL;
    g_autoptr(GUnixFDList) fds = NULL;
    g_autoptr(GVariant) reply =
        g_dbus_connection_call_with_unix_fd_list_finish(G_DBUS_CONNECTION(source), &fds, result, &error);
    g_autoptr(GInputStream) in = NULL;
    gint32 handle;
    int fd;

    if (!reply) {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED) && b->verbose)
            say("GNOME did not give its text: %s", error->message);
        return;
    }
    g_variant_get(reply, "(h)", &handle);
    fd = fds ? g_unix_fd_list_get(fds, handle, NULL) : -1;
    if (fd < 0)
        return;
    in = g_unix_input_stream_new(fd, TRUE);
    g_output_stream_splice_async(g_memory_output_stream_new_resizable(), in,
                                 G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE | G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                                 G_PRIORITY_DEFAULT, b->clip_read, on_gnome_text, b);
}

/* something was copied in GNOME */
static void on_selection_owner_changed(GDBusConnection *bus, const char *sender, const char *path, const char *iface,
                                       const char *signal, GVariant *args, gpointer data)
{
    struct bridge *b = data;
    g_autoptr(GVariant) options = NULL;
    g_autofree const char **types = NULL;
    const char *wanted = NULL;
    gboolean ours = FALSE;

    g_variant_get(args, "(@a{sv})", &options);
    g_variant_lookup(options, "session-is-owner", "b", &ours);
    if (ours)
        return; /* the text of the X side, which the bridge has just offered */
    /* what was on its way from the previous owner is stale */
    if (b->clip_read) {
        g_cancellable_cancel(b->clip_read);
        g_clear_object(&b->clip_read);
    }
    /* in this signal the list is wrapped in a tuple, unlike in the calls */
    if (!g_variant_lookup(options, "mime-types", "(^a&s)", &types))
        return;
    for (int i = 0; i < (int) TEXT_TYPES && !wanted; i++)
        for (int t = 0; types[t]; t++)
            if (!strcmp(types[t], text_types[i]))
                wanted = text_types[i];
    if (!wanted)
        return; /* a picture, files: the X side keeps what it has */
    b->clip_read = g_cancellable_new();
    g_dbus_connection_call_with_unix_fd_list(b->bus, RD_NAME, b->rd_session, RD_SESSION_IFACE, "SelectionRead",
                                             g_variant_new("(s)", wanted), G_VARIANT_TYPE("(h)"),
                                             G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, NULL, b->clip_read, on_gnome_read, b);
}

struct clip_write {
    struct bridge *bridge;
    guint32 serial;
    GBytes *text;
    GOutputStream *out;
};

static void clip_write_end(struct clip_write *w, gboolean ok)
{
    notify(w->bridge, "SelectionWriteDone", g_variant_new("(ub)", w->serial, ok));
    g_bytes_unref(w->text);
    g_clear_object(&w->out);
    g_free(w);
}

static void on_text_written(GObject *source, GAsyncResult *result, gpointer data)
{
    struct clip_write *w = data;
    gboolean ok = g_output_stream_write_all_finish(G_OUTPUT_STREAM(source), result, NULL, NULL);

    g_output_stream_close(w->out, NULL, NULL);
    clip_write_end(w, ok);
}

static void on_write_fd(GObject *source, GAsyncResult *result, gpointer data)
{
    struct clip_write *w = data;
    g_autoptr(GUnixFDList) fds = NULL;
    g_autoptr(GVariant) reply =
        g_dbus_connection_call_with_unix_fd_list_finish(G_DBUS_CONNECTION(source), &fds, result, NULL);
    gint32 handle;
    gsize size;
    int fd = -1;

    if (reply) {
        g_variant_get(reply, "(h)", &handle);
        fd = fds ? g_unix_fd_list_get(fds, handle, NULL) : -1;
    }
    if (fd < 0) {
        clip_write_end(w, FALSE);
        return;
    }
    w->out = g_unix_output_stream_new(fd, TRUE);
    g_output_stream_write_all_async(w->out, g_bytes_get_data(w->text, &size), g_bytes_get_size(w->text),
                                    G_PRIORITY_DEFAULT, NULL, on_text_written, w);
}

/* a program of GNOME pastes the text the session offered */
static void on_selection_transfer(GDBusConnection *bus, const char *sender, const char *path, const char *iface,
                                  const char *signal, GVariant *args, gpointer data)
{
    struct bridge *b = data;
    struct clip_write *w = g_new0(struct clip_write, 1);
    const char *type;

    g_variant_get(args, "(&su)", &type, &w->serial);
    w->bridge = b;
    w->text = b->clip ? g_bytes_ref(b->clip) : g_bytes_new_static("", 0);
    g_dbus_connection_call_with_unix_fd_list(b->bus, RD_NAME, b->rd_session, RD_SESSION_IFACE, "SelectionWrite",
                                             g_variant_new("(u)", w->serial), G_VARIANT_TYPE("(h)"),
                                             G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, NULL, NULL, on_write_fd, w);
}

static void start_clipboard(struct bridge *b)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) reply = NULL;
    GVariantBuilder options;
    Window owner;

    if (!b->xfixes_event)
        return;
    g_dbus_connection_signal_subscribe(b->bus, RD_NAME, RD_SESSION_IFACE, "SelectionOwnerChanged", b->rd_session, NULL,
                                       G_DBUS_SIGNAL_FLAGS_NONE, on_selection_owner_changed, b, NULL);
    g_dbus_connection_signal_subscribe(b->bus, RD_NAME, RD_SESSION_IFACE, "SelectionTransfer", b->rd_session, NULL,
                                       G_DBUS_SIGNAL_FLAGS_NONE, on_selection_transfer, b, NULL);
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    reply = g_dbus_connection_call_sync(b->bus, RD_NAME, b->rd_session, RD_SESSION_IFACE, "EnableClipboard",
                                        g_variant_new("(a{sv})", &options), NULL, G_DBUS_CALL_FLAGS_NONE, 10000, NULL,
                                        &error);
    if (!reply) {
        say("no clipboard: %s", error->message);
        return;
    }
    XFixesSelectSelectionInput(b->dpy, b->win, b->atom_clipboard, XFixesSetSelectionOwnerNotifyMask);
    /* what Android copied before the desktop came up */
    owner = XGetSelectionOwner(b->dpy, b->atom_clipboard);
    if (owner != None && owner != b->win)
        XConvertSelection(b->dpy, b->atom_clipboard, b->atom_text[1], b->atom_transfer, b->win, CurrentTime);
    XFlush(b->dpy);
    say("clipboard shared with the X display");
}

static void drain_x(struct bridge *b)
{
    struct rect exposed[MAX_RECTS];
    int exposed_count = 0;

    while (XPending(b->dpy)) {
        XEvent event;

        XNextEvent(b->dpy, &event);
        switch (event.type) {
        case GenericEvent:
            if (event.xcookie.extension == b->xi_opcode && XGetEventData(b->dpy, &event.xcookie)) {
                handle_xi(b, event.xcookie.evtype, event.xcookie.data);
                XFreeEventData(b->dpy, &event.xcookie);
            }
            break;
        case KeyPress:
        case KeyRelease:
            handle_key(b, &event.xkey, event.type == KeyPress);
            break;
        case Expose:
            add_rect(exposed, &exposed_count, event.xexpose.x, event.xexpose.y, event.xexpose.width,
                     event.xexpose.height, b->width, b->height);
            break;
        case FocusOut:
            release_everything(b);
            break;
        case VisibilityNotify:
            /* something of X came on top (Honor's own keyboard, a dialog): stay below it, take the keys back later */
            break;
        case MapNotify:
            XSetInputFocus(b->dpy, b->win, RevertToPointerRoot, CurrentTime);
            break;
        case SelectionRequest:
            clip_answer_x(b, &event.xselectionrequest);
            break;
        case SelectionNotify:
            clip_from_x(b, &event.xselection);
            break;
        case PropertyNotify:
            if (b->clip_pieces && event.xproperty.atom == b->atom_transfer && event.xproperty.state == PropertyNewValue)
                clip_piece_from_x(b);
            break;
        default:
            if (b->xfixes_event && event.type == b->xfixes_event + XFixesSelectionNotify)
                clip_x_owner_changed(b, (XFixesSelectionNotifyEvent *) &event);
            break;
        }
    }
    if (exposed_count && b->held && b->have_format && !b->fb)
        put_rects(b, b->held, exposed, exposed_count);
}

struct x_source {
    GSource base;
    struct bridge *bridge;
    gpointer tag;
};

static gboolean x_source_prepare(GSource *source, int *timeout)
{
    *timeout = -1;
    return XPending(((struct x_source *) source)->bridge->dpy) > 0;
}

static gboolean x_source_check(GSource *source)
{
    struct x_source *x = (struct x_source *) source;

    if (g_source_query_unix_fd(source, x->tag) & (G_IO_ERR | G_IO_HUP)) {
        fail(x->bridge, "the X server is gone");
        return FALSE;
    }
    return XPending(x->bridge->dpy) > 0;
}

static gboolean x_source_dispatch(GSource *source, GSourceFunc callback, gpointer data)
{
    drain_x(((struct x_source *) source)->bridge);
    return G_SOURCE_CONTINUE;
}

static GSourceFuncs x_source_funcs = {
    .prepare = x_source_prepare,
    .check = x_source_check,
    .dispatch = x_source_dispatch,
};

static int on_x_error(Display *dpy, XErrorEvent *error)
{
    char text[128];

    XGetErrorText(dpy, error->error_code, text, sizeof(text));
    say("X error: %s (request %d.%d)", text, error->request_code, error->minor_code);
    return 0;
}

static int on_x_io_error(Display *dpy)
{
    say("the X server is gone");
    exit(1);
}

static gboolean start_x(struct bridge *b, const char *display_name)
{
    unsigned char bits[XIMaskLen(XI_LASTEVENT)] = {0};
    XIEventMask mask = {XIAllMasterDevices, sizeof(bits), bits};
    XSetWindowAttributes attributes = {0};
    XClassHint class_hint = {(char *) "llbridge", (char *) "Llbridge"};
    XVisualInfo wanted = {0}, *found;
    int screen, event, error, major = 2, minor = 2, count;
    struct x_source *source;
    Window root;

    b->dpy = XOpenDisplay(display_name);
    if (!b->dpy) {
        say("cannot open the X display %s", display_name ? display_name : "");
        return FALSE;
    }
    XSetErrorHandler(on_x_error);
    XSetIOErrorHandler(on_x_io_error);
    screen = DefaultScreen(b->dpy);
    root = RootWindow(b->dpy, screen);
    b->width = DisplayWidth(b->dpy, screen);
    b->height = DisplayHeight(b->dpy, screen);
    b->visual = DefaultVisual(b->dpy, screen);
    b->depth = DefaultDepth(b->dpy, screen);

    wanted.visualid = XVisualIDFromVisual(b->visual);
    found = XGetVisualInfo(b->dpy, VisualIDMask, &wanted, &count);
    if (!found || b->depth != 24 || found->red_mask != 0xff0000 || found->blue_mask != 0xff || ImageByteOrder(b->dpy) != LSBFirst) {
        say("the X screen is not 24 bits deep with blue in the low byte");
        return FALSE;
    }
    XFree(found);

    if (!XQueryExtension(b->dpy, "XInputExtension", &b->xi_opcode, &event, &error) ||
        XIQueryVersion(b->dpy, &major, &minor) != Success) {
        say("the X server has no XInput 2.2, so no touch");
        return FALSE;
    }

    /* nobody manages windows here: the window places itself and takes the keyboard itself */
    attributes.override_redirect = True;
    attributes.background_pixmap = None;
    attributes.bit_gravity = NorthWestGravity;
    attributes.event_mask = KeyPressMask | KeyReleaseMask | ExposureMask | FocusChangeMask | StructureNotifyMask |
                            PropertyChangeMask;
    b->win = XCreateWindow(b->dpy, root, 0, 0, b->width, b->height, 0, CopyFromParent, InputOutput, CopyFromParent,
                           CWOverrideRedirect | CWBackPixmap | CWBitGravity | CWEventMask, &attributes);
    XStoreName(b->dpy, b->win, "GNOME");
    XSetClassHint(b->dpy, b->win, &class_hint);
    b->gc = XCreateGC(b->dpy, b->win, 0, NULL);

    XISetMask(bits, XI_TouchBegin);
    XISetMask(bits, XI_TouchUpdate);
    XISetMask(bits, XI_TouchEnd);
    XISetMask(bits, XI_Motion);
    XISetMask(bits, XI_ButtonPress);
    XISetMask(bits, XI_ButtonRelease);
    XISelectEvents(b->dpy, b->win, &mask, 1);

    XkbSetDetectableAutoRepeat(b->dpy, True, NULL);
    if (XFixesQueryExtension(b->dpy, &event, &error)) {
        b->xfixes_event = event;
        XFixesHideCursor(b->dpy, root); /* GNOME draws its own cursor into the picture */
    }
    b->atom_clipboard = XInternAtom(b->dpy, "CLIPBOARD", False);
    b->atom_targets = XInternAtom(b->dpy, "TARGETS", False);
    b->atom_incr = XInternAtom(b->dpy, "INCR", False);
    b->atom_transfer = XInternAtom(b->dpy, "LLBRIDGE_CLIPBOARD", False);
    for (int i = 0; i < (int) TEXT_TYPES; i++)
        b->atom_text[i] = XInternAtom(b->dpy, text_types[i], False);

    XMapRaised(b->dpy, b->win);
    XFlush(b->dpy);

    source = (struct x_source *) g_source_new(&x_source_funcs, sizeof(struct x_source));
    source->bridge = b;
    source->tag = g_source_add_unix_fd(&source->base, ConnectionNumber(b->dpy), G_IO_IN | G_IO_ERR | G_IO_HUP);
    g_source_attach(&source->base, NULL);
    return TRUE;
}

/* ---------- main ---------- */

/* SIGUSR1 turns the frame statistics and the refused input on and off while the program runs */
static gboolean on_toggle_verbose(gpointer data)
{
    struct bridge *b = data;

    b->verbose = !b->verbose;
    b->stat_since = g_get_monotonic_time();
    b->stat_frames = 0;
    b->stat_put_us = 0;
    b->stat_pixels = 0;
    say("verbose %s", b->verbose ? "on" : "off");
    return G_SOURCE_CONTINUE;
}

static gboolean on_signal(gpointer data)
{
    struct bridge *b = data;

    b->exit_code = 0;
    g_main_loop_quit(b->loop);
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv)
{
    struct bridge bridge = {0}, *b = &bridge;
    const char *display_name = NULL;
    const char *framebuffer = NULL;
    g_autoptr(GError) error = NULL;

    b->connector = ""; /* the primary monitor */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--verbose")) {
            b->verbose = TRUE;
        } else if (!strcmp(argv[i], "--display") && i + 1 < argc) {
            display_name = argv[++i];
        } else if (!strcmp(argv[i], "--connector") && i + 1 < argc) {
            b->connector = argv[++i];
        } else if (!strcmp(argv[i], "--framebuffer") && i + 1 < argc) {
            framebuffer = argv[++i];
        } else if (!strcmp(argv[i], "--no-pacing")) {
            b->no_pacing = TRUE;
        } else {
            fprintf(stderr, "usage: llbridge [--display NAME] [--framebuffer FILE] [--connector NAME] [--no-pacing] [--verbose]\n");
            return 2;
        }
    }
    for (int i = 0; i < MAX_SLOTS; i++)
        b->touch_ids[i] = -1;
    b->stat_since = g_get_monotonic_time();
    b->loop = g_main_loop_new(NULL, FALSE);
    /* a program that stops reading a pasted text must not take the screen down */
    signal(SIGPIPE, SIG_IGN);

    b->bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (!b->bus) {
        say("no session bus: %s", error->message);
        return 1;
    }
    g_dbus_connection_set_exit_on_close(b->bus, FALSE);
    if (!start_x(b, display_name))
        return 1;
    if (!start_pipewire(b)) {
        say("cannot reach PipeWire");
        return 1;
    }
    if (!start_session(b))
        return 1;
    /* frames that come before this go through X; the first one here is copied whole */
    if (framebuffer)
        b->fb = fb_open(framebuffer, b->width, b->height, ConnectionNumber(b->dpy));
    start_clipboard(b);

    g_unix_signal_add(SIGTERM, on_signal, b);
    g_unix_signal_add(SIGINT, on_signal, b);
    g_unix_signal_add(SIGUSR1, on_toggle_verbose, b);
    g_main_loop_run(b->loop);

    /* the screen of X first: the shell may be going down too, and the goodbyes to it can take long */
    if (b->pace.timer)
        g_source_destroy(b->pace.timer);
    fb_close(b->fb);
    release_everything(b);
    stop_session(b);
    if (b->dpy)
        XCloseDisplay(b->dpy);
    return b->exit_code;
}
