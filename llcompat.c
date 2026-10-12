/*
 * llcompat.so: ten things Ubuntu needs and Linux Lab (PRoot inside an Android app) gets wrong, and
 * one a touch screen needs. Loaded into every process through /etc/ld.so.preload; it only steps in
 * after the real call has failed.
 *
 * 1. fstat() on a memfd fails with EACCES, while reading, writing and mmap work.
 *    PipeWire will not map a block it cannot stat, so the screen capture of GNOME Shell never starts;
 *    the image loaders of GLib hand pictures over the same way.
 *    The refused call is answered here: a memfd is a regular file, and lseek() still tells its size.
 *
 * 2. /proc/<pid>/root cannot be opened (ENOENT). PipeWire and xdg-desktop-portal open it to see
 *    whether a client lives in a sandbox, and treat the failure as "sandboxed": the client then waits
 *    for permissions nobody grants. Every process here has the same root, so "/" is opened instead.
 *
 * 3. sd_pid_get_user_unit() says "no such process" about the calling process itself, because
 *    /proc/<pid>/cgroup is not readable. GNOME Shell takes that for a broken systemd session and
 *    refuses to bring up X11 programs. The honest answer here is "not in a unit".
 *
 * 4. Something in the session sends SIGKILL to its whole process group now and then (kill(0) or
 *    kill(-group)): GNOME Shell, PipeWire and the daemons die at once. Such a call is written to
 *    /tmp/llcompat-kill.log with the program that made it, and refused unless it comes from a shell or
 *    timeout(1), which use it on purpose. Every other SIGKILL is written there too.
 *
 * 5. Terminals. Since glibc 2.42 tcgetattr() and tcsetattr() use the ioctls TCGETS2 and TCSETS2, and
 *    Android lets an app use only the old TCGETS and TCSETS: every program gets EACCES, isatty()
 *    says "no" about a real terminal, ttyname() fails. Shells lose their line editing, and the
 *    progress window of Language Support cannot name its terminal. The refused calls are done again
 *    with the old ioctls.
 *
 * 6. The limit of real-time running. PipeWire (its module-rt, which lives in GNOME Shell and in every
 *    program that plays or records) and Mutter ask RealtimeKit how long a real-time thread may run, get
 *    no answer here, and set RLIMIT_RTTIME to 0. Nothing in Linux Lab can become real-time by asking,
 *    so on a PC that would be harmless. Here the GPU driver of the tablet lends its real-time priority
 *    to a thread that holds its lock, in the middle of an ioctl; the scheduler finds a real-time
 *    thread whose limit is 0 and kills the process with SIGKILL, with no word in any log. The shell
 *    died this way on every other change of the monitors and sometimes right after its start, and
 *    every program it started inherited the limit. A request to lower this limit is answered "done"
 *    and not passed on; /tmp/llcompat-rlimit.log names who asked.
 *
 * 7. openat2(). Android does not let an app make this system call, and what comes back is ENOSYS. The
 *    tar of Ubuntu 26.04 opens everything below the top directory of an archive with it and knows no
 *    other way: of any archive only the top level could be unpacked. The call is done here with
 *    openat(), one step of the path at a time, and what the caller asked for is kept: no way out of
 *    the starting directory, symbolic links followed only while they do not lead out of it (or not
 *    at all). It is stricter than the kernel in one thing: ".." is never taken.
 *
 * 8. The name of the processor. The /proc/cpuinfo of an ARM kernel has no "model name": only the
 *    numbers of the maker and of the core, and the name of the chip in one "Hardware" line at the
 *    end. GNOME Settings (System, About) and System Monitor take the name from libgtop, which leaves
 *    it empty for the cores of this tablet, and Settings showed "(null) x 8". Every processor that libgtop lists gets a "model name" here, made of the
 *    maker (by its number) and the chip: "Qualcomm SM8845". This one does not wait for a failure.
 *
 * 9. polkitd in the chroot mode. It is started as root and makes itself the user "polkitd": it clears
 *    its groups and changes its uid. In the chroot mode of a rooted tablet (see llchroot.py) it is
 *    started as the user of Linux Lab, who may change neither, and it ended with "Error switching to
 *    user polkitd: Error clearing groups". Without polkit, accounts-daemon never comes up (GNOME Shell
 *    waited 25 seconds for it at every start), and neither does anything that asks for permission.
 *    polkitd skips the switch when it already is that user, so a polkitd that is not root is told that
 *    the user "polkitd" is the one it runs as. Under PRoot it is root and sees the real entry.
 *
 * 10. Credentials on a socket under fakeroot. In the chroot mode sudo is fakeroot (see sudo-chroot): a
 *    program run with it believes it is root, and GLib passes that on to the kernel when it connects
 *    to D-Bus (SCM_CREDENTIALS with uid 0). The kernel refuses with EPERM, so nothing run with sudo
 *    could reach a bus: the hook that apt runs for PackageKit after every install printed "Error
 *    sending credentials: Operation not permitted". A message refused that way is sent again with the
 *    ids the process really has. The bus learns nothing new: it takes them from the kernel anyway.
 *
 * 11. The menu of a folder by touch. Files (Nautilus 50) opens the menu of a file on a long press, and
 *    the menu of the folder itself (New Folder, Paste, Open in Terminal, Properties) only on a right
 *    click in its empty part: with a finger there was no way to it. When Files gives a folder view
 *    (NautilusListBase) its click gesture, the view gets a long press here as well, touch only, which
 *    does what that right click does. A long press on a file is left to the file. Only in nautilus.
 *
 * Build: gcc -O2 -fPIC -shared -o llcompat.so llcompat.c
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#ifndef RESOLVE_BENEATH
#include <linux/openat2.h>
#endif
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/syscall.h>
#include <unistd.h>

static int is_memfd(int fd)
{
    char link[64], target[16];
    ssize_t n;

    snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    n = readlink(link, target, sizeof target - 1);
    if (n < 7)
        return 0;
    return memcmp(target, "/memfd:", 7) == 0;
}

/* the size without stat: the offset is put back, nobody else moves it meanwhile in practice */
static off_t memfd_size(int fd)
{
    off_t here = lseek(fd, 0, SEEK_CUR);
    off_t size = lseek(fd, 0, SEEK_END);

    if (here >= 0)
        lseek(fd, here, SEEK_SET);
    return size < 0 ? 0 : size;
}

static int refused(int fd, int result)
{
    return result == -1 && errno == EACCES && fd >= 0 && is_memfd(fd);
}

static int fill_stat(int fd, struct stat *st)
{
    memset(st, 0, sizeof *st);
    st->st_mode = S_IFREG | 0777;
    st->st_nlink = 1;
    st->st_uid = getuid();
    st->st_gid = getgid();
    st->st_size = memfd_size(fd);
    st->st_blksize = 4096;
    st->st_blocks = (st->st_size + 511) / 512;
    st->st_dev = makedev(0, 1);
    st->st_ino = ((ino_t) getpid() << 20) | (ino_t) fd;
    errno = 0;
    return 0;
}

static int empty_path(const char *path, int flags)
{
    return (flags & AT_EMPTY_PATH) && (!path || !*path);
}

int fstat(int fd, struct stat *st)
{
    static int (*real)(int, struct stat *);
    int result;

    if (!real)
        real = dlsym(RTLD_NEXT, "fstat");
    result = real(fd, st);
    return refused(fd, result) ? fill_stat(fd, st) : result;
}

int fstat64(int fd, struct stat64 *st)
{
    static int (*real)(int, struct stat64 *);
    int result;

    if (!real)
        real = dlsym(RTLD_NEXT, "fstat64");
    result = real(fd, st);
    /* struct stat and struct stat64 are the same layout on 64-bit Linux */
    return refused(fd, result) ? fill_stat(fd, (struct stat *) st) : result;
}

int fstatat(int dirfd, const char *path, struct stat *st, int flags)
{
    static int (*real)(int, const char *, struct stat *, int);
    int result;

    if (!real)
        real = dlsym(RTLD_NEXT, "fstatat");
    result = real(dirfd, path, st, flags);
    return empty_path(path, flags) && refused(dirfd, result) ? fill_stat(dirfd, st) : result;
}

int fstatat64(int dirfd, const char *path, struct stat64 *st, int flags)
{
    static int (*real)(int, const char *, struct stat64 *, int);
    int result;

    if (!real)
        real = dlsym(RTLD_NEXT, "fstatat64");
    result = real(dirfd, path, st, flags);
    return empty_path(path, flags) && refused(dirfd, result) ? fill_stat(dirfd, (struct stat *) st) : result;
}

int statx(int dirfd, const char *restrict path, int flags, unsigned int mask, struct statx *restrict stx)
{
    static int (*real)(int, const char *, int, unsigned int, struct statx *);
    int result;

    if (!real)
        real = dlsym(RTLD_NEXT, "statx");
    result = real(dirfd, path, flags, mask, stx);
    if (!(empty_path(path, flags) && refused(dirfd, result)))
        return result;
    memset(stx, 0, sizeof *stx);
    stx->stx_mask = STATX_BASIC_STATS;
    stx->stx_mode = S_IFREG | 0777;
    stx->stx_nlink = 1;
    stx->stx_uid = getuid();
    stx->stx_gid = getgid();
    stx->stx_size = memfd_size(dirfd);
    stx->stx_blksize = 4096;
    stx->stx_blocks = (stx->stx_size + 511) / 512;
    stx->stx_dev_minor = 1;
    stx->stx_ino = ((unsigned long long) getpid() << 20) | (unsigned long long) dirfd;
    errno = 0;
    return 0;
}

/* "/proc/<pid>/root" or "/proc/self/root", nothing after it */
static int is_proc_root(const char *path)
{
    const char *p;

    if (!path || strncmp(path, "/proc/", 6) != 0)
        return 0;
    p = path + 6;
    if (strncmp(p, "self", 4) == 0) {
        p += 4;
    } else {
        if (*p < '0' || *p > '9')
            return 0;
        while (*p >= '0' && *p <= '9')
            p++;
    }
    return strcmp(p, "/root") == 0 || strcmp(p, "/root/") == 0;
}

#define OPEN_MODE(flags, last) \
    mode_t mode = 0; \
    if ((flags) & (O_CREAT | __O_TMPFILE)) { \
        va_list ap; \
        va_start(ap, last); \
        mode = va_arg(ap, mode_t); \
        va_end(ap); \
    }

#define OPEN_LIKE(name) \
    int name(const char *path, int flags, ...) \
    { \
        static int (*real)(const char *, int, ...); \
        int fd; \
        OPEN_MODE(flags, flags) \
        if (!real) \
            real = dlsym(RTLD_NEXT, #name); \
        fd = real(path, flags, mode); \
        if (fd == -1 && errno == ENOENT && is_proc_root(path)) \
            fd = real("/", flags, mode); \
        return fd; \
    }

#define OPENAT_LIKE(name) \
    int name(int dirfd, const char *path, int flags, ...) \
    { \
        static int (*real)(int, const char *, int, ...); \
        int fd; \
        OPEN_MODE(flags, flags) \
        if (!real) \
            real = dlsym(RTLD_NEXT, #name); \
        fd = real(dirfd, path, flags, mode); \
        if (fd == -1 && errno == ENOENT && is_proc_root(path)) \
            fd = real(dirfd, "/", flags, mode); \
        return fd; \
    }

OPEN_LIKE(open)
OPEN_LIKE(open64)
OPENAT_LIKE(openat)
OPENAT_LIKE(openat64)

/* the checked variants that glibc headers pick when the flags are not a constant */
int __open_2(const char *path, int flags)
{
    return open(path, flags);
}

int __open64_2(const char *path, int flags)
{
    return open64(path, flags);
}

int __openat_2(int dirfd, const char *path, int flags)
{
    return openat(dirfd, path, flags);
}

int __openat64_2(int dirfd, const char *path, int flags)
{
    return openat64(dirfd, path, flags);
}

int sd_pid_get_user_unit(pid_t pid, char **unit)
{
    static int (*real)(pid_t, char **);
    int result;

    if (!real)
        real = dlsym(RTLD_NEXT, "sd_pid_get_user_unit");
    if (!real)
        return -ENODATA;
    result = real(pid, unit);
    return result == -ESRCH && (pid == 0 || pid == getpid()) ? -ENODATA : result;
}

/* the name of a process, "?" when it cannot be read */
static void comm_of(pid_t pid, char *out, size_t size)
{
    char path[64];
    ssize_t n = -1;
    int fd;

    if (pid > 0)
        snprintf(path, sizeof path, "/proc/%d/comm", (int) pid);
    else
        snprintf(path, sizeof path, "/proc/self/comm");
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        n = read(fd, out, size - 1);
        close(fd);
    }
    if (n <= 0) {
        out[0] = '?';
        n = 1;
    }
    if (out[n - 1] == '\n')
        n--;
    out[n] = 0;
}

/* shells and timeout(1) signal their own group on purpose */
static int group_killer_allowed(const char *comm)
{
    static const char *const allowed[] = {"bash", "sh", "dash", "timeout", "runuser", "sudo", NULL};

    for (int i = 0; allowed[i]; i++)
        if (strcmp(comm, allowed[i]) == 0)
            return 1;
    return 0;
}

static void note_kill(pid_t target, int sig, const char *verdict)
{
    char self[32], whom[32], exe[256], line[512];
    struct timespec now;
    ssize_t n;
    int fd;

    comm_of(0, self, sizeof self);
    comm_of(target > 0 ? target : 0, whom, sizeof whom);
    n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    exe[n > 0 ? n : 0] = 0;
    clock_gettime(CLOCK_REALTIME, &now);
    n = snprintf(line, sizeof line, "%ld.%03ld %s: pid %d (%s, %s, group %d) kill(%d, %d)%s%s\n", (long) now.tv_sec,
                 now.tv_nsec / 1000000, verdict, (int) getpid(), self, exe, (int) getpgrp(), (int) target, sig,
                 target > 0 ? " target " : "", target > 0 ? whom : "");
    fd = open("/tmp/llcompat-kill.log", O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
    if (fd >= 0) {
        if (write(fd, line, n) < 0) {
            /* nothing to do about it */
        }
        close(fd);
    }
}

int kill(pid_t pid, int sig)
{
    static int (*real)(pid_t, int);
    char self[32];

    if (!real)
        real = dlsym(RTLD_NEXT, "kill");
    if (sig == SIGKILL && (pid == 0 || pid == -getpgrp() || pid == -1)) {
        comm_of(0, self, sizeof self);
        if (!group_killer_allowed(self)) {
            note_kill(pid, sig, "refused");
            errno = EPERM;
            return -1;
        }
        note_kill(pid, sig, "allowed");
    } else if (sig == SIGKILL) {
        note_kill(pid, sig, "sent");
    }
    return real(pid, sig);
}

int killpg(pid_t group, int sig)
{
    return kill(group > 0 ? -group : group, sig);
}

/* what the kernel takes with the old ioctls */
struct old_termios {
    unsigned int c_iflag, c_oflag, c_cflag, c_lflag;
    unsigned char c_line;
    unsigned char c_cc[19];
};

#define OLD_TCGETS 0x5401
#define OLD_TCSETS 0x5402 /* + 1 after the output is sent, + 2 with the input dropped too */

/* the speed in the control flags, in baud */
static unsigned int speed_of(unsigned int cflag)
{
    static const unsigned int low[] = {0,    50,   75,   110,  134,  150,  200,   300,
                                       600,  1200, 1800, 2400, 4800, 9600, 19200, 38400};
    static const unsigned int high[] = {0,       57600,   115200,  230400,  460800,  500000,  576000,  921600,
                                        1000000, 1152000, 1500000, 2000000, 2500000, 3000000, 3500000, 4000000};

    return cflag & 0x1000 ? high[cflag & 15] : low[cflag & 15];
}

static int old_get(int fd, struct termios *out)
{
    struct old_termios old;

    if (ioctl(fd, OLD_TCGETS, &old) < 0)
        return -1;
    if (out) {
        memset(out, 0, sizeof *out);
        out->c_iflag = old.c_iflag;
        out->c_oflag = old.c_oflag;
        out->c_cflag = old.c_cflag;
        out->c_lflag = old.c_lflag;
        out->c_line = old.c_line;
        memcpy(out->c_cc, old.c_cc, sizeof old.c_cc);
        out->c_ispeed = out->c_ospeed = speed_of(old.c_cflag);
    }
    return 0;
}

int tcgetattr(int fd, struct termios *out)
{
    static int (*real)(int, struct termios *);
    int result;

    if (!real)
        real = dlsym(RTLD_NEXT, "tcgetattr");
    result = real(fd, out);
    return result < 0 && errno == EACCES ? old_get(fd, out) : result;
}

int tcsetattr(int fd, int when, const struct termios *in)
{
    static int (*real)(int, int, const struct termios *);
    struct old_termios old;
    int result;

    if (!real)
        real = dlsym(RTLD_NEXT, "tcsetattr");
    result = real(fd, when, in);
    if (!(result < 0 && errno == EACCES) || when < TCSANOW || when > TCSAFLUSH)
        return result;
    old.c_iflag = in->c_iflag;
    old.c_oflag = in->c_oflag;
    old.c_cflag = in->c_cflag;
    old.c_lflag = in->c_lflag;
    old.c_line = in->c_line;
    memcpy(old.c_cc, in->c_cc, sizeof old.c_cc);
    return ioctl(fd, OLD_TCSETS + (when - TCSANOW), &old);
}

int isatty(int fd)
{
    static int (*real)(int);
    int result;

    if (!real)
        real = dlsym(RTLD_NEXT, "isatty");
    result = real(fd);
    if (result == 0 && errno == EACCES)
        return old_get(fd, NULL) == 0;
    return result;
}

/* the name of a pseudo-terminal as /proc knows it; 0 when @fd is not one */
static int pts_name(int fd, char *buf, size_t size)
{
    char link[64];
    ssize_t n;

    if (size < 12 || old_get(fd, NULL) < 0)
        return 0;
    snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    n = readlink(link, buf, size - 1);
    if (n < 10)
        return 0;
    buf[n] = 0;
    return strncmp(buf, "/dev/pts/", 9) == 0;
}

int ttyname_r(int fd, char *buf, size_t size)
{
    static int (*real)(int, char *, size_t);
    int result;

    if (!real)
        real = dlsym(RTLD_NEXT, "ttyname_r");
    result = real(fd, buf, size);
    return result == EACCES && pts_name(fd, buf, size) ? 0 : result;
}

/* what the headers of glibc turn ttyname_r() into when they know the size of the buffer */
int __ttyname_r_chk(int fd, char *buf, size_t size, size_t real_size)
{
    return ttyname_r(fd, buf, size < real_size ? size : real_size);
}

char *ttyname(int fd)
{
    static char *(*real)(int);
    static char name[64];
    char *result;

    if (!real)
        real = dlsym(RTLD_NEXT, "ttyname");
    result = real(fd);
    if (!result && errno == EACCES && pts_name(fd, name, sizeof name))
        return name;
    return result;
}

/* a limit on real-time running that is not "none" */
static int limits_rttime(int resource, rlim_t soft, rlim_t hard)
{
    return resource == RLIMIT_RTTIME && (soft != RLIM_INFINITY || hard != RLIM_INFINITY);
}

/* once per process: who wanted the limit, by the library the call came from */
static void note_rttime(void *caller, rlim_t soft, rlim_t hard)
{
    static int noted;
    char self[32], line[512];
    Dl_info where = {0};
    int fd, n;

    if (noted)
        return;
    noted = 1;
    comm_of(0, self, sizeof self);
    dladdr(caller, &where);
    n = snprintf(line, sizeof line, "%ld pid %d (%s): RLIMIT_RTTIME %lld:%lld asked by %s, left alone\n", (long) time(NULL),
                 (int) getpid(), self, (long long) soft, (long long) hard, where.dli_fname ? where.dli_fname : "?");
    fd = open("/tmp/llcompat-rlimit.log", O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
    if (fd >= 0) {
        if (write(fd, line, n) < 0) {
            /* nothing to do about it */
        }
        close(fd);
    }
}

int setrlimit(__rlimit_resource_t resource, const struct rlimit *wanted)
{
    static int (*real)(__rlimit_resource_t, const struct rlimit *);

    if (!real)
        real = dlsym(RTLD_NEXT, "setrlimit");
    if (limits_rttime(resource, wanted->rlim_cur, wanted->rlim_max)) {
        note_rttime(__builtin_return_address(0), wanted->rlim_cur, wanted->rlim_max);
        return 0;
    }
    return real(resource, wanted);
}

int setrlimit64(__rlimit_resource_t resource, const struct rlimit64 *wanted)
{
    static int (*real)(__rlimit_resource_t, const struct rlimit64 *);

    if (!real)
        real = dlsym(RTLD_NEXT, "setrlimit64");
    if (limits_rttime(resource, wanted->rlim_cur, wanted->rlim_max)) {
        note_rttime(__builtin_return_address(0), wanted->rlim_cur, wanted->rlim_max);
        return 0;
    }
    return real(resource, wanted);
}

int prlimit(pid_t pid, __rlimit_resource_t resource, const struct rlimit *wanted, struct rlimit *old)
{
    static int (*real)(pid_t, __rlimit_resource_t, const struct rlimit *, struct rlimit *);

    if (!real)
        real = dlsym(RTLD_NEXT, "prlimit");
    if (wanted && limits_rttime(resource, wanted->rlim_cur, wanted->rlim_max)) {
        note_rttime(__builtin_return_address(0), wanted->rlim_cur, wanted->rlim_max);
        return old ? real(pid, resource, NULL, old) : 0;
    }
    return real(pid, resource, wanted, old);
}

int prlimit64(pid_t pid, __rlimit_resource_t resource, const struct rlimit64 *wanted, struct rlimit64 *old)
{
    static int (*real)(pid_t, __rlimit_resource_t, const struct rlimit64 *, struct rlimit64 *);

    if (!real)
        real = dlsym(RTLD_NEXT, "prlimit64");
    if (wanted && limits_rttime(resource, wanted->rlim_cur, wanted->rlim_max)) {
        note_rttime(__builtin_return_address(0), wanted->rlim_cur, wanted->rlim_max);
        return old ? real(pid, resource, NULL, old) : 0;
    }
    return real(pid, resource, wanted, old);
}

/*
 * openat2() by hand for the callers that confine the path: opens @path below @dirfd one step at a
 * time. @links counts the symbolic links followed so far.
 */
static int open_below(int dirfd, const char *path, int flags, mode_t mode, unsigned long long resolve, int links)
{
    char step[NAME_MAX + 1], target[PATH_MAX], onward[PATH_MAX];
    int at = dirfd, own = 0, result = -1, error;

    if (path[0] == '/' && !(resolve & RESOLVE_IN_ROOT)) {
        errno = EXDEV;
        return -1;
    }
    for (;;) {
        const char *end;
        size_t length;
        ssize_t linked;
        int last, next;

        path += strspn(path, "/");
        end = path + strcspn(path, "/");
        length = end - path;
        last = end[strspn(end, "/")] == 0;
        if (length == 0 || (length == 1 && path[0] == '.')) {
            if (last) {
                result = openat(at, ".", flags, mode);
                break;
            }
            path = end;
            continue;
        }
        if (length > NAME_MAX) {
            errno = ENAMETOOLONG;
            break;
        }
        memcpy(step, path, length);
        step[length] = 0;
        if (strcmp(step, "..") == 0) {
            errno = EXDEV;
            break;
        }
        linked = readlinkat(at, step, target, sizeof target - 1);
        if (linked >= 0) {
            target[linked] = 0;
            if (last && (flags & O_NOFOLLOW)) {
                if (flags & O_PATH)
                    result = openat(at, step, flags, mode);
                else
                    errno = ELOOP;
                break;
            }
            if ((resolve & RESOLVE_NO_SYMLINKS) || links >= 40 ||
                snprintf(onward, sizeof onward, "%s%s", target, end) >= (int) sizeof onward) {
                errno = ELOOP;
                break;
            }
            /* from the directory of the link: where it points, then the rest of the path */
            result = open_below(at, onward, flags, mode, resolve, links + 1);
            break;
        }
        if (last) {
            /* it was not a link a moment ago, and must not be one now */
            result = openat(at, step, flags | O_NOFOLLOW, mode);
            break;
        }
        next = openat(at, step, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0)
            break;
        if (own)
            close(at);
        at = next;
        own = 1;
        path = end;
    }
    error = errno;
    if (own)
        close(at);
    errno = error;
    return result;
}

int openat2(int dirfd, const char *path, const struct open_how *how, size_t size)
{
    static int (*real)(int, const char *, const struct open_how *, size_t);
    static int looked;
    int result;

    if (!looked) {
        real = dlsym(RTLD_NEXT, "openat2");
        looked = 1;
    }
    if (real) {
        result = real(dirfd, path, how, size);
        if (result >= 0 || errno != ENOSYS)
            return result;
    }
    if (size < 24) {
        errno = EINVAL;
        return -1;
    }
    if (!(how->resolve & (RESOLVE_BENEATH | RESOLVE_IN_ROOT | RESOLVE_NO_SYMLINKS)))
        return openat(dirfd, path, how->flags, how->mode);
    return open_below(dirfd, path, how->flags, how->mode, how->resolve, 0);
}

/* ---------- 8. the name of the processor, for libgtop ---------- */

/* glibtop_sysinfo of libgtop 2 */
struct gtop_entry {
    void *labels;
    void *values; /* GHashTable: key -> value, both freed by the table */
    void *descriptions;
};

struct gtop_sysinfo {
    unsigned long long flags;
    unsigned long long ncpu;
    struct gtop_entry cpuinfo[];
};

/* the makers of ARM cores, by "CPU implementer" */
static const char *cpu_maker(unsigned code)
{
    switch (code) {
    case 0x41: return "ARM";
    case 0x42: return "Broadcom";
    case 0x46: return "Fujitsu";
    case 0x48: return "HiSilicon";
    case 0x4e: return "NVIDIA";
    case 0x51: return "Qualcomm";
    case 0x53: return "Samsung";
    case 0x61: return "Apple";
    case 0x6d: return "Microsoft";
    case 0xc0: return "Ampere";
    }
    return NULL;
}

/* "Qualcomm SM8845" from the maker and the "Hardware" line; NULL when neither is known */
static char *cpu_name(const char *implementer)
{
    const char *maker = implementer ? cpu_maker((unsigned) strtoul(implementer, NULL, 0)) : NULL;
    char line[256], chip[128] = "", *name;
    FILE *f = fopen("/proc/cpuinfo", "re");

    if (f) {
        while (fgets(line, sizeof line, f)) {
            char *colon = strchr(line, ':');

            if (strncmp(line, "Hardware", 8) != 0 || !colon)
                continue;
            colon++;
            colon += strspn(colon, " \t");
            snprintf(chip, sizeof chip, "%s", colon);
            chip[strcspn(chip, "\n")] = 0;
        }
        fclose(f);
    }
    if (!maker && !*chip)
        return NULL;
    if (!*chip)
        return asprintf(&name, "%s", maker) < 0 ? NULL : name;
    if (!maker || strcasestr(chip, maker))
        return asprintf(&name, "%s", chip) < 0 ? NULL : name;
    return asprintf(&name, "%s %s", maker, chip) < 0 ? NULL : name;
}

static void name_processors(struct gtop_sysinfo *info)
{
    static int done;
    void *(*lookup)(void *, const void *) = dlsym(RTLD_DEFAULT, "g_hash_table_lookup");
    int (*insert)(void *, void *, void *) = dlsym(RTLD_DEFAULT, "g_hash_table_insert");
    char *(*copy)(const char *) = dlsym(RTLD_DEFAULT, "g_strdup");
    const char *known;
    char *name;

    if (done || !info || !lookup || !insert || !copy || info->ncpu == 0 || !info->cpuinfo[0].values)
        return;
    done = 1;
    known = lookup(info->cpuinfo[0].values, "model name");
    if (known && *known)
        return; /* a kernel that says it; libgtop of Ubuntu puts an empty one there for cores it does not know */
    name = cpu_name(lookup(info->cpuinfo[0].values, "CPU implementer"));
    if (!name)
        return;
    for (unsigned long long i = 0; i < info->ncpu; i++)
        if (info->cpuinfo[i].values)
            insert(info->cpuinfo[i].values, copy("model name"), copy(name));
    free(name);
}

const struct gtop_sysinfo *glibtop_get_sysinfo(void)
{
    static const struct gtop_sysinfo *(*real)(void);
    const struct gtop_sysinfo *info;

    if (!real)
        real = dlsym(RTLD_NEXT, "glibtop_get_sysinfo");
    if (!real)
        return NULL;
    info = real();
    name_processors((struct gtop_sysinfo *) info);
    return info;
}

const struct gtop_sysinfo *glibtop_get_sysinfo_s(void *server)
{
    static const struct gtop_sysinfo *(*real)(void *);
    const struct gtop_sysinfo *info;

    if (!real)
        real = dlsym(RTLD_NEXT, "glibtop_get_sysinfo_s");
    if (!real)
        return NULL;
    info = real(server);
    name_processors((struct gtop_sysinfo *) info);
    return info;
}

/* ---------- 9. polkitd, which cannot switch users in the chroot mode ---------- */

struct passwd *getpwnam(const char *name)
{
    static struct passwd *(*real)(const char *);
    static struct passwd same;
    struct passwd *pw;

    if (!real)
        real = dlsym(RTLD_NEXT, "getpwnam");
    pw = real ? real(name) : NULL;
    if (!pw || strcmp(name, "polkitd") != 0 || strcmp(program_invocation_short_name, "polkitd") != 0
        || getuid() == 0 || getuid() == pw->pw_uid)
        return pw;
    same = *pw;
    same.pw_uid = getuid();
    same.pw_gid = getgid();
    return &same;
}

/* ---------- 10. credentials that fakeroot made up ---------- */

ssize_t sendmsg(int fd, const struct msghdr *msg, int flags)
{
    static ssize_t (*real)(int, const struct msghdr *, int);
    struct msghdr again;
    char control[256];
    int fixed = 0;
    ssize_t n;

    if (!real)
        real = dlsym(RTLD_NEXT, "sendmsg");
    if (!real) {
        errno = ENOSYS;
        return -1;
    }
    n = real(fd, msg, flags);
    if (n >= 0 || errno != EPERM || !msg || !msg->msg_control || msg->msg_controllen > sizeof control)
        return n;
    again = *msg;
    memcpy(control, msg->msg_control, msg->msg_controllen);
    again.msg_control = control;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&again); c; c = CMSG_NXTHDR(&again, c)) {
        struct ucred *cred = (struct ucred *) CMSG_DATA(c);
        /* the ids of the kernel, not the ones fakeroot answers getuid() with */
        uid_t uid = syscall(SYS_getuid);
        gid_t gid = syscall(SYS_getgid);

        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_CREDENTIALS || c->cmsg_len < CMSG_LEN(sizeof *cred)
            || (cred->uid == uid && cred->gid == gid))
            continue;
        cred->uid = uid;
        cred->gid = gid;
        fixed = 1;
    }
    if (!fixed) {
        errno = EPERM;
        return n;
    }
    return real(fd, &again, flags);
}

/* ---------- 11. a long press on the empty part of a folder, in Files ---------- */

/* GTK and GObject are taken from the process (GTK programs only call what comes here) */
static struct {
    unsigned long (*type_from_name)(const char *);
    int (*type_is_a)(unsigned long, unsigned long);
    unsigned long (*click_type)(void);
    void *(*long_press_new)(void);
    void (*touch_only)(void *, int);
    unsigned long (*connect)(void *, const char *, void (*)(void), void *, void *, int);
    void *(*pick)(void *, double, double, int);
    void *(*parent)(void *);
    int (*claim)(void *, int);
    void (*emit)(void *, const char *, ...);
} gtk;

/* G_OBJECT_TYPE(): an instance starts with its class, and a class with its type */
static unsigned long type_of(void *instance)
{
    return **(unsigned long **) instance;
}

static int is_a(void *instance, const char *name)
{
    unsigned long type = gtk.type_from_name(name);

    return type && gtk.type_is_a(type_of(instance), type);
}

static void folder_long_press(void *gesture, double x, double y, void *view)
{
    for (void *w = gtk.pick(view, x, y, 0 /* GTK_PICK_DEFAULT */); w && w != view; w = gtk.parent(w))
        if (is_a(w, "NautilusViewCell") || is_a(w, "GtkColumnViewTitle"))
            return; /* a file, with a long press of its own, or the head of a column */
    /* the press itself has already emptied the selection and focused the view, as for a right click */
    gtk.claim(gesture, 1 /* GTK_EVENT_SEQUENCE_CLAIMED */);
    gtk.emit(view, "popup-background-context-menu", x, y);
}

void gtk_widget_add_controller(void *widget, void *controller)
{
    static void (*real)(void *, void *);
    static int files = -1;

    if (!real)
        real = dlsym(RTLD_NEXT, "gtk_widget_add_controller");
    if (!real)
        return;
    real(widget, controller);
    if (files < 0) {
        files = strcmp(program_invocation_short_name, "nautilus") == 0;
        gtk.type_from_name = dlsym(RTLD_DEFAULT, "g_type_from_name");
        gtk.type_is_a = dlsym(RTLD_DEFAULT, "g_type_is_a");
        gtk.click_type = dlsym(RTLD_DEFAULT, "gtk_gesture_click_get_type");
        gtk.long_press_new = dlsym(RTLD_DEFAULT, "gtk_gesture_long_press_new");
        gtk.touch_only = dlsym(RTLD_DEFAULT, "gtk_gesture_single_set_touch_only");
        gtk.connect = dlsym(RTLD_DEFAULT, "g_signal_connect_data");
        gtk.pick = dlsym(RTLD_DEFAULT, "gtk_widget_pick");
        gtk.parent = dlsym(RTLD_DEFAULT, "gtk_widget_get_parent");
        gtk.claim = dlsym(RTLD_DEFAULT, "gtk_gesture_set_state");
        gtk.emit = dlsym(RTLD_DEFAULT, "g_signal_emit_by_name");
        for (void **f = (void **) &gtk; f < (void **) (&gtk + 1); f++)
            if (!*f)
                files = 0;
    }
    /* the click gesture of a folder view, the one that opens its menu on a right click */
    if (!files || !widget || !controller || !is_a(widget, "NautilusListBase")
        || !gtk.type_is_a(type_of(controller), gtk.click_type()))
        return;
    void *press = gtk.long_press_new();
    gtk.touch_only(press, 1);
    gtk.connect(press, "pressed", (void (*)(void)) folder_long_press, widget, NULL, 0);
    real(widget, press);
}
