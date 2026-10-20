/*
 * Lifecycle tests for the rocm-ernic server: startup, misbehaving clients,
 * and signal-driven shutdown.
 *
 * Each row starts the real server binary, mostly on the loopback backend,
 * does something to it (nothing, a client that hangs up, a client that sends
 * a bad header, a client that never speaks), signals it, and checks the exit
 * status and the log.  Six things are pinned here:
 *
 *   - only the socket's owner, and the group --socket-group names, can
 *     connect to it, whatever the umask or a default ACL on its directory;
 *   - at startup, the server removes a stale socket at its path, and leaves
 *     anything else there alone and refuses to start;
 *   - a client that drops out during vfio-user negotiation no longer takes
 *     the server down: it logs the drop and waits for the next client;
 *   - a clean shutdown frees the ionic layer.  When the server itself is
 *     built with -DERNIC_USE_SANITIZERS=ON, LeakSanitizer runs at its exit
 *     and any report fails the row;
 *   - on the TCP backend, SIGTERM stops the backend's threads promptly
 *     rather than waiting out a blocking accept() or the health-check
 *     interval;
 *   - a startup failure after the backend has started its threads stops
 *     and joins them before the server exits.
 *
 * Usage: test_server_lifecycle
 *
 * The server it runs is the one built with it, whose path the build compiles
 * in as ERNIC_SERVER_PATH.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

#define SOCKET_WAIT_MS 20000 /* sanitizer builds start slowly */
#define EXIT_WAIT_MS   20000
#define LOG_WAIT_MS    5000
#define SETTLE_MS      300

#define DROPPED_LINE  "client dropped during negotiation"
#define SHUTDOWN_LINE "Shutdown complete"
#define STALE_LINE    "Removed stale socket file"

/* What a row leaves in a regular file at the socket's path. */
#define KEEP_CONTENT "not a socket\n"

/* ---- Helpers ----------------------------------------------------------- */

static void sleep_ms(uint32_t ms)
{
    struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000};

    while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
        ;
}

static int64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool path_is_socket(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 && S_ISSOCK(st.st_mode);
}

/* Store path's permission bits in *mode and its group in *gid, without
 * following a symlink; false if it can't be lstat()ed. */
static bool path_perms(const char *path, uint32_t *mode, uint32_t *gid)
{
    struct stat st;

    if (lstat(path, &st) != 0)
        return false;
    *mode = (uint32_t)(st.st_mode & 07777);
    *gid = (uint32_t)st.st_gid;
    return true;
}

/* A group this process is in: another than its own if it has one, so that
 * giving the socket that group changes something. */
static gid_t my_other_group(void)
{
    gid_t groups[256];
    int n = getgroups((int)(sizeof(groups) / sizeof(groups[0])), groups);

    for (int i = 0; i < n; i++)
        if (groups[i] != getegid())
            return groups[i];
    return getegid();
}

/* Whether this process is in group @gid; true if it can't tell. */
static bool in_group(gid_t gid)
{
    gid_t groups[256];
    int n = getgroups((int)(sizeof(groups) / sizeof(groups[0])), groups);

    if (n < 0 || gid == getegid())
        return true;
    for (int i = 0; i < n; i++)
        if (groups[i] == gid)
            return true;
    return false;
}

/* A group this process isn't in, so it can't give a file that group. */
static bool foreign_group(gid_t *gid)
{
    for (gid_t g = 0; g < 65536; g++) {
        if (!in_group(g)) {
            *gid = g;
            return true;
        }
    }
    return false;
}

/* Store the name of group @gid in @buf, or its number if it has none. */
static void group_name(gid_t gid, char *buf, size_t len)
{
    char grbuf[4096];
    struct group grp, *found = NULL;

    if (getgrgid_r(gid, &grp, grbuf, sizeof(grbuf), &found) == 0 && found)
        snprintf(buf, len, "%s", found->gr_name);
    else
        snprintf(buf, len, "%u", (unsigned)gid);
}

/*
 * POSIX ACLs, as the kernel stores them in an extended attribute: a 32-bit
 * version, then 8 bytes per entry (16-bit tag, 16-bit permissions, 32-bit
 * id), all little-endian.  The kernel's <linux/posix_acl_xattr.h> describes
 * it, but kernel headers before 6.2 don't install that.
 */
#define ACL_XATTR_VERSION 2
#define ACL_USER_OBJ      0x01
#define ACL_USER          0x02
#define ACL_GROUP_OBJ     0x04
#define ACL_MASK          0x10
#define ACL_OTHER         0x20
#define ACL_RW            0x06
#define ACL_NO_ID         UINT32_MAX
#define NOBODY_UID        65534

/* A default ACL for a scratch directory, which is copied onto what's created
 * in it. */
enum dir_acl {
    NO_ACL,    /* none: the scratch directory itself */
    ACL_NAMED, /* owner and group rw, and user nobody rw, which no mode
                  shows */
    ACL_OPEN,  /* owner, group, and other rw, only the base entries */
    ACL_TIGHT, /* owner rw and nothing for anyone else, only the base
                  entries */
};

static uint8_t *put_le(uint8_t *p, uint32_t value, size_t bytes)
{
    for (size_t i = 0; i < bytes; i++)
        *p++ = (uint8_t)(value >> (8 * i));
    return p;
}

static uint8_t *put_acl_entry(uint8_t *p, uint32_t tag, uint32_t perm,
                              uint32_t id)
{
    p = put_le(p, tag, 2);
    p = put_le(p, perm, 2);
    return put_le(p, id, 4);
}

/* Make dir with the default ACL @kind; false if it can't. */
static bool make_acl_dir(const char *dir, enum dir_acl kind)
{
    uint8_t acl[4 + 5 * 8], *p = acl;

    if (mkdir(dir, 0700) < 0)
        return false;
    memset(acl, 0, sizeof(acl));
    p = put_le(p, ACL_XATTR_VERSION, 4);
    /* In the order the kernel requires: by tag, then by id. */
    p = put_acl_entry(p, ACL_USER_OBJ, ACL_RW, ACL_NO_ID);
    if (kind == ACL_NAMED)
        p = put_acl_entry(p, ACL_USER, ACL_RW, NOBODY_UID);
    p = put_acl_entry(p, ACL_GROUP_OBJ, kind == ACL_TIGHT ? 0 : ACL_RW,
                      ACL_NO_ID);
    if (kind == ACL_NAMED)
        p = put_acl_entry(p, ACL_MASK, ACL_RW, ACL_NO_ID);
    p = put_acl_entry(p, ACL_OTHER, kind == ACL_OPEN ? ACL_RW : 0, ACL_NO_ID);
    return setxattr(dir, "system.posix_acl_default", acl, (size_t)(p - acl),
                    0) == 0;
}

static int connect_unix(const char *path)
{
    struct sockaddr_un addr;
    size_t len = strlen(path);
    int fd;

    /* A path that does not fit sun_path cannot be connected to. */
    if (len >= sizeof(addr.sun_path))
        return -1;
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, len + 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Leave a socket at @path that nothing listens on, as a server that didn't
 * exit cleanly does.  False if it can't. */
static bool make_stale_socket(const char *path)
{
    struct sockaddr_un addr;
    size_t len = strlen(path);
    bool ok;
    int fd;

    if (len >= sizeof(addr.sun_path))
        return false;
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return false;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, len + 1);
    ok = bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0;
    close(fd);
    return ok;
}

/* Create a regular file at @path holding @content.  False if it can't. */
static bool write_file(const char *path, const char *content)
{
    size_t len = strlen(content);
    /* With _FORTIFY_SOURCE, glibc 2.40 and later give Clang an
     * open(path, flags, mode) overload, and android-cloexec-open looks for
     * O_CLOEXEC in its mode */
    /* NOLINTBEGIN(android-cloexec-open) */
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    /* NOLINTEND(android-cloexec-open) */
    bool ok;

    if (fd < 0)
        return false;
    ok = write(fd, content, len) == (ssize_t)len;
    return close(fd) == 0 && ok;
}

/*
 * A listening TCP socket on an ephemeral port, on every address as the TCP
 * manager binds.  Its port is stored in *port.  Closing it frees the port
 * for the server; keeping it open gives a worker a "manager" that accepts
 * its connection (in the kernel's backlog) and never answers.
 */
static int listen_ephemeral(uint16_t *port)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0)
        return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(fd, 4) < 0 ||
        getsockname(fd, (struct sockaddr *)&addr, &len) < 0) {
        close(fd);
        return -1;
    }
    *port = ntohs(addr.sin_port);
    return fd;
}

/* Read the whole log (bounded) into a NUL-terminated heap buffer. */
static char *read_log(const char *path)
{
    FILE *f = fopen(path, "re");
    char *buf;
    size_t n;

    if (!f)
        return NULL;
    buf = calloc(1, 1 << 20);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    n = fread(buf, 1, (1 << 20) - 1, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static int count_in_log(const char *path, const char *needle)
{
    char *log = read_log(path);
    int n = 0;

    if (!log)
        return 0;
    for (const char *p = log; (p = strstr(p, needle)) != NULL;
         p += strlen(needle))
        n++;
    free(log);
    return n;
}

static bool wait_for_log(const char *path, const char *needle, int count)
{
    int64_t deadline = now_ms() + LOG_WAIT_MS;

    while (now_ms() < deadline) {
        if (count_in_log(path, needle) >= count)
            return true;
        sleep_ms(20);
    }
    return false;
}

/* Any sanitizer report in the server's output. */
static bool sanitizer_reported(const char *path)
{
    static const char *const markers[] = {
        "ERROR: LeakSanitizer",
        "ERROR: AddressSanitizer",
        "WARNING: ThreadSanitizer",
        "runtime error:",
    };

    for (size_t i = 0; i < sizeof(markers) / sizeof(markers[0]); i++)
        if (count_in_log(path, markers[i]) > 0)
            return true;
    return false;
}

/* waitpid() with a deadline; the server is SIGKILLed if it overstays. */
static int wait_exit(pid_t pid, int *status)
{
    int64_t deadline = now_ms() + EXIT_WAIT_MS;

    while (now_ms() < deadline) {
        pid_t r = waitpid(pid, status, WNOHANG);

        if (r == pid)
            return 0;
        if (r < 0 && errno != EINTR)
            return -1;
        sleep_ms(20);
    }
    kill(pid, SIGKILL);
    waitpid(pid, status, 0);
    return -1;
}

/* ---- Client actions ---------------------------------------------------- */

enum action {
    IDLE,    /* no client at all */
    HANG_UP, /* connect, then close without a word */
    BAD_HDR, /* send a header that is not VFIO_USER_VERSION, then close */
    SILENT,  /* connect and stay connected, saying nothing */
};

/*
 * A vfio-user message header (struct vfio_user_header): msg_id, cmd,
 * msg_size, flags, error_no.  cmd 2 is not VFIO_USER_VERSION (1), so the
 * server's version negotiation rejects it with EINVAL.
 */
static int send_bad_header(int fd)
{
    const uint8_t hdr[16] = {0x01, 0x00, 0x02, 0x00, 0x10, 0x00, 0x00, 0x00,
                             0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

    return send(fd, hdr, sizeof(hdr), MSG_NOSIGNAL) == (ssize_t)sizeof(hdr)
               ? 0
               : -1;
}

/* ---- Tests ------------------------------------------------------------- */

/* What to pass as --socket-group. */
enum sock_group {
    SG_NONE,        /* nothing: the socket is the owner's alone, 0600 */
    SG_MINE_NAME,   /* the name of a group this process is in: 0660 */
    SG_MINE_NUMBER, /* the same, as a number */
    SG_UNKNOWN,     /* a name no group has */
    SG_FOREIGN,     /* the number of a group this process isn't in */
    SG_HUGE,        /* a gid above INT_MAX this process isn't in */
    SG_NO_CHANGE,   /* (gid_t)-1, which tells chown() to change nothing */
};

/* What's at the socket's path, or on the way to it, as the server starts. */
enum sock_pre {
    PRE_NONE,      /* nothing at the path */
    PRE_STALE,     /* a socket nothing listens on, as a server that didn't
                      exit cleanly leaves */
    PRE_FILE,      /* a regular file */
    PRE_SYMLINK,   /* a symlink to a stale socket */
    PRE_NO_DIR,    /* the path is in a directory that doesn't exist */
    PRE_NO_SEARCH, /* the path is in a directory the server can read but not
                      search */
    PRE_NO_READ,   /* the path is in a directory the server can write and
                      search but not read */
    PRE_DIR_SLASH, /* the path is a directory's, ending in '/' */
};

struct lifecycle_case {
    const char *category;
    const char *description;
    const char *expected;
    /* --backend value: @DIR@ is the scratch dir, @PORT@ a free TCP port */
    const char *backend;
    const char *env_name; /* an environment variable for the server, or NULL */
    const char *env_value;
    bool fake_manager; /* hold @PORT@ open as a manager that never answers */
    bool set_umask;    /* run the server under umask, not the inherited one */
    mode_t umask;      /* the umask for set_umask */
    enum dir_acl acl;  /* put the socket in a directory with this ACL */
    enum sock_pre pre; /* what's at the socket's path beforehand */
    enum sock_group group;
    const char *opt;     /* another option for the server, or NULL */
    const char *opt_arg; /* its argument, or NULL for none */
    bool want_socket;    /* the socket should appear, mode 0600, or 0660 and the
                            group asked for */
    bool want_gone;      /* refused: no socket left once the server exits */
    bool settle;         /* let the server reach its attach loop first */
    enum action action;
    int clients;           /* how many times to do the action */
    int sig;               /* signal to send (0: none, wait for exit) */
    int sigs;              /* how many times to send it */
    int want_exit;         /* exit status; -1 means any non-zero */
    int want_dropped;      /* DROPPED_LINE count, checked before the signal */
    int want_shutdown;     /* SHUTDOWN_LINE count after exit */
    const char *want_line; /* another line the log must contain, or NULL */
};

static const struct lifecycle_case cases[] = {
    {.category = "positive",
     .description = "SIGTERM with no client",
     .expected = "exits 0 after one 'Shutdown complete', no sanitizer report",
     .backend = "loopback",
     .want_socket = true,
     .settle = true,
     .action = IDLE,
     .clients = 0,
     .sig = SIGTERM,
     .sigs = 1,
     .want_exit = 0,
     .want_dropped = 0,
     .want_shutdown = 1},
    {.category = "positive",
     .description = "SIGINT with no client",
     .expected = "the same clean shutdown as SIGTERM",
     .backend = "loopback",
     .want_socket = true,
     .settle = true,
     .action = IDLE,
     .clients = 0,
     .sig = SIGINT,
     .sigs = 1,
     .want_exit = 0,
     .want_dropped = 0,
     .want_shutdown = 1},
    {.category = "positive",
     .description = "nvmeof backend with a file-backed namespace, then SIGTERM",
     .expected = "a clean shutdown that also frees the parsed namespace path",
     .backend = "nvmeof:size=1M,file=@DIR@/ns.img",
     .want_socket = true,
     .settle = true,
     .action = IDLE,
     .clients = 0,
     .sig = SIGTERM,
     .sigs = 1,
     .want_exit = 0,
     .want_dropped = 0,
     .want_shutdown = 1},
    /* The interval is longer than EXIT_WAIT_MS, so a health-check thread
     * that sleeps it out instead of being woken fails the row. */
    {.category = "positive",
     .description = "TCP mesh manager with a 60 s health-check interval, then "
                    "SIGTERM",
     .expected = "exits 0 without waiting out the interval or a blocked "
                 "accept()",
     .backend = "tcp:manager:listen:@PORT@",
     .env_name = "ERNIC_TCP_HEALTH_INTERVAL",
     .env_value = "60",
     .want_socket = true,
     .settle = true,
     .action = IDLE,
     .clients = 0,
     .sig = SIGTERM,
     .sigs = 1,
     .want_exit = 0,
     .want_dropped = 0,
     .want_shutdown = 1},
    {.category = "negative",
     .description = "a client connects and hangs up before negotiating",
     .expected = "logs the drop, keeps listening, then shuts down cleanly",
     .backend = "loopback",
     .want_socket = true,
     .settle = true,
     .action = HANG_UP,
     .clients = 1,
     .sig = SIGTERM,
     .sigs = 1,
     .want_exit = 0,
     .want_dropped = 1,
     .want_shutdown = 1,
     .want_line = "No message of desired type"},
    {.category = "negative",
     .description = "a client sends a non-VERSION header and hangs up",
     .expected =
         "negotiation fails with EINVAL, logged as a drop; keeps listening",
     .backend = "loopback",
     .want_socket = true,
     .settle = true,
     .action = BAD_HDR,
     .clients = 1,
     .sig = SIGTERM,
     .sigs = 1,
     .want_exit = 0,
     .want_dropped = 1,
     .want_shutdown = 1,
     .want_line = "Invalid argument"},
    {.category = "negative",
     .description = "an nvmeof namespace file that cannot be opened",
     .expected = "ionic init unwinds and the server exits non-zero, no "
                 "sanitizer report",
     .backend = "nvmeof:file=/nonexistent/rocm-ernic-ns.img",
     .want_socket = false,
     .settle = false,
     .action = IDLE,
     .clients = 0,
     .sig = 0,
     .sigs = 0,
     .want_exit = -1,
     .want_dropped = 0,
     .want_shutdown = 0,
     .want_line = "nvmeof backend: cannot open"},
    {.category = "negative",
     .description = "TCP mesh worker whose manager accepts it and never "
                    "answers",
     .expected = "registration times out after 5 s, the backend unwinds its "
                 "threads, and the server exits non-zero",
     .backend = "tcp:worker:127.0.0.1:@PORT@",
     .fake_manager = true,
     .want_socket = false,
     .settle = false,
     .action = IDLE,
     .clients = 0,
     .sig = 0,
     .sigs = 0,
     .want_exit = -1,
     .want_dropped = 0,
     .want_shutdown = 0,
     .want_line = "Registration timeout"},
    /* Attaching the TAP comes after the backend has started its threads.
     * '/' is never valid in an interface name, so the attach fails whether
     * or not the test can create TAP devices.  "Cleaning up" is logged at
     * info level, by tcp_fini(), so it shows that the threads were stopped
     * and joined before the process exited. */
    {.category = "negative",
     .description = "a TAP that can't be attached, on a TCP mesh manager",
     .expected = "the backend's threads are stopped, the socket is removed, "
                 "and the server exits non-zero, with no sanitizer report",
     .backend = "tcp:manager:listen:@PORT@",
     .env_name = "ERNIC_LOG_LEVEL",
     .env_value = "info",
     .opt = "--tap",
     .opt_arg = "ernic/bad",
     .want_socket = false,
     .want_gone = true,
     .settle = false,
     .action = IDLE,
     .clients = 0,
     .sig = 0,
     .sigs = 0,
     .want_exit = -1,
     .want_dropped = 0,
     .want_shutdown = 0,
     .want_line = "TCP backend: Cleaning up"},
    /* Bad options are refused before the socket exists.  Under LeakSanitizer
     * these also check that the refusal frees what parsing allocated. */
    {.category = "negative",
     .description = "--log-level names no level",
     .expected = "the server exits non-zero before creating the socket",
     .backend = "loopback",
     .opt = "--log-level",
     .opt_arg = "loud",
     .want_socket = false,
     .want_gone = true,
     .settle = false,
     .action = IDLE,
     .clients = 0,
     .sig = 0,
     .sigs = 0,
     .want_exit = -1,
     .want_dropped = 0,
     .want_shutdown = 0,
     .want_line = "Invalid log level"},
    {.category = "negative",
     .description = "--mac has five bytes",
     .expected = "the server exits non-zero before creating the socket",
     .backend = "loopback",
     .opt = "--mac",
     .opt_arg = "02:00:00:00:00",
     .want_socket = false,
     .want_gone = true,
     .settle = false,
     .action = IDLE,
     .clients = 0,
     .sig = 0,
     .sigs = 0,
     .want_exit = -1,
     .want_dropped = 0,
     .want_shutdown = 0,
     .want_line = "Invalid MAC address format"},
    {.category = "negative",
     .description = "an option the server doesn't know",
     .expected = "the server prints its usage and exits non-zero before "
                 "creating the socket",
     .backend = "loopback",
     .opt = "--no-such-option",
     .want_socket = false,
     .want_gone = true,
     .settle = false,
     .action = IDLE,
     .clients = 0,
     .sig = 0,
     .sigs = 0,
     .want_exit = -1,
     .want_dropped = 0,
     .want_shutdown = 0,
     .want_line = "Usage:"},
    /* A default ACL on the directory is copied onto the socket, and a named
     * entry in it lets that user connect whatever the mode says, so the
     * server refuses to serve the socket.  It checks before --socket-group
     * widens the mode, and with it the ACL mask, which would let the entry
     * take effect. */
    {.category = "negative",
     .description = "the socket's directory has a default ACL that also "
                    "grants another user rw, with --socket-group",
     .expected = "the socket has an ACL entry no mode shows; the server "
                 "refuses to serve it, removes it, and exits non-zero",
     .backend = "loopback",
     .acl = ACL_NAMED,
     .group = SG_MINE_NAME,
     .want_socket = false,
     .want_gone = true,
     .settle = false,
     .action = IDLE,
     .clients = 0,
     .sig = 0,
     .sigs = 0,
     .want_exit = -1,
     .want_dropped = 0,
     .want_shutdown = 0,
     .want_line = "has an ACL"},
    {.category = "negative",
     .description = "--socket-group names no group",
     .expected = "the server exits non-zero before creating the socket",
     .backend = "loopback",
     .group = SG_UNKNOWN,
     .want_socket = false,
     .want_gone = true,
     .settle = false,
     .action = IDLE,
     .clients = 0,
     .sig = 0,
     .sigs = 0,
     .want_exit = -1,
     .want_dropped = 0,
     .want_shutdown = 0,
     .want_line = "no such group"},
    {.category = "negative",
     .description = "--socket-group is a group the server isn't in",
     .expected = "it can't give the socket that group, so it removes the "
                 "socket and exits non-zero",
     .backend = "loopback",
     .group = SG_FOREIGN,
     .want_socket = false,
     .want_gone = true,
     .settle = false,
     .action = IDLE,
     .clients = 0,
     .sig = 0,
     .sigs = 0,
     .want_exit = -1,
     .want_dropped = 0,
     .want_shutdown = 0,
     .want_line = "be a member of that group"},
    /* gid_t is unsigned 32 bits, so a gid above INT_MAX is valid.  Reaching
     * the lchown() at all shows the number was taken as a gid. */
    {.category = "boundary",
     .description = "--socket-group is 3000000000, a gid above INT_MAX the "
                    "server isn't in",
     .expected = "it's taken as a gid, which the server can't give the "
                 "socket, so it removes the socket and exits non-zero",
     .backend = "loopback",
     .group = SG_HUGE,
     .want_socket = false,
     .want_gone = true,
     .settle = false,
     .action = IDLE,
     .clients = 0,
     .sig = 0,
     .sigs = 0,
     .want_exit = -1,
     .want_dropped = 0,
     .want_shutdown = 0,
     .want_line = "be a member of that group"},
    {.category = "boundary",
     .description = "--socket-group is 4294967295, which is (gid_t)-1",
     .expected = "it's no gid, since chown() takes it to mean no change: the "
                 "server exits non-zero before creating the socket",
     .backend = "loopback",
     .group = SG_NO_CHANGE,
     .want_socket = false,
     .want_gone = true,
     .settle = false,
     .action = IDLE,
     .clients = 0,
     .sig = 0,
     .sigs = 0,
     .want_exit = -1,
     .want_dropped = 0,
     .want_shutdown = 0,
     .want_line = "no such group"},
    /* What's at the socket's path beforehand.  The server removes only a
     * socket, and the client's drop shows that the one it then serves is its
     * own: connecting to the stale one would be refused. */
    {.category = "positive",
     .description = "a stale socket is at the socket's path",
     .expected = "the server removes it, serves a socket of its own, then "
                 "shuts down cleanly",
     .backend = "loopback",
     .pre = PRE_STALE,
     .want_socket = true,
     .settle = true,
     .action = HANG_UP,
     .clients = 1,
     .sig = SIGTERM,
     .sigs = 1,
     .want_exit = 0,
     .want_dropped = 1,
     .want_shutdown = 1,
     .want_line = STALE_LINE},
    {.category = "negative",
     .description = "a regular file is at the socket's path",
     .expected = "the server leaves the file alone and exits non-zero",
     .backend = "loopback",
     .pre = PRE_FILE,
     .want_socket = false,
     .settle = false,
     .action = IDLE,
     .clients = 0,
     .sig = 0,
     .sigs = 0,
     .want_exit = -1,
     .want_dropped = 0,
     .want_shutdown = 0,
     .want_line = "isn't a socket; not removing it"},
    {.category = "negative",
     .description = "a symlink to a stale socket is at the socket's path",
     .expected = "the server leaves the symlink and the socket alone and "
                 "exits non-zero",
     .backend = "loopback",
     .pre = PRE_SYMLINK,
     .want_socket = false,
     .settle = false,
     .action = IDLE,
     .clients = 0,
     .sig = 0,
     .sigs = 0,
     .want_exit = -1,
     .want_dropped = 0,
     .want_shutdown = 0,
     .want_line = "isn't a socket; not removing it"},
    {.category = "negative",
     .description = "the socket's directory doesn't exist",
     .expected = "the server can't open the directory, so it exits non-zero",
     .backend = "loopback",
     .pre = PRE_NO_DIR,
     .want_socket = false,
     .want_gone = true,
     .settle = false,
     .action = IDLE,
     .clients = 0,
     .sig = 0,
     .sigs = 0,
     .want_exit = -1,
     .want_dropped = 0,
     .want_shutdown = 0,
     .want_line = "cannot open socket directory"},
    {.category = "negative",
     .description = "the socket's directory can be read but not searched",
     .expected = "the server can't check the path, so it exits non-zero",
     .backend = "loopback",
     .pre = PRE_NO_SEARCH,
     .want_socket = false,
     .settle = false,
     .action = IDLE,
     .clients = 0,
     .sig = 0,
     .sigs = 0,
     .want_exit = -1,
     .want_dropped = 0,
     .want_shutdown = 0,
     .want_line = "cannot check"},
    /* Binding the socket needs no read permission on its directory, so
     * neither may the check for a stale one. */
    {.category = "boundary",
     .description = "the socket's directory can be written and searched but "
                    "not read",
     .expected = "the server serves the socket, then shuts down cleanly",
     .backend = "loopback",
     .pre = PRE_NO_READ,
     .want_socket = true,
     .settle = true,
     .action = IDLE,
     .clients = 0,
     .sig = SIGTERM,
     .sigs = 1,
     .want_exit = 0,
     .want_dropped = 0,
     .want_shutdown = 1},
    {.category = "boundary",
     .description = "the socket's path is a directory's, ending in '/'",
     .expected = "it names no file, so the server exits non-zero",
     .backend = "loopback",
     .pre = PRE_DIR_SLASH,
     .want_socket = false,
     .settle = false,
     .action = IDLE,
     .clients = 0,
     .sig = 0,
     .sigs = 0,
     .want_exit = -1,
     .want_dropped = 0,
     .want_shutdown = 0,
     .want_line = "has no file name"},
    /* Every row that wants a socket checks that it's mode 0600, or 0660 and
     * the group asked for, as soon as it appears and again once the server
     * is up.  The server sets all of it before the socket can be served. */
    {.category = "positive",
     .description = "--socket-group names a group the server is in",
     .expected = "the socket is 0660 with that group, served, then a clean "
                 "shutdown",
     .backend = "loopback",
     .group = SG_MINE_NAME,
     .want_socket = true,
     .settle = true,
     .action = IDLE,
     .clients = 0,
     .sig = SIGTERM,
     .sigs = 1,
     .want_exit = 0,
     .want_dropped = 0,
     .want_shutdown = 1},
    /* The kernel applies the umask to a UNIX socket before a default ACL,
     * which can only take bits away. */
    {.category = "boundary",
     .description = "the socket's directory has a default ACL that grants "
                    "everyone rw, and no named entries",
     .expected = "the socket is still 0600, and the server serves it and "
                 "shuts down cleanly",
     .backend = "loopback",
     .acl = ACL_OPEN,
     .want_socket = true,
     .settle = true,
     .action = IDLE,
     .clients = 0,
     .sig = SIGTERM,
     .sigs = 1,
     .want_exit = 0,
     .want_dropped = 0,
     .want_shutdown = 1},
    /* That ACL takes away what --socket-group then gives back. */
    {.category = "boundary",
     .description = "the socket's directory has a default ACL that grants "
                    "the group nothing, with --socket-group",
     .expected = "the socket is still 0660 with the group asked for, so the "
                 "group can connect",
     .backend = "loopback",
     .acl = ACL_TIGHT,
     .group = SG_MINE_NAME,
     .want_socket = true,
     .settle = true,
     .action = IDLE,
     .clients = 0,
     .sig = SIGTERM,
     .sigs = 1,
     .want_exit = 0,
     .want_dropped = 0,
     .want_shutdown = 1},
    {.category = "boundary",
     .description = "SIGTERM as soon as the socket appears",
     .expected = "a clean shutdown even before the attach loop is reached",
     .backend = "loopback",
     .want_socket = true,
     .settle = false,
     .action = IDLE,
     .clients = 0,
     .sig = SIGTERM,
     .sigs = 1,
     .want_exit = 0,
     .want_dropped = 0,
     .want_shutdown = 1},
    {.category = "boundary",
     .description = "the server inherits umask 000",
     .expected = "the socket is 0600 from the moment it appears, not 0777",
     .backend = "loopback",
     .set_umask = true,
     .umask = 0,
     .want_socket = true,
     .settle = true,
     .action = IDLE,
     .clients = 0,
     .sig = SIGTERM,
     .sigs = 1,
     .want_exit = 0,
     .want_dropped = 0,
     .want_shutdown = 1},
    {.category = "boundary",
     .description = "the server inherits umask 077, with --socket-group as a "
                    "number",
     .expected = "the socket is still 0660 with that group, so the group "
                 "can connect",
     .backend = "loopback",
     .set_umask = true,
     .umask = 077,
     .group = SG_MINE_NUMBER,
     .want_socket = true,
     .settle = true,
     .action = IDLE,
     .clients = 0,
     .sig = SIGTERM,
     .sigs = 1,
     .want_exit = 0,
     .want_dropped = 0,
     .want_shutdown = 1},
    {.category = "boundary",
     .description = "three clients hang up in a row",
     .expected = "three drops logged, still listening, then a clean shutdown",
     .backend = "loopback",
     .want_socket = true,
     .settle = true,
     .action = HANG_UP,
     .clients = 3,
     .sig = SIGTERM,
     .sigs = 1,
     .want_exit = 0,
     .want_dropped = 3,
     .want_shutdown = 1},
    {.category = "corner",
     .description = "a client connects and never speaks, then SIGTERM",
     .expected =
         "the signal interrupts the blocked negotiation: clean shutdown",
     .backend = "loopback",
     .want_socket = true,
     .settle = true,
     .action = SILENT,
     .clients = 1,
     .sig = SIGTERM,
     .sigs = 1,
     .want_exit = 0,
     .want_dropped = 0,
     .want_shutdown = 1},
    {.category = "corner",
     .description = "SIGTERM sent twice",
     .expected = "still exits 0 with exactly one 'Shutdown complete'",
     .backend = "loopback",
     .want_socket = true,
     .settle = true,
     .action = IDLE,
     .clients = 0,
     .sig = SIGTERM,
     .sigs = 2,
     .want_exit = 0,
     .want_dropped = 0,
     .want_shutdown = 1},
};

#ifndef ERNIC_SERVER_PATH
#error "ERNIC_SERVER_PATH must be the path of the server binary"
#endif

static const char *const g_server = ERNIC_SERVER_PATH;

/*
 * The scratch directory.  It's always under /tmp, not $TMPDIR: the server's
 * socket goes in it, and a socket path has to fit the 108 bytes of sun_path.
 */
static char g_dir[] = "/tmp/ernic-lifecycle-XXXXXX";

/*
 * Copy src to dst with each "@DIR@" replaced by the scratch directory and
 * each "@PORT@" by port.  Returns false if the result doesn't fit.
 */
static bool expand_backend(const char *src, uint16_t port, char *dst,
                           size_t len)
{
    char port_str[8];
    size_t n = 0;

    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);
    while (*src) {
        const char *sub = NULL;
        size_t skip = 1;

        if (strncmp(src, "@DIR@", 5) == 0) {
            sub = g_dir;
            skip = 5;
        } else if (strncmp(src, "@PORT@", 6) == 0) {
            sub = port_str;
            skip = 6;
        }
        size_t sl = sub ? strlen(sub) : 1;

        if (sl >= len - n)
            return false;
        memcpy(dst + n, sub ? sub : src, sl);
        n += sl;
        src += skip;
    }
    dst[n] = '\0';
    return true;
}

/*
 * The environment for the server: this process's, with name=value in place
 * of any name= already in it.  Its strings are this process's, apart from
 * the last, name=value, which is stored in *entry.  The caller frees both
 * the array and *entry.  Returns NULL if there's no memory.
 *
 * The new string is on the heap, not in a buffer of the caller's, so that
 * no stack address ends up in environ when the child installs the array.
 */
static char **server_env(const char *name, const char *value, char **entry)
{
    size_t name_len = strlen(name), value_len = strlen(value), n = 0, out = 0;
    char **env;

    *entry = NULL;
    if (name_len > SIZE_MAX - 2 - value_len) /* '=' and '\0' */
        return NULL;
    *entry = malloc(name_len + value_len + 2);
    if (!*entry)
        return NULL;
    memcpy(*entry, name, name_len);
    (*entry)[name_len] = '=';
    memcpy(*entry + name_len + 1, value, value_len + 1);
    while (environ[n])
        n++;
    env = calloc(n + 2, sizeof(*env));
    if (!env) {
        free(*entry);
        *entry = NULL;
        return NULL;
    }
    for (size_t i = 0; i < n; i++) {
        if (strncmp(environ[i], name, name_len) != 0 ||
            environ[i][name_len] != '=')
            env[out++] = environ[i];
    }
    env[out] = *entry;
    return env;
}

/* @group is the --socket-group to pass, or NULL for none. */
static pid_t start_server(const struct lifecycle_case *c, const char *backend,
                          const char *sock, const char *log, const char *group)
{
    char *entry = NULL;
    char **env = NULL;
    pid_t pid;

    /* Built here, since the child may only call async-signal-safe functions
     * until it execs, and setenv() and malloc() aren't */
    if (c->env_name) {
        env = server_env(c->env_name, c->env_value, &entry);
        if (!env)
            return -1;
    }

    pid = fork();
    if (pid == 0) {
        /* dup2() clears close-on-exec on the copies, so the server still
         * inherits stdout and stderr.  With _FORTIFY_SOURCE, glibc 2.40 and
         * later give Clang an open(path, flags, mode) overload, and
         * android-cloexec-open looks for O_CLOEXEC in its mode */
        /* NOLINTBEGIN(android-cloexec-open) */
        int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        /* NOLINTEND(android-cloexec-open) */

        if (fd < 0)
            _exit(126);
        if (env)
            environ = env;
        if (c->set_umask)
            umask(c->umask);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        close(fd);
        /* The options a row may add, packed to the front, since execl()
         * stops at the first NULL */
        const char *opt[4] = {NULL, NULL, NULL, NULL};
        size_t n = 0;

        if (group) {
            opt[n++] = "--socket-group";
            opt[n++] = group;
        }
        if (c->opt) {
            opt[n++] = c->opt;
            if (c->opt_arg)
                opt[n++] = c->opt_arg;
        }
        execl(g_server, g_server, "--socket", sock, "--backend", backend,
              opt[0], opt[1], opt[2], opt[3], (char *)NULL);
        _exit(127);
    }
    free(env);
    free(entry);
    return pid;
}

/*
 * Wait for the socket to appear, and if @after isn't NULL, for the line
 * @after in @log before it, so a socket already there doesn't count.  If the
 * server exits first, this reaps it, stores its status in *status, and sets
 * *exited.
 */
static bool wait_for_socket(pid_t pid, const char *sock, const char *log,
                            const char *after, int *status, bool *exited)
{
    int64_t deadline = now_ms() + SOCKET_WAIT_MS;

    *exited = false;
    while (now_ms() < deadline) {
        if ((!after || count_in_log(log, after) > 0) && path_is_socket(sock))
            return true;
        if (waitpid(pid, status, WNOHANG) == pid) {
            *exited = true;
            return false;
        }
        sleep_ms(10);
    }
    return false;
}

/*
 * Check that the socket is mode @want_mode and, if that's 0660, has group
 * @want_gid.  If not, says why in @why, mentioning @when.  With @early, the
 * server may not have let the group in yet, so 0600 will do too: it's
 * created that way, and only then given the group and widened.
 */
static bool socket_perms_ok(const char *sock, uint32_t want_mode,
                            gid_t want_gid, bool early, const char *when,
                            char *why, size_t len)
{
    uint32_t mode, gid;

    if (!path_perms(sock, &mode, &gid)) {
        snprintf(why, len, "could not stat the socket %s", when);
        return false;
    }
    if (early && mode == 0600)
        return true;
    if (mode != want_mode) {
        snprintf(why, len,
                 "the socket was mode %04" PRIo32 " %s, not %04" PRIo32, mode,
                 when, want_mode);
        return false;
    }
    if (want_mode == 0660 && gid != (uint32_t)want_gid) {
        snprintf(why, len, "the socket had group %" PRIu32 " %s, not %u", gid,
                 when, (unsigned)want_gid);
        return false;
    }
    return true;
}

/* Put what @pre asks for at @sock; a symlink points to @target.  False if it
 * can't. */
static bool place_at_path(enum sock_pre pre, const char *sock,
                          const char *target)
{
    switch (pre) {
    case PRE_STALE:
        return make_stale_socket(sock);
    case PRE_FILE:
        return write_file(sock, KEEP_CONTENT);
    case PRE_SYMLINK:
        return make_stale_socket(target) && symlink(target, sock) == 0;
    case PRE_NONE:
    case PRE_NO_DIR:
    case PRE_NO_SEARCH:
    case PRE_NO_READ:
    case PRE_DIR_SLASH:
    default:
        return true;
    }
}

/* Whether what place_at_path() put at @sock for @pre is still there as it
 * was.  The server must not remove anything but a socket. */
static bool left_alone(enum sock_pre pre, const char *sock, const char *target)
{
    struct stat st;
    char buf[320];
    ssize_t n;
    bool same;
    int fd;

    switch (pre) {
    case PRE_FILE:
        /* Checked through the descriptor that's read, without following a
         * symlink. */
        fd = open(sock, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0)
            return false;
        n = read(fd, buf, sizeof(buf));
        same = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
               n == (ssize_t)strlen(KEEP_CONTENT) &&
               memcmp(buf, KEEP_CONTENT, (size_t)n) == 0;
        close(fd);
        return same;
    case PRE_SYMLINK:
        if (lstat(sock, &st) != 0 || !S_ISLNK(st.st_mode))
            return false;
        n = readlink(sock, buf, sizeof(buf) - 1);
        if (n < 0)
            return false;
        buf[n] = '\0';
        return strcmp(buf, target) == 0 && path_is_socket(target);
    case PRE_NONE:
    case PRE_STALE:
    case PRE_NO_DIR:
    case PRE_NO_SEARCH:
    case PRE_NO_READ:
    case PRE_DIR_SLASH:
    default:
        return true;
    }
}

static bool exit_matches(int status, int want)
{
    if (!WIFEXITED(status))
        return false;
    if (want < 0)
        return WEXITSTATUS(status) != 0;
    return WEXITSTATUS(status) == want;
}

static void describe_status(int status, char *buf, size_t len)
{
    if (WIFEXITED(status))
        snprintf(buf, len, "exited %d", WEXITSTATUS(status));
    else if (WIFSIGNALED(status))
        snprintf(buf, len, "killed by signal %d", WTERMSIG(status));
    else
        snprintf(buf, len, "status %#x", (unsigned)status);
}

static int run_case(size_t idx, const struct lifecycle_case *c)
{
    char sock[320], log[320], backend[512], why[480] = "", got[64] = "";
    char dir[256]; /* shorter than sock, which adds a name to it */
    char target[320], group[64];
    int held[8], nheld = 0, status = 0, port_fd = -1;
    uint32_t want_mode = 0600;
    uint16_t port = 0;
    gid_t want_gid = 0;
    bool ok = true, exited = false, own_dir = false;
    pid_t pid;

    /* Root can search any directory, so there's nothing to refuse. */
    if (c->pre == PRE_NO_SEARCH && geteuid() == 0) {
        printf("ok   %s: %s; not applicable as root\n", c->category,
               c->description);
        return 0;
    }

    switch (c->group) {
    case SG_NONE:
    default:
        break;
    case SG_MINE_NAME:
        want_gid = my_other_group();
        group_name(want_gid, group, sizeof(group));
        want_mode = 0660;
        break;
    case SG_MINE_NUMBER:
        want_gid = my_other_group();
        snprintf(group, sizeof(group), "%u", (unsigned)want_gid);
        want_mode = 0660;
        break;
    case SG_UNKNOWN:
        snprintf(group, sizeof(group), "ernic-lifecycle-no-such-group");
        break;
    case SG_NO_CHANGE:
        snprintf(group, sizeof(group), "%" PRIu32, UINT32_MAX);
        break;
    case SG_HUGE:
        /* Root can give a file any group, so there's nothing to refuse. */
        if (geteuid() == 0) {
            printf("ok   %s: %s; not applicable as root\n", c->category,
                   c->description);
            return 0;
        }
        want_gid = (gid_t)UINT32_C(3000000000);
        if (in_group(want_gid)) {
            printf("FAIL %s: %s; this process is in group %u\n", c->category,
                   c->description, (unsigned)want_gid);
            return 1;
        }
        snprintf(group, sizeof(group), "%u", (unsigned)want_gid);
        break;
    case SG_FOREIGN:
        /* Root can give a file any group, so there's nothing to refuse. */
        if (geteuid() == 0) {
            printf("ok   %s: %s; not applicable as root\n", c->category,
                   c->description);
            return 0;
        }
        if (!foreign_group(&want_gid)) {
            printf("FAIL %s: %s; could not find a group this process isn't "
                   "in\n",
                   c->category, c->description);
            return 1;
        }
        snprintf(group, sizeof(group), "%u", (unsigned)want_gid);
        break;
    }

    /* A row with an ACL, or with a directory the server can't search or
     * read, gets a directory of its own for the socket. */
    if (c->acl != NO_ACL) {
        snprintf(dir, sizeof(dir), "%s/row%zu.d", g_dir, idx);
        if (!make_acl_dir(dir, c->acl)) {
            printf("FAIL %s: %s; could not give %s a default ACL: %s\n",
                   c->category, c->description, dir, strerror(errno));
            return 1;
        }
        own_dir = true;
    } else if (c->pre == PRE_NO_SEARCH || c->pre == PRE_NO_READ) {
        snprintf(dir, sizeof(dir), "%s/row%zu.d", g_dir, idx);
        if (mkdir(dir, c->pre == PRE_NO_SEARCH ? 0600 : 0300) < 0) {
            printf("FAIL %s: %s; could not make %s: %s\n", c->category,
                   c->description, dir, strerror(errno));
            return 1;
        }
        own_dir = true;
    } else if (c->pre == PRE_NO_DIR) {
        snprintf(dir, sizeof(dir), "%s/row%zu.none", g_dir, idx);
    } else {
        snprintf(dir, sizeof(dir), "%s", g_dir);
    }
    if (c->pre == PRE_DIR_SLASH)
        snprintf(sock, sizeof(sock), "%s/", dir);
    else
        snprintf(sock, sizeof(sock), "%s/row%zu.sock", dir, idx);
    snprintf(target, sizeof(target), "%s/row%zu.target", g_dir, idx);
    snprintf(log, sizeof(log), "%s/row%zu.log", g_dir, idx);
    unlink(sock);
    if (!place_at_path(c->pre, sock, target)) {
        printf("FAIL %s: %s; could not set up %s: %s\n", c->category,
               c->description, sock, strerror(errno));
        return 1;
    }

    if (strstr(c->backend, "@PORT@")) {
        port_fd = listen_ephemeral(&port);
        if (port_fd < 0) {
            printf("FAIL %s: %s; no free TCP port\n", c->category,
                   c->description);
            return 1;
        }
        if (!c->fake_manager) {
            close(port_fd);
            port_fd = -1;
        }
    }
    if (!expand_backend(c->backend, port, backend, sizeof(backend))) {
        printf("FAIL %s: %s; backend string too long\n", c->category,
               c->description);
        if (port_fd >= 0)
            close(port_fd);
        return 1;
    }
    pid =
        start_server(c, backend, sock, log, c->group == SG_NONE ? NULL : group);
    if (pid < 0) {
        printf("FAIL %s: %s; could not start the server\n", c->category,
               c->description);
        if (port_fd >= 0)
            close(port_fd);
        return 1;
    }

    if (c->want_socket &&
        !wait_for_socket(pid, sock, log,
                         c->pre == PRE_STALE ? STALE_LINE : NULL, &status,
                         &exited)) {
        if (exited) {
            describe_status(status, got, sizeof(got));
            snprintf(why, sizeof(why),
                     "the server %s before its socket "
                     "appeared",
                     got);
            pid = -1; /* reaped, so there's nothing to signal or wait for */
        } else {
            snprintf(why, sizeof(why), "the socket never appeared");
        }
        ok = false;
    }

    /* Whoever can connect drives the device, so only the owner and the group
     * asked for, at any moment.  Checked as soon as the socket appears, and
     * again once the server is up, when it must be exactly what was asked
     * for. */
    if (ok && c->want_socket)
        ok = socket_perms_ok(sock, want_mode, want_gid, true, "as it appeared",
                             why, sizeof(why));

    if (ok && c->settle && !wait_for_log(log, "waiting for client", 1)) {
        snprintf(why, sizeof(why), "never reached the attach loop");
        ok = false;
    }

    if (ok && c->want_socket && c->settle)
        ok = socket_perms_ok(sock, want_mode, want_gid, false, "once serving",
                             why, sizeof(why));

    for (int i = 0; ok && i < c->clients; i++) {
        int fd = connect_unix(sock);

        if (fd < 0) {
            snprintf(why, sizeof(why), "client %d could not connect", i + 1);
            ok = false;
            break;
        }
        if (c->action == BAD_HDR && send_bad_header(fd) < 0) {
            snprintf(why, sizeof(why), "could not send the header");
            ok = false;
        }
        if (c->action == SILENT && nheld < (int)(sizeof(held) / sizeof(*held)))
            held[nheld++] = fd;
        else
            close(fd);
        if (ok && c->want_dropped > 0 &&
            !wait_for_log(log, DROPPED_LINE, i + 1)) {
            if (waitpid(pid, &status, WNOHANG) == pid) {
                describe_status(status, got, sizeof(got));
                snprintf(why, sizeof(why),
                         "the server %s instead of logging drop %d", got,
                         i + 1);
                pid = -1;
            } else {
                snprintf(why, sizeof(why), "drop %d was never logged", i + 1);
            }
            ok = false;
        }
    }
    if (ok && c->action == SILENT)
        sleep_ms(SETTLE_MS); /* let the server block in negotiation */

    /* Still serving: alive, and the socket still takes connections. */
    if (ok && c->want_socket && waitpid(pid, &status, WNOHANG) == pid) {
        describe_status(status, got, sizeof(got));
        snprintf(why, sizeof(why), "server already gone (%s)", got);
        ok = false;
        pid = -1;
    }
    if (ok && c->want_dropped > 0) {
        int fd = connect_unix(sock);

        if (fd < 0) {
            snprintf(why, sizeof(why), "the socket stopped accepting");
            ok = false;
        } else {
            close(fd);
            /* That probe is one more drop; wait for it to be logged so it
             * cannot race the signal below. */
            if (!wait_for_log(log, DROPPED_LINE, c->want_dropped + 1)) {
                snprintf(why, sizeof(why), "probe drop never logged");
                ok = false;
            }
        }
    }

    if (pid > 0) {
        for (int i = 0; i < c->sigs; i++) {
            kill(pid, c->sig);
            if (i + 1 < c->sigs)
                sleep_ms(20);
        }
        if (wait_exit(pid, &status) < 0) {
            snprintf(why, sizeof(why), "did not exit within %d ms",
                     EXIT_WAIT_MS);
            ok = false;
        }
        describe_status(status, got, sizeof(got));
    }
    for (int i = 0; i < nheld; i++)
        close(held[i]);
    if (port_fd >= 0)
        close(port_fd);

    if (ok && !exit_matches(status, c->want_exit)) {
        snprintf(why, sizeof(why), "%s (want %s)", got,
                 c->want_exit < 0 ? "non-zero" : "exit 0");
        ok = false;
    }
    if (ok && count_in_log(log, SHUTDOWN_LINE) != c->want_shutdown) {
        snprintf(why, sizeof(why), "%d '%s' line(s) (want %d)",
                 count_in_log(log, SHUTDOWN_LINE), SHUTDOWN_LINE,
                 c->want_shutdown);
        ok = false;
    }
    /* A refused socket must not be left behind: the server closed it. */
    if (ok && c->want_gone && access(sock, F_OK) == 0) {
        snprintf(why, sizeof(why), "the refused socket is still there");
        ok = false;
    }
    if (ok && !left_alone(c->pre, sock, target)) {
        snprintf(why, sizeof(why), "what was at the socket's path changed");
        ok = false;
    }
    if (ok && c->want_line && count_in_log(log, c->want_line) == 0) {
        snprintf(why, sizeof(why), "log lacks '%s'", c->want_line);
        ok = false;
    }
    if (ok && sanitizer_reported(log)) {
        snprintf(why, sizeof(why), "sanitizer report in %s", log);
        ok = false;
    }

    printf("%s %s: %s; expected: %s\n", ok ? "ok  " : "FAIL", c->category,
           c->description, c->expected);
    if (!ok) {
        printf("     %s; server log: %s\n", why, log);
        return 1;
    }
    unlink(sock);
    unlink(log);
    if (c->pre == PRE_SYMLINK)
        unlink(target);
    if (own_dir)
        rmdir(dir);
    return 0;
}

int main(int argc, char **argv)
{
    int failures = 0;

    if (argc != 1) {
        fprintf(stderr, "usage: %s\n", argv[0]);
        return 2;
    }
    if (access(g_server, X_OK) != 0) {
        printf("SKIP: server not found: %s\n", g_server);
        return 77;
    }
    if (!mkdtemp(g_dir)) {
        perror("mkdtemp");
        return 1;
    }

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        failures += run_case(i, &cases[i]);

    if (failures) {
        printf("\n%d check(s) failed; logs kept in %s\n", failures, g_dir);
        return 1;
    }
    {
        char ns[320];

        snprintf(ns, sizeof(ns), "%s/ns.img", g_dir);
        unlink(ns);
    }
    rmdir(g_dir);
    printf("\nall checks passed\n");
    return 0;
}
