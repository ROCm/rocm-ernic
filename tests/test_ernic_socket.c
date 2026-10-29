/*
 * Unit tests for the vfio-user socket's permission helpers (ernic_socket.c).
 *
 * test_server_lifecycle runs them through the real server, against real
 * directories and ACLs.  This calls each one directly, including on paths
 * the server can't be driven down: the socket replaced between the first
 * check and a later one, a symlink to the socket at its path, a group entry
 * too big for the first buffer, a failing chown(), chmod(), unlink() or
 * ACL lookup, and running out of memory.  Each row checks the result, what
 * it left on disk, and the reason printed to stderr, which tells one refusal
 * from another.
 *
 * What can't be arranged with real files comes from wrappers around the
 * calls ernic_socket.c makes (the linker's --wrap), which fail one call on
 * demand and pass every other to libc.  Nothing needs root or a group other
 * than the process's own.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "ernic_gnu.h"
#include "ernic_socket.h"
#include "rocm-ernic-warnings.h"

struct group;

static int failures;

static bool check(const char *test, bool ok, const char *what)
{
    if (!ok) {
        printf("FAIL %s: %s\n", test, what);
        failures++;
    }
    return ok;
}

/* ---- Failure injection ------------------------------------------------- */

/* The call the next wrapper to see it fails, with fail_errno. */
enum fail_call {
    FAIL_NONE,
    FAIL_MALLOC,
    FAIL_STRDUP,
    FAIL_STRNDUP,
    FAIL_FCHOWNAT,
    FAIL_CHMOD,
    FAIL_LGETXATTR,
    FAIL_UNLINKAT,
};

static enum fail_call fail_call;
static int fail_errno;
static bool fail_fired;

/* Fail the next call to @call with @err. */
static void fail_next(enum fail_call call, int err)
{
    fail_call = call;
    fail_errno = err;
    fail_fired = false;
}

/* Whether the failure fail_next() asked for happened; disarms it. */
static bool fail_happened(void)
{
    fail_call = FAIL_NONE;
    return fail_fired;
}

static bool should_fail(enum fail_call call)
{
    if (fail_call != call)
        return false;
    fail_call = FAIL_NONE;
    fail_fired = true;
    errno = fail_errno;
    return true;
}

/* lgetxattr() reports an access ACL, as if a default ACL had made one. */
static bool acl_present;

/*
 * getgrnam_r() answers ERANGE for any buffer shorter than this, as glibc
 * does for an entry that doesn't fit; SIZE_MAX makes every buffer too
 * short.  0 lets every call through.  The calls and the largest buffer
 * offered are counted while it's set.
 */
static size_t grp_erange_below;
static unsigned grp_calls;
static size_t grp_max_len;

/*
 * The names are --wrap's, so they are reserved identifiers by necessity, and
 * they can't be static, since the linker has to find them.
 */
ROCM_ERNIC_WARN_RESERVED_IDENTIFIER_OFF
/* NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) */
/* NOLINTBEGIN(misc-use-internal-linkage) */
void *__real_malloc(size_t size);
char *__real_strdup(const char *s);
char *__real_strndup(const char *s, size_t n);
int __real_getgrnam_r(const char *name, struct group *grp, char *buf,
                      size_t buflen, struct group **result);
int __real_fchownat(int dirfd, const char *path, uid_t owner, gid_t group,
                    int flags);
int __real_chmod(const char *path, mode_t mode);
ssize_t __real_lgetxattr(const char *path, const char *name, void *value,
                         size_t size);
int __real_unlinkat(int dirfd, const char *path, int flags);

void *__wrap_malloc(size_t size);
char *__wrap_strdup(const char *s);
char *__wrap_strndup(const char *s, size_t n);
int __wrap_getgrnam_r(const char *name, struct group *grp, char *buf,
                      size_t buflen, struct group **result);
int __wrap_fchownat(int dirfd, const char *path, uid_t owner, gid_t group,
                    int flags);
int __wrap_chmod(const char *path, mode_t mode);
ssize_t __wrap_lgetxattr(const char *path, const char *name, void *value,
                         size_t size);
int __wrap_unlinkat(int dirfd, const char *path, int flags);

void *__wrap_malloc(size_t size)
{
    return should_fail(FAIL_MALLOC) ? NULL : __real_malloc(size);
}

char *__wrap_strdup(const char *s)
{
    return should_fail(FAIL_STRDUP) ? NULL : __real_strdup(s);
}

char *__wrap_strndup(const char *s, size_t n)
{
    return should_fail(FAIL_STRNDUP) ? NULL : __real_strndup(s, n);
}

int __wrap_getgrnam_r(const char *name, struct group *grp, char *buf,
                      size_t buflen, struct group **result)
{
    if (grp_erange_below) {
        grp_calls++;
        if (buflen > grp_max_len)
            grp_max_len = buflen;
        if (buflen < grp_erange_below) {
            *result = NULL;
            return ERANGE;
        }
    }
    return __real_getgrnam_r(name, grp, buf, buflen, result);
}

int __wrap_fchownat(int dirfd, const char *path, uid_t owner, gid_t group,
                    int flags)
{
    return should_fail(FAIL_FCHOWNAT)
               ? -1
               : __real_fchownat(dirfd, path, owner, group, flags);
}

int __wrap_chmod(const char *path, mode_t mode)
{
    return should_fail(FAIL_CHMOD) ? -1 : __real_chmod(path, mode);
}

ssize_t __wrap_lgetxattr(const char *path, const char *name, void *value,
                         size_t size)
{
    if (should_fail(FAIL_LGETXATTR))
        return -1;
    if (acl_present)
        return 28; /* a header and three entries: user, group and other */
    return __real_lgetxattr(path, name, value, size);
}

int __wrap_unlinkat(int dirfd, const char *path, int flags)
{
    return should_fail(FAIL_UNLINKAT) ? -1
                                      : __real_unlinkat(dirfd, path, flags);
}
/* NOLINTEND(misc-use-internal-linkage) */
/* NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) */
ROCM_ERNIC_WARN_RESERVED_IDENTIFIER_ON

/*
 * A row couldn't be set up, which fails the test: say so, with errno's
 * reason, and stop.  _Exit(), since glibc doesn't make exit() thread safe;
 * the flush is what exit() would have done.
 */
static _Noreturn void setup_failed(const char *what, const char *arg)
{
    fprintf(stderr, "FAIL setting up: %s %s: %s\n", what, arg, strerror(errno));
    fflush(NULL);
    _Exit(1);
}

/* ---- stderr capture ---------------------------------------------------- */

/*
 * The helpers say why they refused on stderr.  A row sends stderr to a
 * file while it calls one, then checks the file for the reason it expects,
 * so a refusal for the wrong reason fails the row.
 */
static int saved_stderr = -1;
static FILE *captured;
static char said[1024];

static void capture_begin(void)
{
    fflush(stderr);
    captured = tmpfile();
    saved_stderr = fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 0);
    if (!captured || saved_stderr < 0 ||
        dup2(fileno(captured), STDERR_FILENO) < 0)
        setup_failed("capturing", "stderr");
}

/* Stop capturing; what was printed is in said[]. */
static void capture_end(void)
{
    size_t n;

    fflush(stderr);
    if (saved_stderr < 0 || !captured || dup2(saved_stderr, STDERR_FILENO) < 0)
        setup_failed("restoring", "stderr");
    close(saved_stderr);
    saved_stderr = -1;
    if (fseek(captured, 0, SEEK_SET) < 0)
        setup_failed("reading", "stderr");
    n = fread(said, 1, sizeof(said) - 1, captured);
    said[n] = '\0';
    fclose(captured);
    captured = NULL;
    /* Pass it on, so a failing row's log shows what the helper said. */
    fputs(said, stderr);
}

/* Check that the helper said @want, and nothing at all if @want is NULL. */
static void check_said(const char *test, const char *want)
{
    char what[1200];

    if (want) {
        (void)snprintf(what, sizeof(what), "said \"%s\", wanted \"%s\"", said,
                       want);
        check(test, strstr(said, want) != NULL, what);
    } else {
        (void)snprintf(what, sizeof(what), "said \"%s\", wanted nothing", said);
        check(test, said[0] == '\0', what);
    }
}

/* ---- Fixture ----------------------------------------------------------- */

/*
 * The scratch directory.  It's under /tmp, not $TMPDIR, so a socket's path
 * fits the 108 bytes of sun_path.
 */
static char g_dir[] = "/tmp/ernic-socket-XXXXXX";

/* @name's path in the scratch directory, in @buf. */
static const char *in_dir(char *buf, size_t len, const char *name)
{
    int n = snprintf(buf, len, "%s/%s", g_dir, name);

    if (n < 0 || (size_t)n >= len) {
        errno = ENAMETOOLONG;
        setup_failed("naming", name);
    }
    return buf;
}

/*
 * Make a UNIX socket file at @path, mode 0600, as the server does: the
 * kernel applies the umask to the socket it creates, so there's no window
 * with another mode and no chmod() by name.  Exits on failure.
 */
static void make_socket(const char *path)
{
    struct sockaddr_un sa;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    mode_t old_mask;
    int rc;

    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(sa.sun_path)) {
        errno = ENAMETOOLONG;
        setup_failed("making socket", path);
    }
    memcpy(sa.sun_path, path, strlen(path) + 1);
    if (fd < 0)
        setup_failed("making socket", path);
    old_mask = umask(0177);
    rc = bind(fd, (struct sockaddr *)&sa, sizeof(sa));
    umask(old_mask);
    if (rc < 0)
        setup_failed("making socket", path);
    close(fd);
}

/*
 * Give @name in the scratch directory mode @mode.  It's named relative to
 * the directory, not by its path, so the change can only land on what's in
 * the scratch directory.  Exits on failure.
 */
static void set_mode(const char *name, mode_t mode)
{
    int dfd = ernic_gnu_open_path(g_dir, O_DIRECTORY);

    if (dfd < 0 || fchmodat(dfd, name, mode, 0) < 0)
        setup_failed("chmod", name);
    close(dfd);
}

/* Make a regular file at @path, mode 0600.  Exits on failure. */
static void make_file(const char *path)
{
    /* With _FORTIFY_SOURCE, glibc 2.40 and later give Clang an
     * open(path, flags, mode) overload, and android-cloexec-open looks for
     * O_CLOEXEC in its mode */
    /* NOLINTBEGIN(android-cloexec-open) */
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    /* NOLINTEND(android-cloexec-open) */

    if (fd < 0)
        setup_failed("making file", path);
    close(fd);
}

/* Make a symlink at @path to @target.  Exits on failure. */
static void make_symlink(const char *target, const char *path)
{
    if (symlink(target, path) < 0)
        setup_failed("making symlink", path);
}

/* lstat() @path into @st.  Exits on failure. */
static void lstat_or_die(const char *path, struct stat *st)
{
    if (lstat(path, st) < 0)
        setup_failed("lstat", path);
}

static bool exists(const char *path)
{
    struct stat st;

    return lstat(path, &st) == 0;
}

/* Empty the scratch directory, which holds no subdirectories. */
static void clear_dir(void)
{
    DIR *d = opendir(g_dir);
    const struct dirent *e;
    int fd = d ? dirfd(d) : -1;

    if (fd < 0)
        setup_failed("opening", g_dir);
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0 &&
            unlinkat(fd, e->d_name, 0) < 0)
            setup_failed("removing", e->d_name);
    }
    closedir(d);
}

/* Descriptors open in this process, to check that none are leaked. */
static int count_fds(void)
{
    DIR *d = opendir("/proc/self/fd");
    int n = 0;

    if (!d)
        setup_failed("opening", "/proc/self/fd");
    while (readdir(d) != NULL)
        n++;
    closedir(d);
    return n;
}

/* ---- ernic_socket_resolve_group() -------------------------------------- */

/* Resolve @spec with no failures injected and check the outcome. */
static void resolve_row(const char *test, const char *spec, bool want_ok,
                        gid_t want_gid)
{
    gid_t gid = 12345;
    bool ok = ernic_socket_resolve_group(spec, &gid);

    check(test, ok == want_ok, want_ok ? "refused" : "accepted");
    if (want_ok)
        check(test, gid == want_gid, "resolved to the wrong gid");
    else
        check(test, gid == 12345, "changed the gid it refused");
}

static void test_resolve_group(void)
{
    /* A name: root is in every group database. */
    resolve_row("resolve: a group name", "root", true, 0);
    /* No group has a name that's a number this big, so it's a gid. */
    resolve_row("resolve: a gid above INT_MAX", "3000000000", true,
                (gid_t)3000000000u);
    resolve_row("resolve: (gid_t)-1, which lchown() ignores", "4294967295",
                false, 0);
    resolve_row("resolve: a gid past 32 bits", "4294967296", false, 0);
    resolve_row("resolve: no such group", "no-such-ernic-group", false, 0);
    resolve_row("resolve: a number with trailing text", "12abc", false, 0);
    resolve_row("resolve: a negative number", "-1", false, 0);
    resolve_row("resolve: empty", "", false, 0);
}

/* An entry that doesn't fit the first buffer gets a bigger one. */
static void test_resolve_group_grows(void)
{
    const char *test = "resolve: a group entry bigger than the first buffer";
    gid_t gid = 12345;

    grp_erange_below = (size_t)64 * 1024;
    grp_calls = 0;
    grp_max_len = 0;
    check(test, ernic_socket_resolve_group("root", &gid), "refused");
    check(test, gid == 0, "resolved to the wrong gid");
    check(test, grp_calls > 1, "didn't retry");
    check(test, grp_max_len >= (size_t)64 * 1024,
          "never offered a big enough buffer");
    grp_erange_below = 0;
}

/*
 * An entry too big for any buffer stops the growth at 1 MiB, and then a
 * number still resolves as a gid, while a name doesn't.
 */
static void test_resolve_group_too_big(void)
{
    const char *test = "resolve: a group entry too big for any buffer";
    gid_t gid = 12345;

    grp_erange_below = SIZE_MAX;
    grp_calls = 0;
    grp_max_len = 0;
    check(test, !ernic_socket_resolve_group("root", &gid), "accepted a name");
    check(test, gid == 12345, "changed the gid it refused");
    check(test, grp_max_len >= (size_t)1 << 20 && grp_max_len < (size_t)1 << 21,
          "didn't stop growing the buffer at 1 MiB");

    grp_calls = 0;
    check(test, ernic_socket_resolve_group("1234", &gid), "refused a number");
    check(test, gid == 1234, "resolved a number to the wrong gid");
    check(test, grp_calls > 0, "never looked the number up as a name");
    grp_erange_below = 0;
}

static void test_resolve_group_no_memory(void)
{
    const char *test = "resolve: no memory for the lookup";
    gid_t gid = 12345;

    fail_next(FAIL_MALLOC, ENOMEM);
    check(test, !ernic_socket_resolve_group("root", &gid), "accepted");
    check(test, fail_happened(), "malloc() wasn't called");
    check(test, gid == 12345, "changed the gid it refused");
}

/* ---- ernic_socket_remove_stale() --------------------------------------- */

static void test_remove_stale(void)
{
    char path[128], target[128];
    const char *test;

    test = "remove_stale: nothing at the path";
    in_dir(path, sizeof(path), "x.sock");
    capture_begin();
    check(test, ernic_socket_remove_stale(path), "refused");
    capture_end();
    check_said(test, NULL);

    test = "remove_stale: a socket at the path";
    make_socket(path);
    capture_begin();
    check(test, ernic_socket_remove_stale(path), "refused");
    capture_end();
    check_said(test, NULL);
    check(test, !exists(path), "the socket is still there");

    test = "remove_stale: a regular file at the path";
    make_file(path);
    capture_begin();
    check(test, !ernic_socket_remove_stale(path), "accepted");
    capture_end();
    check_said(test, "exists and isn't a socket; not removing it");
    check(test, exists(path), "removed the file");
    clear_dir();

    /* lstat(), not stat(): the link is what's at the path. */
    test = "remove_stale: a symlink to a socket at the path";
    in_dir(target, sizeof(target), "target.sock");
    make_socket(target);
    make_symlink(target, path);
    capture_begin();
    check(test, !ernic_socket_remove_stale(path), "accepted");
    capture_end();
    check_said(test, "exists and isn't a socket; not removing it");
    check(test, exists(path), "removed the symlink");
    check(test, exists(target), "removed the socket it leads to");
    clear_dir();

    test = "remove_stale: a directory that doesn't exist";
    in_dir(path, sizeof(path), "missing/x.sock");
    capture_begin();
    check(test, !ernic_socket_remove_stale(path), "accepted");
    capture_end();
    check_said(test, "cannot open socket directory");

    test = "remove_stale: a path ending in '/'";
    in_dir(path, sizeof(path), "");
    capture_begin();
    check(test, !ernic_socket_remove_stale(path), "accepted");
    capture_end();
    check_said(test, "has no file name");
}

/* A name too long for the file system: fstatat() fails, but not ENOENT. */
static void test_remove_stale_long_name(void)
{
    const char *test = "remove_stale: a file name longer than NAME_MAX";
    char name[NAME_MAX + 2];
    char path[sizeof(g_dir) + sizeof(name) + 1];

    memset(name, 'a', sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';
    in_dir(path, sizeof(path), name);
    capture_begin();
    check(test, !ernic_socket_remove_stale(path), "accepted");
    capture_end();
    check_said(test, "cannot check");
}

/* A path with no '/' is in the current directory. */
static void test_remove_stale_relative(void)
{
    const char *test = "remove_stale: a relative path with no directory";
    char path[128];
    int cwd = ernic_gnu_open_path(".", O_DIRECTORY);

    if (cwd < 0 || chdir(g_dir) < 0)
        setup_failed("changing to", g_dir);
    make_socket("rel.sock");
    capture_begin();
    check(test, ernic_socket_remove_stale("rel.sock"), "refused");
    capture_end();
    check_said(test, NULL);
    check(test, !exists(in_dir(path, sizeof(path), "rel.sock")),
          "the socket is still there");

    /* The directory is "." here, from strdup() rather than strndup(). */
    test = "remove_stale: no memory for a relative path's directory";
    make_socket("rel.sock");
    fail_next(FAIL_STRDUP, ENOMEM);
    capture_begin();
    check(test, !ernic_socket_remove_stale("rel.sock"), "accepted");
    capture_end();
    check(test, fail_happened(), "strdup() wasn't called");
    check_said(test, "out of memory");
    check(test, exists(path), "removed the socket");

    if (fchdir(cwd) < 0)
        setup_failed("changing back from", g_dir);
    close(cwd);
    clear_dir();
}

static void test_remove_stale_failures(void)
{
    char path[128];
    const char *test;

    in_dir(path, sizeof(path), "x.sock");

    test = "remove_stale: no memory for the directory";
    make_socket(path);
    fail_next(FAIL_STRNDUP, ENOMEM);
    capture_begin();
    check(test, !ernic_socket_remove_stale(path), "accepted");
    capture_end();
    check(test, fail_happened(), "strndup() wasn't called");
    check_said(test, "out of memory");
    check(test, exists(path), "removed the socket");

    test = "remove_stale: the socket can't be removed";
    fail_next(FAIL_UNLINKAT, EACCES);
    capture_begin();
    check(test, !ernic_socket_remove_stale(path), "accepted");
    capture_end();
    check(test, fail_happened(), "unlinkat() wasn't called");
    check_said(test, "cannot remove stale socket");

    /* Something else removed it first, which leaves the path free. */
    test = "remove_stale: the socket goes before it's removed";
    fail_next(FAIL_UNLINKAT, ENOENT);
    capture_begin();
    check(test, ernic_socket_remove_stale(path), "refused");
    capture_end();
    check(test, fail_happened(), "unlinkat() wasn't called");
    check_said(test, NULL);
    clear_dir();
}

/* ---- ernic_socket_access_ok() ------------------------------------------ */

/*
 * Check @path with ernic_socket_access_ok(), letting in @want_gid and
 * comparing against @created (either may be NULL), and check that it
 * accepts it if @want_ok, or refuses it saying @want_said.  *@st gets what
 * was checked.
 */
static void access_row(const char *test, const char *path,
                       const gid_t *want_gid, const struct stat *created,
                       bool want_ok, const char *want_said, struct stat *st)
{
    capture_begin();
    check(test, ernic_socket_access_ok(path, want_gid, created, st) == want_ok,
          want_ok ? "refused" : "accepted");
    capture_end();
    check_said(test, want_said);
}

/* *st is what was checked, and @created picks out one socket. */
static void test_access_ok_identity(void)
{
    char path[128], other[128];
    struct stat created, other_st, st;

    in_dir(path, sizeof(path), "x.sock");
    in_dir(other, sizeof(other), "other.sock");
    make_socket(path);
    make_socket(other);
    lstat_or_die(path, &created);
    lstat_or_die(other, &other_st);

    memset(&st, 0, sizeof(st));
    access_row("access_ok: a socket only its owner can use", path, NULL, NULL,
               true, NULL, &st);
    check("access_ok: a socket only its owner can use",
          st.st_ino == created.st_ino && S_ISSOCK(st.st_mode),
          "*st isn't what was checked");
    access_row("access_ok: the socket found earlier", path, NULL, &created,
               true, NULL, &st);
    access_row("access_ok: another socket than the one found earlier", path,
               NULL, &other_st, false,
               "is no longer the socket the server created", &st);
    clear_dir();
}

/*
 * The mode must be exactly 0600, or 0660 with the group to let in, and
 * that group the socket's.
 */
static void test_access_ok_mode(void)
{
    char path[128];
    struct stat st;
    gid_t gid = getgid();
    gid_t not_gid = gid + 1;

    in_dir(path, sizeof(path), "x.sock");
    make_socket(path);

    set_mode("x.sock", 0640);
    access_row("access_ok: mode 0640", path, NULL, NULL, false,
               "is mode 0640, not 0600", &st);

    set_mode("x.sock", 0660);
    access_row("access_ok: mode 0660 without a group to let in", path, NULL,
               NULL, false, "is mode 0660, not 0600", &st);
    access_row("access_ok: mode 0660 and the group to let in", path, &gid, NULL,
               true, NULL, &st);
    access_row("access_ok: mode 0660 and another group", path, &not_gid, NULL,
               false, "has group", &st);

    set_mode("x.sock", 0600);
    access_row("access_ok: mode 0600 with a group to let in", path, &gid, NULL,
               false, "is mode 0600, not 0660", &st);
    clear_dir();
}

/* Any access ACL is refused, and so is not being able to tell. */
static void test_access_ok_acl(void)
{
    char path[128];
    struct stat st;

    in_dir(path, sizeof(path), "x.sock");
    make_socket(path);

    acl_present = true;
    access_row("access_ok: an ACL", path, NULL, NULL, false, "has an ACL", &st);
    acl_present = false;

    fail_next(FAIL_LGETXATTR, EIO);
    access_row("access_ok: the ACL can't be read", path, NULL, NULL, false,
               "cannot check the ACL", &st);
    check("access_ok: the ACL can't be read", fail_happened(),
          "lgetxattr() wasn't called");

    /* A file system without ACLs can't have one on the socket. */
    fail_next(FAIL_LGETXATTR, ENOTSUP);
    access_row("access_ok: a file system without ACLs", path, NULL, NULL, true,
               NULL, &st);
    check("access_ok: a file system without ACLs", fail_happened(),
          "lgetxattr() wasn't called");
    clear_dir();
}

/* Only a socket is accepted, checked as it is, without following a link. */
static void test_access_ok_not_socket(void)
{
    char path[128], other[128];
    struct stat st;

    in_dir(path, sizeof(path), "x.sock");
    in_dir(other, sizeof(other), "other.sock");

    access_row("access_ok: nothing at the path", path, NULL, NULL, false,
               "cannot stat socket", &st);

    make_file(path);
    access_row("access_ok: a regular file", path, NULL, NULL, false,
               "is not a socket", &st);
    clear_dir();

    make_socket(other);
    make_symlink(other, path);
    access_row("access_ok: a symlink to a good socket", path, NULL, NULL, false,
               "is not a socket", &st);
    clear_dir();
}

/* ---- ernic_socket_set_group() ------------------------------------------ */

/*
 * Make the socket at @path, mode 0600, and record it in @created as the
 * server's first ernic_socket_access_ok() does.
 */
static void make_created(const char *path, struct stat *created)
{
    make_socket(path);
    lstat_or_die(path, created);
}

static mode_t mode_of(const char *path)
{
    struct stat st;

    lstat_or_die(path, &st);
    return st.st_mode & 07777;
}

static void test_set_group(void)
{
    char path[128], other[128];
    struct stat created, st;
    gid_t gid = getgid();
    const char *test;

    in_dir(path, sizeof(path), "x.sock");
    in_dir(other, sizeof(other), "other.sock");

    /* Our own group: no privilege needed to give it to our own socket. */
    test = "set_group: the process's own group";
    make_created(path, &created);
    capture_begin();
    check(test, ernic_socket_set_group(path, "own", gid, &created), "refused");
    capture_end();
    check_said(test, NULL);
    check(test, mode_of(path) == 0660, "the socket isn't mode 0660");
    capture_begin();
    check(test, ernic_socket_access_ok(path, &gid, &created, &st),
          "the result isn't what access_ok() lets in");
    capture_end();
    check_said(test, NULL);
    clear_dir();

    test = "set_group: nothing at the path";
    capture_begin();
    check(test, !ernic_socket_set_group(path, "own", gid, &created),
          "accepted");
    capture_end();
    check_said(test, "cannot open socket");

    /*
     * What a server sharing a directory with someone else could meet.  The
     * server's socket stays, moved aside, while the other is made: removing
     * it first lets ext4 give the other its inode number, and then the two
     * can't be told apart.
     */
    test = "set_group: another socket put at the path";
    make_created(other, &created);
    make_socket(path);
    capture_begin();
    check(test, !ernic_socket_set_group(path, "own", gid, &created),
          "accepted");
    capture_end();
    check_said(test, "is no longer the socket the server created");
    check(test, mode_of(path) == 0600, "changed the other socket's mode");
    clear_dir();

    /* O_NOFOLLOW opens the link itself, which isn't the socket. */
    test = "set_group: a symlink to the socket put at the path";
    make_created(other, &created);
    make_symlink(other, path);
    capture_begin();
    check(test, !ernic_socket_set_group(path, "own", gid, &created),
          "accepted");
    capture_end();
    check_said(test, "is no longer the socket the server created");
    check(test, mode_of(other) == 0600, "changed the socket's mode");
    clear_dir();
}

static void test_set_group_failures(void)
{
    char path[128];
    struct stat created;
    gid_t gid = getgid();
    const char *test;

    in_dir(path, sizeof(path), "x.sock");
    make_created(path, &created);

    test = "set_group: not allowed to give the group";
    fail_next(FAIL_FCHOWNAT, EPERM);
    capture_begin();
    check(test, !ernic_socket_set_group(path, "wheel", gid, &created),
          "accepted");
    capture_end();
    check(test, fail_happened(), "fchownat() wasn't called");
    check_said(test, "cannot give socket");
    check_said(test, "the group wheel");
    check_said(test, "has to run as root or be a member of that group");
    check(test, mode_of(path) == 0600, "changed the mode");

    /* Only EPERM is about membership, so only it says so. */
    test = "set_group: the group can't be given for another reason";
    fail_next(FAIL_FCHOWNAT, EROFS);
    capture_begin();
    check(test, !ernic_socket_set_group(path, "wheel", gid, &created),
          "accepted");
    capture_end();
    check(test, fail_happened(), "fchownat() wasn't called");
    check_said(test, "cannot give socket");
    check(test, strstr(said, "has to run as root") == NULL,
          "blamed group membership");

    test = "set_group: the mode can't be set";
    fail_next(FAIL_CHMOD, EACCES);
    capture_begin();
    check(test, !ernic_socket_set_group(path, "own", gid, &created),
          "accepted");
    capture_end();
    check(test, fail_happened(), "chmod() wasn't called");
    check_said(test, "cannot make socket");
    check_said(test, "/proc/self/fd/");
    check(test, mode_of(path) == 0600, "changed the mode");
    clear_dir();
}

int main(void)
{
    int fds;

    if (!mkdtemp(g_dir)) {
        perror("mkdtemp");
        return 1;
    }
    fds = count_fds();

    test_resolve_group();
    test_resolve_group_grows();
    test_resolve_group_too_big();
    test_resolve_group_no_memory();
    test_remove_stale();
    test_remove_stale_long_name();
    test_remove_stale_relative();
    test_remove_stale_failures();
    test_access_ok_identity();
    test_access_ok_mode();
    test_access_ok_acl();
    test_access_ok_not_socket();
    test_set_group();
    test_set_group_failures();

    check("all rows", count_fds() == fds, "a descriptor was left open");
    clear_dir();
    if (rmdir(g_dir) < 0) {
        fprintf(stderr, "rmdir %s: %s\n", g_dir, strerror(errno));
        failures++;
    }

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ernic-socket-unit: all tests passed\n");
    return 0;
}
