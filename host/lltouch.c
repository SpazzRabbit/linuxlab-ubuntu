/*
 * lltouch.so: feeds real touches into the touchscreen of Honor's Xvfb.
 *
 * Honor's X server already has an XInput2 direct-touch device ("Honor Devices
 * touch", 15 contacts) and a function that drives it, vfbTouchScreenEvent(). The
 * Linux Lab app never uses them: it turns every touch into mouse clicks. Their
 * socket takes a single client, the app itself, so nobody else can send the
 * touch message either.
 *
 * This library is listed in /etc/ld.so.preload and stays idle in every process
 * except Xvfb. There it finds the server's own functions by name in the .symtab
 * of the executable (the binary is not stripped), opens a TCP socket on
 * 127.0.0.1 and hands it to the server's main loop. Each 6-byte frame "<BBHH"
 * (type, finger, x, y in screen pixels) becomes one vfbTouchScreenEvent() call:
 *
 *   1 begin   2 update   3 end   0 keep-alive
 *   4 pointer: "finger" is a button mask (bit 0 left, 1 middle, 2 right, 3 and 4
 *     wheel up and down, 5 and 6 wheel left and right), x and y the new position
 *
 * Touch-aware programs get real multi-touch; for the rest the server emulates
 * the pointer from the first finger, as on any Linux touchscreen. The pointer
 * frame drives Honor's mouse device through vfbPointerRawEvent(), for a
 * touchpad-like cursor mode.
 *
 * A stamp file guards against a start that kills the server: it is written
 * before anything else and removed once the socket is in the main loop. Finding
 * it at the next start means the previous attempt died, and the library turns
 * itself off (delete lltouch.failed to try again).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define PORT 54398
#define STAMP_STARTING "/usr/local/lib/lltouch.starting"
#define STAMP_FAILED "/usr/local/lib/lltouch.failed"
#define X_NOTIFY_READ 1
/* Honor's action codes are Android's: 0 down, 1 up, 2 move */
enum { ACTION_BEGIN = 0, ACTION_END = 1, ACTION_UPDATE = 2 };
/* offsetof(ScreenInfo, width) and height in xorg-server 21.1 (the struct is 328 bytes) */
#define SCREENINFO_WIDTH 320
#define SCREENINFO_HEIGHT 324

static struct {
    void (*vfbTouchScreenEvent)(int id, int action, double x, double y);
    void (*vfbPointerRawEvent)(const void *packet);
    void (*SetNotifyFd)(int fd, void (*notify)(int, int, void *), int mask, void *data);
    char *screenInfo;
} x;

static const struct {
    const char *name;
    void **slot;
} wanted[] = {
    {"vfbTouchScreenEvent", (void **) &x.vfbTouchScreenEvent},
    {"vfbPointerRawEvent", (void **) &x.vfbPointerRawEvent},
    {"SetNotifyFd", (void **) &x.SetNotifyFd},
    {"screenInfo", (void **) &x.screenInfo},
};
#define WANTED (sizeof(wanted) / sizeof(wanted[0]))

static pthread_t main_thread;
static int armed;
static int started;
static int listener = -1;
static int client = -1;
static unsigned char pending[6];
static size_t pending_len;
static struct {
    unsigned char down;
    unsigned short x, y;
} fingers[256];

/* The payload of Honor's pointer message: the last byte hides the cursor when set. */
struct pointer_packet {
    int32_t buttons;
    float x;
    float y;
    uint8_t hide_cursor;
} __attribute__((packed));

static int buttons_held;
static int last_pointer_x;
static int last_pointer_y;

static void pointer_event(int buttons, int px, int py) {
    struct pointer_packet packet = {buttons, (float) px, (float) py, 0};
    buttons_held = buttons;
    last_pointer_x = px;
    last_pointer_y = py;
    x.vfbPointerRawEvent(&packet);
}

static void say(const char *text) {
    fprintf(stderr, "lltouch: %s\n", text);
}

static int first_object(struct dl_phdr_info *info, size_t size, void *data) {
    (void) size;
    *(uintptr_t *) data = info->dlpi_addr; /* the first entry is the executable itself */
    return 1;
}

/* Look the wanted names up in the static symbol table of the running executable. */
static int resolve(void) {
    uintptr_t base = 0;
    struct stat st;
    size_t found = 0;
    int fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return 0;
    }
    if (fstat(fd, &st) != 0 || st.st_size < (off_t) sizeof(Elf64_Ehdr)) {
        close(fd);
        return 0;
    }
    unsigned char *map = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        return 0;
    }
    dl_iterate_phdr(first_object, &base);
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *) map;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) == 0 && eh->e_ident[EI_CLASS] == ELFCLASS64 && eh->e_shoff != 0
            && eh->e_shoff + (uint64_t) eh->e_shnum * sizeof(Elf64_Shdr) <= (uint64_t) st.st_size) {
        const Elf64_Shdr *sh = (const Elf64_Shdr *) (map + eh->e_shoff);
        for (int i = 0; i < eh->e_shnum; i++) {
            if (sh[i].sh_type != SHT_SYMTAB || sh[i].sh_link >= eh->e_shnum) {
                continue;
            }
            const Elf64_Sym *syms = (const Elf64_Sym *) (map + sh[i].sh_offset);
            const char *names = (const char *) (map + sh[sh[i].sh_link].sh_offset);
            size_t count = sh[i].sh_size / sizeof(Elf64_Sym);
            for (size_t s = 0; s < count; s++) {
                if (syms[s].st_shndx == SHN_UNDEF || syms[s].st_value == 0) {
                    continue;
                }
                const char *name = names + syms[s].st_name;
                for (size_t w = 0; w < WANTED; w++) {
                    if (*wanted[w].slot == NULL && strcmp(name, wanted[w].name) == 0) {
                        *wanted[w].slot = (void *) (base + syms[s].st_value);
                        found++;
                    }
                }
            }
        }
    }
    munmap(map, st.st_size);
    return found == WANTED;
}

static void on_client(int fd, int ready, void *data);

static void drop_client(void) {
    if (client < 0) {
        return;
    }
    x.SetNotifyFd(client, NULL, 0, NULL);
    close(client);
    client = -1;
    pending_len = 0;
    /* a finger that never got its end would stay down for ever, and so would a button */
    for (int id = 0; id < 256; id++) {
        if (fingers[id].down) {
            fingers[id].down = 0;
            x.vfbTouchScreenEvent(id, ACTION_END, fingers[id].x, fingers[id].y);
        }
    }
    if (buttons_held != 0) {
        pointer_event(0, last_pointer_x, last_pointer_y);
    }
    say("client disconnected");
}

static void on_frame(const unsigned char *f) {
    int id = f[1];
    unsigned short px = (unsigned short) (f[2] | (f[3] << 8));
    unsigned short py = (unsigned short) (f[4] | (f[5] << 8));
    switch (f[0]) {
        case 1:
            if (fingers[id].down) {
                x.vfbTouchScreenEvent(id, ACTION_END, fingers[id].x, fingers[id].y);
            }
            fingers[id].down = 1;
            fingers[id].x = px;
            fingers[id].y = py;
            x.vfbTouchScreenEvent(id, ACTION_BEGIN, px, py);
            break;
        case 2:
            if (fingers[id].down) {
                fingers[id].x = px;
                fingers[id].y = py;
                x.vfbTouchScreenEvent(id, ACTION_UPDATE, px, py);
            }
            break;
        case 3:
            if (fingers[id].down) {
                fingers[id].down = 0;
                x.vfbTouchScreenEvent(id, ACTION_END, px, py);
            }
            break;
        case 4:
            pointer_event(f[1] & 0x7f, px, py);
            break;
        default:
            break;
    }
}

/* Both handlers run in the main loop of the server, like Honor's own socket handler. */
static void on_client(int fd, int ready, void *data) {
    unsigned char buf[600];
    (void) ready;
    (void) data;
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            drop_client();
            return;
        }
        if (n < 0) {
            return;
        }
        for (ssize_t i = 0; i < n; i++) {
            pending[pending_len++] = buf[i];
            if (pending_len == sizeof(pending)) {
                on_frame(pending);
                pending_len = 0;
            }
        }
    }
}

static void on_listener(int fd, int ready, void *data) {
    char hello[48];
    int one = 1;
    (void) ready;
    (void) data;
    int incoming = accept4(fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (incoming < 0) {
        return;
    }
    drop_client(); /* the newest client wins */
    setsockopt(incoming, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    int len = snprintf(hello, sizeof(hello), "LLTOUCH %d %d\n",
                       *(int *) (x.screenInfo + SCREENINFO_WIDTH), *(int *) (x.screenInfo + SCREENINFO_HEIGHT));
    if (send(incoming, hello, (size_t) len, MSG_NOSIGNAL) < 0) {
        close(incoming);
        return;
    }
    client = incoming;
    x.SetNotifyFd(client, on_client, X_NOTIFY_READ, NULL);
    say("client connected");
}

static int open_listener(void) {
    struct sockaddr_in addr;
    int one = 1;
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) != 0 || listen(fd, 2) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void start(void) {
    listener = open_listener();
    if (listener < 0) {
        say("cannot listen on 127.0.0.1:54398");
    } else {
        x.SetNotifyFd(listener, on_listener, X_NOTIFY_READ, NULL);
        say("ready, listening on 127.0.0.1:54398");
    }
    unlink(STAMP_STARTING);
}

/*
 * The main loop of the server sleeps in epoll_wait; its first call from the main
 * thread means the server is up and its devices exist.
 */
int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout) {
    static int (*real)(int, struct epoll_event *, int, int);
    if (real == NULL) {
        real = (int (*)(int, struct epoll_event *, int, int)) dlsym(RTLD_NEXT, "epoll_wait");
    }
    if (armed && !started && pthread_equal(pthread_self(), main_thread)) {
        started = 1;
        start();
    }
    return real(epfd, events, maxevents, timeout);
}

__attribute__((constructor)) static void init(void) {
    char exe[256];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n < 5 || memcmp(exe + n - 5, "/Xvfb", 5) != 0) {
        return; /* any other program: do nothing at all */
    }
    if (access(STAMP_FAILED, F_OK) == 0) {
        return;
    }
    if (access(STAMP_STARTING, F_OK) == 0) {
        rename(STAMP_STARTING, STAMP_FAILED);
        say("the previous start did not finish, turned off");
        return;
    }
    int fd = open(STAMP_STARTING, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        return; /* without the guard we do not take the risk */
    }
    close(fd);
    main_thread = pthread_self();
    if (!resolve()) {
        unlink(STAMP_STARTING);
        say("this Xvfb has no vfbTouchScreenEvent, staying idle");
        return;
    }
    armed = 1;
}
