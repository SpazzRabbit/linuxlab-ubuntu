/*
 * llcapture.so: makes the screen cast of GNOME Shell cheap and exact. Preloaded into gnome-shell only.
 *
 * The screen of this session is a screen cast into plain memory (see llbridge.c). For such a cast
 * Mutter calls clutter_stage_paint_to_buffer() on every frame: it makes a texture of the size of the
 * monitor, paints the whole stage into it a second time and reads all of it back. On the tablet that
 * takes 15 ms and holds the desktop at about 45 frames a second, whatever the monitor could do.
 *
 * Here the monitor is a virtual one, and its view already is a texture that keeps the frame Mutter
 * has just painted. So the call is answered by reading that texture back, and only the parts of it
 * that changed: Mutter tells the stage what it is about to repaint (signal "paint-view"), and this
 * library remembers it for the last frames. The buffers of the stream take turns, so each one gets
 * everything that changed since it was written the last time.
 *
 * The same lists go to llbridge through a shared file (llcapture.h): the list Mutter attaches to a
 * frame misses what was painted while an earlier frame still waited to be recorded.
 *
 * Reading back still makes the shell wait for the GPU: 9 ms for a window of two megapixels, too much
 * for 120 frames a second. So while the screen keeps changing the read is asked for and collected one
 * frame later (a pixel buffer object, 2 ms instead of 9; dev/readback-test.c has the numbers). The stream
 * is then one frame behind. A change that stands alone is read at once, and when a run of changes ends,
 * a repaint of one pixel is queued so that the last frame comes out too.
 *
 * Mutter records a frame for such a stream "when idle", and a frame painted before that moment comes
 * is never recorded on its own. At 120 paintings a second about half of them went that way. The
 * source Mutter creates for it is given a priority just above painting (see g_source_set_name_by_id
 * below), so every frame painted is recorded before the next one is.
 *
 * When the request is not for exactly one whole view, the real function does the work.
 *
 *   LLCAPTURE=off       always the real function
 *   LLCAPTURE=full      read the whole view every time
 *   LLCAPTURE=sync      never read behind (the stream is never late, and slower)
 *   LLCAPTURE_STATS=1   a line per second on stderr
 *   LLCAPTURE_VSYNC=off the frame clock of the virtual monitor is left as Mutter runs it (see below)
 *   LLCAPTURE_VSYNC_LOG=<file>  a line per frame: when it was ready, its tick, how late the tick came
 *
 * The frame clock of a virtual monitor has no vertical blank to go by: Mutter takes the moment a
 * frame is painted for the moment it is shown, and when nothing has to be painted the clock stops.
 * So a window that paints a frame every time the previous one is "shown" (a video player on Wayland
 * takes its frame callbacks for the vertical blank) runs at the pace of the painting itself, which
 * is never even: the frames of a 60 Hz video came 9 to 28 ms apart, and the screen, which refreshes
 * at 120 Hz, held each for one, two or three refreshes. Here the notification of a shown frame is
 * made to look like one of a real monitor: the moment of showing is the next tick of a steady grid
 * at the refresh rate of the monitor, and it is marked as synchronised to the vertical blank, so that
 * Mutter schedules the painting of the frames as it does for a monitor: ahead of the tick they are
 * meant for, and on the grid again after a pause.
 *
 * Build: gcc -O2 -fPIC -shared -o llcapture.so llcapture.c -lm
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "llcapture.h"

typedef struct {
    int x, y, width, height;
} MtkRectangle;

typedef struct list {
    void *data;
    struct list *next, *prev;
} GList;

#define COGL_READ_PIXELS_COLOR_BUFFER 1
#define BUFFERS 16       /* buffers of streams that are followed */
#define BUFFER_FRESH 500 /* ms; a buffer not written for longer is read whole */
#define NEAR 16          /* pixels; rectangles closer than this are read as one */
#define RUN_GAP 30       /* ms; frames closer than this are a run of changes */
#define TAIL_WAIT 14     /* ms after the last frame of a run before it is flushed out */

#define GL_UNSIGNED_BYTE 0x1401
#define GL_BGRA 0x80E1
#define GL_PACK_ROW_LENGTH 0x0D02
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_PIXEL_PACK_BUFFER 0x88EB
#define GL_STREAM_READ 0x88E1
#define GL_MAP_READ_BIT 0x0001
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA

static struct {
    int ready; /* 0 not looked up yet, 1 usable, -1 a function is missing */
    int (*real)(void *, const MtkRectangle *, float, uint8_t *, int, int, void *, int, void **);
    GList *(*peek_views)(void *);
    void (*view_layout)(void *, MtkRectangle *);
    float (*view_scale)(void *);
    void *(*view_framebuffer)(void *);
    int (*fb_width)(void *);
    int (*fb_height)(void *);
    void *(*fb_context)(void *);
    void *(*bitmap_for_data)(void *, int, int, int, int, uint8_t *);
    int (*read_into_bitmap)(void *, int, int, int, void *);
    void (*unref)(void *);
    int (*region_count)(const void *);
    MtkRectangle (*region_rect)(const void *, int);
    MtkRectangle (*region_extents)(const void *);
    unsigned long (*signal_connect)(void *, const char *, void (*)(void), void *, void *, int);
    /* for reading behind; without them the reads are made at once */
    void (*fb_flush)(void *);
    void (*view_add_clip)(void *, const MtkRectangle *);
    void (*view_schedule)(void *);
    unsigned (*timeout_add)(unsigned, int (*)(void *), void *);
    int (*source_remove)(unsigned);
} api;

/* OpenGL itself, of the context the shell paints with */
static struct {
    void (*GenBuffers)(int, unsigned *);
    void (*BindBuffer)(unsigned, unsigned);
    void (*BufferData)(unsigned, long, const void *, unsigned);
    void (*ReadPixels)(int, int, int, int, unsigned, unsigned, void *);
    void *(*MapBufferRange)(unsigned, long, long, unsigned);
    unsigned char (*UnmapBuffer)(unsigned);
    void (*GetIntegerv)(unsigned, int *);
    void (*BindFramebuffer)(unsigned, unsigned);
    void (*PixelStorei)(unsigned, int);
    void (*Flush)(void);
    unsigned (*GetError)(void);
} gl;

static int stats;
static int vsync = 1;
static enum { BEHIND, SYNC, FULL, OFF } mode;

static void *followed;               /* the stage whose "paint-view" is connected */
static struct llcapture_shared *seen; /* the shared file, or private memory when it cannot be made */

static struct {
    uint8_t *data;
    uint64_t frame; /* the frame this buffer holds */
    double written;
} buffers[BUFFERS];

/* the child programs of the shell have no use for this library */
__attribute__((constructor)) static void init(void)
{
    const char *wanted = getenv("LLCAPTURE");

    mode = !wanted ? BEHIND : strcmp(wanted, "off") == 0 ? OFF : strcmp(wanted, "full") == 0 ? FULL :
           strcmp(wanted, "sync") == 0 ? SYNC : BEHIND;
    stats = getenv("LLCAPTURE_STATS") != NULL;
    vsync = !(wanted = getenv("LLCAPTURE_VSYNC")) || strcmp(wanted, "off") != 0;
    unsetenv("LD_PRELOAD");
}

static void lookup(void)
{
    api.real = dlsym(RTLD_NEXT, "clutter_stage_paint_to_buffer");
    api.peek_views = dlsym(RTLD_DEFAULT, "clutter_stage_peek_stage_views");
    api.view_layout = dlsym(RTLD_DEFAULT, "clutter_stage_view_get_layout");
    api.view_scale = dlsym(RTLD_DEFAULT, "clutter_stage_view_get_scale");
    api.view_framebuffer = dlsym(RTLD_DEFAULT, "clutter_stage_view_get_framebuffer");
    api.fb_width = dlsym(RTLD_DEFAULT, "cogl_framebuffer_get_width");
    api.fb_height = dlsym(RTLD_DEFAULT, "cogl_framebuffer_get_height");
    api.fb_context = dlsym(RTLD_DEFAULT, "cogl_framebuffer_get_context");
    api.bitmap_for_data = dlsym(RTLD_DEFAULT, "cogl_bitmap_new_for_data");
    api.read_into_bitmap = dlsym(RTLD_DEFAULT, "cogl_framebuffer_read_pixels_into_bitmap");
    api.unref = dlsym(RTLD_DEFAULT, "g_object_unref");
    api.region_count = dlsym(RTLD_DEFAULT, "mtk_region_num_rectangles");
    api.region_rect = dlsym(RTLD_DEFAULT, "mtk_region_get_rectangle");
    api.region_extents = dlsym(RTLD_DEFAULT, "mtk_region_get_extents");
    api.signal_connect = dlsym(RTLD_DEFAULT, "g_signal_connect_data");
    api.fb_flush = dlsym(RTLD_DEFAULT, "cogl_framebuffer_flush");
    api.view_add_clip = dlsym(RTLD_DEFAULT, "clutter_stage_view_add_redraw_clip");
    api.view_schedule = dlsym(RTLD_DEFAULT, "clutter_stage_view_schedule_update");
    api.timeout_add = dlsym(RTLD_DEFAULT, "g_timeout_add");
    api.source_remove = dlsym(RTLD_DEFAULT, "g_source_remove");
    api.ready = api.peek_views && api.view_layout && api.view_scale && api.view_framebuffer && api.fb_width &&
                        api.fb_height && api.fb_context && api.bitmap_for_data && api.read_into_bitmap && api.unref &&
                        api.region_count && api.region_rect && api.region_extents && api.signal_connect
                    ? 1
                    : -1;
    if (api.ready < 0)
        fprintf(stderr, "llcapture: this Mutter lacks a function, the screen cast stays as it is\n");
}

/* the file llbridge reads; without it the lists are kept all the same, for the buffers */
static void share(void)
{
    static struct llcapture_shared private_copy;
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    char path[512];
    void *map = MAP_FAILED;
    int fd;

    seen = &private_copy;
    if (!runtime)
        return;
    snprintf(path, sizeof path, "%s/" LLCAPTURE_FILE, runtime);
    /* the same file as before when it exists: a bridge that outlived the last shell has it mapped */
    fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd >= 0) {
        if (ftruncate(fd, sizeof *seen) == 0)
            map = mmap(NULL, sizeof *seen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
    }
    if (map == MAP_FAILED) {
        fprintf(stderr, "llcapture: cannot share %s, the bridge will use the lists of Mutter\n", path);
        return;
    }
    seen = map;
    memset(seen, 0, sizeof *seen);
    seen->pid = getpid();
    seen->magic = LLCAPTURE_MAGIC;
}

static double now_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* ---------- lists of rectangles ---------- */

static int near(const struct llcapture_rect *a, const struct llcapture_rect *b)
{
    return a->x < b->x + b->w + NEAR && b->x < a->x + a->w + NEAR && a->y < b->y + b->h + NEAR &&
           b->y < a->y + a->h + NEAR;
}

static struct llcapture_rect around(struct llcapture_rect a, struct llcapture_rect b)
{
    int x2 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w;
    int y2 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;

    a.x = a.x < b.x ? a.x : b.x;
    a.y = a.y < b.y ? a.y : b.y;
    a.w = x2 - a.x;
    a.h = y2 - a.y;
    return a;
}

/* adds a rectangle; ones that touch become one, and so do all of them when the list is full */
static void add(struct llcapture_frame *list, struct llcapture_rect r)
{
    if (list->count < 0 || r.w <= 0 || r.h <= 0)
        return;
    for (int i = 0; i < list->count;) {
        if (near(&list->rects[i], &r)) {
            r = around(list->rects[i], r);
            list->rects[i] = list->rects[--list->count];
            i = 0;
        } else {
            i++;
        }
    }
    if (list->count == LLCAPTURE_RECTS) {
        for (int i = 0; i < list->count; i++)
            r = around(list->rects[i], r);
        list->count = 0;
    }
    list->rects[list->count++] = r;
}

/* what the frames after @from up to @to changed; count -1 for "everything" */
static void changed_between(struct llcapture_frame *list, uint64_t from, uint64_t to)
{
    list->count = 0;
    if (from == 0 || from > to || seen->frame - from >= LLCAPTURE_FRAMES) {
        list->count = -1;
        return;
    }
    for (uint64_t f = from + 1; f <= to; f++) {
        const struct llcapture_frame *frame = &seen->frames[f % LLCAPTURE_FRAMES];

        if (frame->count < 0) {
            list->count = -1;
            return;
        }
        for (int i = 0; i < frame->count; i++)
            add(list, frame->rects[i]);
    }
}

/* how the frames are spaced: counts of gaps up to 9, 13, 18, 26 ms and longer (a still screen is no gap) */
static int gaps[5];

static void note_gap(void)
{
    static double last;
    double now = now_ms(), gap = now - last;

    last = now;
    if (gap < 100)
        gaps[gap <= 9 ? 0 : gap <= 13 ? 1 : gap <= 18 ? 2 : gap <= 26 ? 3 : 4]++;
}

/* Mutter is about to repaint these parts of a view (stage coordinates); NULL is all of it */
static FILE *tick_log;
static int64_t monotonic_us(void);

static void on_paint_view(void *stage, void *view, const void *redraw_clip, void *frame, void *unused)
{
    struct llcapture_frame *changed = &seen->frames[(seen->frame + 1) % LLCAPTURE_FRAMES];
    MtkRectangle layout;
    float scale;
    int count;

    if (tick_log)
        fprintf(tick_log, "%lld paint\n", (long long) monotonic_us());
    if (stats)
        note_gap();
    changed->count = -1;
    if (redraw_clip) {
        api.view_layout(view, &layout);
        scale = api.view_scale(view);
        count = api.region_count(redraw_clip);
        changed->count = 0;
        for (int i = 0; i < (count > LLCAPTURE_RECTS ? 1 : count); i++) {
            MtkRectangle clip = count > LLCAPTURE_RECTS ? api.region_extents(redraw_clip) : api.region_rect(redraw_clip, i);
            struct llcapture_rect r;

            /* outwards, and a pixel more: the clip was rounded once already on its way here */
            r.x = (int) floorf((clip.x - layout.x) * scale) - 1;
            r.y = (int) floorf((clip.y - layout.y) * scale) - 1;
            r.w = (int) ceilf((clip.x - layout.x + clip.width) * scale) + 1 - r.x;
            r.h = (int) ceilf((clip.y - layout.y + clip.height) * scale) + 1 - r.y;
            add(changed, r);
        }
    }
    /* the entry first, then the number that makes it count */
    __atomic_store_n(&seen->frame, seen->frame + 1, __ATOMIC_RELEASE);
}

/* the framebuffer of the only view, when it shows exactly this rectangle at this scale */
static void *whole_view(void *stage, const MtkRectangle *rect, float scale, int width, int height, void **view)
{
    GList *views = api.peek_views(stage);
    MtkRectangle layout;
    void *framebuffer;

    if (!views || views->next)
        return NULL;
    api.view_layout(views->data, &layout);
    if (layout.x != rect->x || layout.y != rect->y || layout.width != rect->width || layout.height != rect->height ||
        fabsf(api.view_scale(views->data) - scale) > 0.001f)
        return NULL;
    framebuffer = api.view_framebuffer(views->data);
    if (!framebuffer || api.fb_width(framebuffer) != width || api.fb_height(framebuffer) != height)
        return NULL;
    *view = views->data;
    return framebuffer;
}

/* the entry of a buffer of a stream; the one unused for the longest time is taken for a new buffer */
static int buffer_of(uint8_t *data)
{
    int oldest = 0;

    for (int i = 0; i < BUFFERS; i++) {
        if (buffers[i].data == data)
            return i;
        if (buffers[i].written < buffers[oldest].written)
            oldest = i;
    }
    buffers[oldest].data = data;
    buffers[oldest].frame = 0;
    buffers[oldest].written = 0;
    return oldest;
}

/* the part of a rectangle inside the view; 0 when there is none */
static int inside(struct llcapture_rect *r, int width, int height)
{
    if (r->x < 0) {
        r->w += r->x;
        r->x = 0;
    }
    if (r->y < 0) {
        r->h += r->y;
        r->y = 0;
    }
    if (r->x + r->w > width)
        r->w = width - r->x;
    if (r->y + r->h > height)
        r->h = height - r->y;
    return r->w > 0 && r->h > 0;
}

/* reads a rectangle of the view into its place in @data, waiting for the GPU */
static int read_part(void *framebuffer, struct llcapture_rect r, int width, int height, uint8_t *data, int stride,
                     int format, double *megapixels)
{
    void *bitmap;
    int result;

    if (!inside(&r, width, height))
        return 1;
    bitmap = api.bitmap_for_data(api.fb_context(framebuffer), r.w, r.h, format, stride,
                                 data + (size_t) r.y * stride + (size_t) r.x * 4);
    result = api.read_into_bitmap(framebuffer, r.x, r.y, COGL_READ_PIXELS_COLOR_BUFFER, bitmap);
    api.unref(bitmap);
    *megapixels += r.w * (double) r.h / 1e6;
    return result;
}

/* ---------- reading behind ---------- */

static struct {
    int state;         /* 0 not tried yet, 1 in use, -1 given up: the reads are made at once */
    void *framebuffer; /* the framebuffer the OpenGL name below belongs to */
    void *view;
    unsigned fbo;
    int flipped;       /* rows come out of OpenGL bottom first */
    unsigned pbo[2];
    int next;
    uint8_t *shadow;   /* the latest frame that has been read back, whole */
    int width, height, stride;
    uint64_t shadow_frame;
    int pending; /* a read has been asked for: */
    unsigned pending_pbo;
    uint64_t pending_frame;
    struct llcapture_frame pending_rects;
    double last_record;
    unsigned timer;
    int flush_queued;
} behind;

static int lookup_gl(void)
{
    void *(*get)(const char *) = dlsym(RTLD_DEFAULT, "eglGetProcAddress");

    if (!get)
        return 0;
    gl.GenBuffers = get("glGenBuffers");
    gl.BindBuffer = get("glBindBuffer");
    gl.BufferData = get("glBufferData");
    gl.ReadPixels = get("glReadPixels");
    gl.MapBufferRange = get("glMapBufferRange");
    gl.UnmapBuffer = get("glUnmapBuffer");
    gl.GetIntegerv = get("glGetIntegerv");
    gl.BindFramebuffer = get("glBindFramebuffer");
    gl.PixelStorei = get("glPixelStorei");
    gl.Flush = get("glFlush");
    gl.GetError = get("glGetError");
    return gl.GenBuffers && gl.BindBuffer && gl.BufferData && gl.ReadPixels && gl.MapBufferRange && gl.UnmapBuffer &&
           gl.GetIntegerv && gl.BindFramebuffer && gl.PixelStorei && gl.Flush && gl.GetError && api.fb_flush &&
           api.view_add_clip && api.view_schedule && api.timeout_add && api.source_remove;
}

/* asks OpenGL for rectangles of the view, into a pixel buffer object, one after another; no waiting */
static void ask(void *framebuffer, const struct llcapture_frame *rects, unsigned pbo)
{
    int bound = 0, row_length = 0, alignment = 4;
    long offset = 0;

    api.fb_flush(framebuffer);
    gl.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &bound);
    gl.GetIntegerv(GL_PACK_ROW_LENGTH, &row_length);
    gl.GetIntegerv(GL_PACK_ALIGNMENT, &alignment);
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, behind.fbo);
    gl.PixelStorei(GL_PACK_ROW_LENGTH, 0);
    gl.PixelStorei(GL_PACK_ALIGNMENT, 1);
    gl.BindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
    for (int i = 0; i < rects->count; i++) {
        const struct llcapture_rect *r = &rects->rects[i];

        gl.ReadPixels(r->x, behind.flipped ? behind.height - r->y - r->h : r->y, r->w, r->h, GL_BGRA,
                      GL_UNSIGNED_BYTE, (void *) offset);
        offset += (long) r->w * r->h * 4;
    }
    gl.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    gl.PixelStorei(GL_PACK_ROW_LENGTH, row_length);
    gl.PixelStorei(GL_PACK_ALIGNMENT, alignment);
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, bound);
    gl.Flush();
}

/* copies what was asked for into its place in @data; 0 when OpenGL did not give it */
static int collect(const struct llcapture_frame *rects, unsigned pbo, uint8_t *data, int stride)
{
    const uint8_t *map, *from;
    long total = 0;

    for (int i = 0; i < rects->count; i++)
        total += (long) rects->rects[i].w * rects->rects[i].h * 4;
    gl.BindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
    map = gl.MapBufferRange(GL_PIXEL_PACK_BUFFER, 0, total, GL_MAP_READ_BIT);
    from = map;
    for (int i = 0; map && i < rects->count; i++) {
        const struct llcapture_rect *r = &rects->rects[i];

        for (int row = 0; row < r->h; row++)
            memcpy(data + (size_t) (r->y + (behind.flipped ? r->h - 1 - row : row)) * stride + (size_t) r->x * 4,
                   from + (size_t) row * r->w * 4, (size_t) r->w * 4);
        from += (size_t) r->w * r->h * 4;
    }
    if (map)
        gl.UnmapBuffer(GL_PIXEL_PACK_BUFFER);
    gl.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    return map != NULL;
}

/*
 * Finds the OpenGL name of the framebuffer and checks on a corner of the screen that a read made
 * here gives the very bytes Cogl gives. Anything else, and the reads stay with Cogl.
 */
static int learn(void *framebuffer, int format)
{
    int side = behind.width < 512 || behind.height < 512 ? 64 : 512;
    struct llcapture_frame corner = {1, {{0, 0, side, side}}};
    uint8_t pixel[4], *cogl, *mine;
    void *bitmap;
    int name = 0, same = 0;

    /* Cogl leaves the framebuffer it has just read from bound for reading */
    bitmap = api.bitmap_for_data(api.fb_context(framebuffer), 1, 1, format, 4, pixel);
    api.read_into_bitmap(framebuffer, 0, 0, COGL_READ_PIXELS_COLOR_BUFFER, bitmap);
    api.unref(bitmap);
    gl.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &name);
    if (name <= 0)
        return 0;
    behind.fbo = name;
    if (!behind.pbo[0])
        gl.GenBuffers(2, behind.pbo);
    for (int i = 0; i < 2; i++) {
        gl.BindBuffer(GL_PIXEL_PACK_BUFFER, behind.pbo[i]);
        gl.BufferData(GL_PIXEL_PACK_BUFFER, (long) behind.stride * behind.height, NULL, GL_STREAM_READ);
    }
    gl.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    cogl = malloc((size_t) side * side * 4);
    mine = malloc((size_t) side * side * 4);
    if (!cogl || !mine)
        return 0;
    bitmap = api.bitmap_for_data(api.fb_context(framebuffer), side, side, format, side * 4, cogl);
    api.read_into_bitmap(framebuffer, 0, 0, COGL_READ_PIXELS_COLOR_BUFFER, bitmap);
    api.unref(bitmap);
    while (gl.GetError())
        ;
    for (behind.flipped = 0; behind.flipped < 2 && !same; behind.flipped++) {
        ask(framebuffer, &corner, behind.pbo[0]);
        same = collect(&corner, behind.pbo[0], mine, side * 4) && !gl.GetError() &&
               memcmp(cogl, mine, (size_t) side * side * 4) == 0;
    }
    behind.flipped--;
    free(cogl);
    free(mine);
    return same;
}

/* ready to read behind for this view? */
static int behind_ready(void *framebuffer, void *view, int width, int height, int stride, int format)
{
    if (mode != BEHIND || behind.state < 0)
        return 0;
    if (behind.state == 0 && !lookup_gl()) {
        behind.state = -1;
        return 0;
    }
    if (behind.framebuffer != framebuffer || behind.width != width || behind.height != height ||
        behind.stride != stride) {
        /* the first time, or the monitor has changed */
        free(behind.shadow);
        behind.shadow = malloc((size_t) stride * height);
        behind.framebuffer = framebuffer;
        behind.width = width;
        behind.height = height;
        behind.stride = stride;
        behind.shadow_frame = 0;
        behind.pending = 0;
        behind.fbo = 0;
        if (!behind.shadow || stride != width * 4 || !learn(framebuffer, format)) {
            fprintf(stderr, "llcapture: reading behind does not work here, every frame is read at once\n");
            behind.state = -1;
            return 0;
        }
        behind.state = 1;
        fprintf(stderr, "llcapture: reading one frame behind while the screen keeps changing\n");
    }
    behind.view = view;
    return 1;
}

/* a run of changes has ended with a frame still to come: one pixel is repainted to bring it out */
static int flush_tail(void *unused)
{
    GList *views = followed ? api.peek_views(followed) : NULL;

    behind.timer = 0;
    if (behind.pending && views && !views->next && views->data == behind.view) {
        MtkRectangle layout;

        api.view_layout(behind.view, &layout);
        layout.width = layout.height = 1;
        behind.flush_queued = 1;
        api.view_add_clip(behind.view, &layout);
        api.view_schedule(behind.view);
    }
    return 0;
}

/* nothing but the pixel of flush_tail() */
static int only_flush(const struct llcapture_frame *list)
{
    return behind.flush_queued && list->count == 1 && list->rects[0].x <= 0 && list->rects[0].y <= 0 &&
           list->rects[0].w <= 8 && list->rects[0].h <= 8;
}

/*
 * Brings the shadow as far as it goes without waiting and fills the buffer of the stream from it.
 * Returns the frame the buffer now holds.
 */
static uint64_t record_behind(void *framebuffer, int entry, uint8_t *data, int format, double started,
                              double *megapixels, int *waited)
{
    int width = behind.width, height = behind.height, stride = behind.stride;
    int run = started - behind.last_record < RUN_GAP;
    struct llcapture_frame news, lacks;
    int ok = 1;

    behind.last_record = started;
    if (behind.pending) {
        if (collect(&behind.pending_rects, behind.pending_pbo, behind.shadow, stride))
            behind.shadow_frame = behind.pending_frame;
        else
            behind.shadow_frame = 0;
        behind.pending = 0;
    }

    changed_between(&news, behind.shadow_frame, seen->frame);
    for (int i = 0; i < news.count;) {
        if (inside(&news.rects[i], width, height))
            i++;
        else
            news.rects[i] = news.rects[--news.count];
    }
    if (news.count < 0) {
        ok = read_part(framebuffer, (struct llcapture_rect) {0, 0, width, height}, width, height, behind.shadow, stride,
                       format, megapixels);
        behind.shadow_frame = ok ? seen->frame : 0;
        *waited = 1;
    } else if (news.count == 0 || only_flush(&news)) {
        behind.shadow_frame = seen->frame;
    } else if (run) {
        behind.pending_pbo = behind.pbo[behind.next];
        behind.next = !behind.next;
        behind.pending_rects = news;
        behind.pending_frame = seen->frame;
        behind.pending = 1;
        ask(framebuffer, &news, behind.pending_pbo);
        for (int i = 0; i < news.count; i++)
            *megapixels += news.rects[i].w * (double) news.rects[i].h / 1e6;
    } else {
        for (int i = 0; i < news.count && ok; i++)
            ok = read_part(framebuffer, news.rects[i], width, height, behind.shadow, stride, format, megapixels);
        behind.shadow_frame = ok ? seen->frame : 0;
        *waited = 1;
    }
    behind.flush_queued = 0;

    if (behind.timer) {
        api.source_remove(behind.timer);
        behind.timer = 0;
    }
    if (behind.pending)
        behind.timer = api.timeout_add(TAIL_WAIT, flush_tail, NULL);

    if (!behind.shadow_frame)
        return 0;
    changed_between(&lacks, buffers[entry].frame, behind.shadow_frame);
    if (started - buffers[entry].written > BUFFER_FRESH)
        lacks.count = -1;
    if (lacks.count < 0) {
        memcpy(data, behind.shadow, (size_t) stride * height);
    } else {
        for (int i = 0; i < lacks.count; i++) {
            struct llcapture_rect r = lacks.rects[i];

            if (!inside(&r, width, height))
                continue;
            for (int y = r.y; y < r.y + r.h; y++)
                memcpy(data + (size_t) y * stride + (size_t) r.x * 4, behind.shadow + (size_t) y * stride + (size_t) r.x * 4,
                       (size_t) r.w * 4);
        }
    }
    return behind.shadow_frame;
}

static void count(double started, double megapixels, int fast, int waited)
{
    static double since, spent, pixels;
    static int calls, fast_calls, waits;
    double now = now_ms();

    spent += now - started;
    pixels += megapixels;
    calls++;
    fast_calls += fast;
    waits += waited;
    if (since == 0)
        since = now;
    if (now - since >= 1000) {
        fprintf(stderr,
                "llcapture: %d frames/s, %.1f ms and %.1f Mpx per frame, %d of %d read from the view, %d waited "
                "for the GPU; gaps <=9ms %d, <=13 %d, <=18 %d, <=26 %d, longer %d\n",
                (int) (calls * 1000 / (now - since)), spent / calls, pixels / calls, fast_calls, calls, waits, gaps[0],
                gaps[1], gaps[2], gaps[3], gaps[4]);
        memset(gaps, 0, sizeof gaps);
        since = now;
        spent = pixels = 0;
        calls = fast_calls = waits = 0;
    }
}

static int paint_to_buffer(void *stage, const MtkRectangle *rect, float scale, uint8_t *data, int stride,
                           int format, void *color_state, int paint_flags, void **error);

int clutter_stage_paint_to_buffer(void *stage, const MtkRectangle *rect, float scale, uint8_t *data, int stride,
                                  int format, void *color_state, int paint_flags, void **error)
{
    int64_t started = tick_log ? monotonic_us() : 0;
    int result = paint_to_buffer(stage, rect, scale, data, stride, format, color_state, paint_flags, error);

    if (tick_log)
        fprintf(tick_log, "%lld record %lld\n", (long long) started, (long long) (monotonic_us() - started));
    return result;
}

static int paint_to_buffer(void *stage, const MtkRectangle *rect, float scale, uint8_t *data, int stride,
                           int format, void *color_state, int paint_flags, void **error)
{
    double started = now_ms();
    double megapixels = 0;
    void *framebuffer = NULL;
    int result = 0, waited = 0;

    if (stride != (int) roundf(rect->width * scale) * 4) {
        /*
         * The buffer was made for a picture of another width: the monitor has just been turned or
         * resized, and Mutter still records into the buffers of the old stream until that stream is
         * closed. Filling such a buffer writes megabytes past its end, here and in the real function
         * alike. So nothing is written, and the frame is reported as failed.
         */
        static void (*set_error)(void **, unsigned, int, const char *);
        static unsigned (*io_error)(void);

        if (!set_error) {
            set_error = dlsym(RTLD_DEFAULT, "g_set_error_literal");
            io_error = dlsym(RTLD_DEFAULT, "g_io_error_quark");
        }
        if (set_error && io_error)
            set_error(error, io_error(), 0, "the buffer of the stream does not fit the monitor any more");
        return 0;
    }
    if (!api.ready)
        lookup();
    if (api.ready > 0 && mode != OFF) {
        int width = (int) roundf(rect->width * scale);
        int height = (int) roundf(rect->height * scale);
        struct llcapture_record *record;
        uint64_t holds;
        void *view = NULL;

        if (!seen)
            share();
        if (followed != stage) {
            /* the first call for this stage: its frames are followed from now on, this one is read whole */
            api.signal_connect(stage, "paint-view", (void (*)(void)) on_paint_view, NULL, NULL, 0);
            followed = stage;
            for (int i = 0; i < LLCAPTURE_FRAMES; i++)
                seen->frames[i].count = -1;
            __atomic_store_n(&seen->frame, seen->frame + LLCAPTURE_FRAMES, __ATOMIC_RELEASE);
        }
        holds = seen->frame;

        framebuffer = whole_view(stage, rect, scale, width, height, &view);
        if (framebuffer) {
            int entry = buffer_of(data);

            if (behind_ready(framebuffer, view, width, height, stride, format)) {
                holds = record_behind(framebuffer, entry, data, format, started, &megapixels, &waited);
                result = holds != 0;
            } else {
                struct llcapture_frame lacks;

                changed_between(&lacks, buffers[entry].frame, seen->frame);
                if (mode == FULL || started - buffers[entry].written > BUFFER_FRESH)
                    lacks.count = -1;
                result = 1;
                waited = 1;
                if (lacks.count < 0)
                    result = read_part(framebuffer, (struct llcapture_rect) {0, 0, width, height}, width, height, data,
                                       stride, format, &megapixels);
                for (int i = 0; i < lacks.count && result; i++)
                    result = read_part(framebuffer, lacks.rects[i], width, height, data, stride, format, &megapixels);
            }
            if (result) {
                buffers[entry].frame = holds;
                buffers[entry].written = started;
            } else {
                buffers[entry].frame = 0;
                framebuffer = NULL;
                holds = seen->frame;
            }
        }
        /* for the bridge: this frame went to a stream now (the timestamp of the buffer is a moment older) */
        record = &seen->record[seen->records % LLCAPTURE_RECORDS];
        record->time_us = (int64_t) (started * 1000);
        record->frame = holds;
        __atomic_store_n(&seen->records, seen->records + 1, __ATOMIC_RELEASE);
    }
    if (!framebuffer) {
        if (!api.real)
            return 0;
        result = api.real(stage, rect, scale, data, stride, format, color_state, paint_flags, error);
        megapixels = rect->width * scale * rect->height * scale / 1e6;
        waited = 1;
        if (api.ready > 0 && mode != OFF)
            buffers[buffer_of(data)].frame = 0;
    }
    if (stats)
        count(started, megapixels, framebuffer != NULL, waited);
    return result;
}

/*
 * A monitor that is turned, resized or given another scale gets new views, and the old ones are
 * destroyed with their framebuffers. Mutter hands this function to g_clear_list() by address, from
 * another library, so the one here is the one that is called.
 */
static void forget_ticks(void *view);

void clutter_stage_view_destroy(void *view)
{
    static void (*real)(void *);

    if (!real)
        real = dlsym(RTLD_NEXT, "clutter_stage_view_destroy");
    forget_ticks(view);
    /* what was learned about this view and its framebuffer is void: a new one may get the same address */
    if (behind.view == view || behind.framebuffer) {
        if (behind.timer && api.source_remove)
            api.source_remove(behind.timer);
        behind.timer = 0;
        behind.view = NULL;
        behind.framebuffer = NULL;
        behind.fbo = 0;
        behind.pending = 0;
        behind.shadow_frame = 0;
    }
    if (real)
        real(view);
}

/*
 * Mutter names its "record the frame when idle" source right after creating it, which is where it can
 * be caught: g_idle_add_once() gives it the lowest priority there is, below the painting of the next
 * frame. One step above painting (CLUTTER_PRIORITY_REDRAW, 150) it runs after each frame instead.
 */
#define RECORD_PRIORITY 149

void g_source_set_name_by_id(unsigned tag, const char *name)
{
    static void (*real)(unsigned, const char *);
    static void *(*find)(void *, unsigned);
    static void (*set_priority)(void *, int);

    if (!real) {
        real = dlsym(RTLD_NEXT, "g_source_set_name_by_id");
        find = dlsym(RTLD_DEFAULT, "g_main_context_find_source_by_id");
        set_priority = dlsym(RTLD_DEFAULT, "g_source_set_priority");
    }
    real(tag, name);
    if (mode != OFF && find && set_priority && name && strncmp(name, "[mutter] maybe_record_frame_on_idle", 35) == 0) {
        void *source = find(NULL, tag);

        if (source)
            set_priority(source, RECORD_PRIORITY);
    }
}

/* ---------- the frame clock of the virtual monitor ---------- */

/* ClutterFrameInfo of Mutter 50 */
typedef struct {
    int64_t global_frame_counter;
    int64_t view_frame_counter;
    int64_t presentation_time; /* microseconds, CLOCK_MONOTONIC */
    float refresh_rate;
    int flags;
    unsigned sequence;
    int64_t kms_ready_time_us;
} ClutterFrameInfo;

#define CLUTTER_FRAME_INFO_FLAG_ZERO_COPY (1 << 1)
#define CLUTTER_FRAME_INFO_FLAG_VSYNC (1 << 2)
#define G_PRIORITY_HIGH (-100)

static struct {
    void (*real)(void *, ClutterFrameInfo *);
    void *(*timeout_source_new)(unsigned);
    void (*source_set_ready_time)(void *, int64_t);
    void (*source_set_priority)(void *, int);
    void (*source_set_callback)(void *, int (*)(void *), void *, void (*)(void *));
    unsigned (*source_attach)(void *, void *);
    void (*source_unref)(void *);
    void (*source_destroy)(void *);
} vblank;

/* a frame waiting for its tick */
static struct tick {
    void *view;
    void *source;
    ClutterFrameInfo info;
} ticks[4];

static int64_t monotonic_us(void)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t) now.tv_sec * 1000000 + now.tv_nsec / 1000;
}

static int deliver(void *data)
{
    struct tick *t = data;
    void *view = t->view;
    ClutterFrameInfo info = t->info;

    t->source = NULL;
    t->view = NULL;
    if (tick_log)
        fprintf(tick_log, "%lld ready %lld tick %lld delivered %+lld\n", (long long) info.kms_ready_time_us,
                (long long) info.kms_ready_time_us, (long long) info.presentation_time,
                (long long) (monotonic_us() - info.presentation_time));
    vblank.real(view, &info);
    return 0;
}

/*
 * The frame is reported as shown at the next tick of a grid at the refresh rate of the monitor, and
 * at that moment, not before: Mutter then sends the frame callbacks of the windows at the tick, like a
 * monitor, and plans the next frame ahead of the next tick. No two frames share a tick. The moment
 * the frame really was ready is given as the moment it reached "KMS", so that the clock measures how
 * long its frames take. The frame is marked as copied directly as well, which keeps the clock from
 * painting two frames ahead (triple buffering), which the stream behind the screen cannot use.
 */
void clutter_stage_view_notify_presented(void *view, ClutterFrameInfo *info)
{
    static int64_t last_tick;
    int64_t at, tick;
    double interval;
    struct tick *t = NULL;

    if (!vblank.real) {
        const char *path = getenv("LLCAPTURE_VSYNC_LOG");

        if (path)
            tick_log = fopen(path, "a");
        if (tick_log)
            setvbuf(tick_log, NULL, _IOLBF, 0);
        vblank.real = dlsym(RTLD_NEXT, "clutter_stage_view_notify_presented");
        vblank.timeout_source_new = dlsym(RTLD_DEFAULT, "g_timeout_source_new");
        vblank.source_set_ready_time = dlsym(RTLD_DEFAULT, "g_source_set_ready_time");
        vblank.source_set_priority = dlsym(RTLD_DEFAULT, "g_source_set_priority");
        vblank.source_set_callback = dlsym(RTLD_DEFAULT, "g_source_set_callback");
        vblank.source_attach = dlsym(RTLD_DEFAULT, "g_source_attach");
        vblank.source_unref = dlsym(RTLD_DEFAULT, "g_source_unref");
        vblank.source_destroy = dlsym(RTLD_DEFAULT, "g_source_destroy");
    }
    if (!vsync || !info || info->refresh_rate <= 1 || (info->flags & CLUTTER_FRAME_INFO_FLAG_VSYNC) ||
        !vblank.timeout_source_new || !vblank.source_set_ready_time || !vblank.source_set_priority ||
        !vblank.source_set_callback || !vblank.source_attach || !vblank.source_unref) {
        vblank.real(view, info);
        return;
    }
    for (int i = 0; i < 4 && !t; i++)
        if (!ticks[i].view)
            t = &ticks[i];
    if (!t) {
        vblank.real(view, info);
        return;
    }
    at = monotonic_us();
    interval = 1e6 / info->refresh_rate;
    tick = (int64_t) (ceil(at / interval) * interval);
    if (tick <= last_tick)
        tick = (int64_t) ((floor(last_tick / interval) + 1) * interval);
    last_tick = tick;

    t->view = view;
    t->info = *info;
    t->info.presentation_time = tick;
    t->info.flags |= CLUTTER_FRAME_INFO_FLAG_VSYNC | CLUTTER_FRAME_INFO_FLAG_ZERO_COPY;
    if (!t->info.kms_ready_time_us)
        t->info.kms_ready_time_us = at;
    t->source = vblank.timeout_source_new(0);
    vblank.source_set_ready_time(t->source, tick);
    vblank.source_set_priority(t->source, G_PRIORITY_HIGH);
    vblank.source_set_callback(t->source, deliver, t, NULL);
    vblank.source_attach(t->source, NULL);
    vblank.source_unref(t->source);
}

/* the view is going away with its frame clock: what waited for a tick is dropped */
static void forget_ticks(void *view)
{
    for (int i = 0; i < 4; i++) {
        if (ticks[i].view != view)
            continue;
        if (ticks[i].source && vblank.source_destroy)
            vblank.source_destroy(ticks[i].source);
        ticks[i].source = NULL;
        ticks[i].view = NULL;
    }
}
