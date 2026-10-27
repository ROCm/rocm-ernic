/*
 * The vfio-user socket's permissions.  See ernic_socket.h.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ernic_socket.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

#include "ernic_log.h"
#include "parse_int.h"

/* A numeric --socket-group is parsed as a uint32_t, which only fits a gid_t
 * of that width. */
_Static_assert(sizeof(gid_t) == sizeof(uint32_t) && (gid_t)-1 > 0,
               "gid_t must be an unsigned 32-bit type");

bool ernic_socket_resolve_group(const char *spec, gid_t *gid)
{
    long max = sysconf(_SC_GETGR_R_SIZE_MAX);
    size_t len = max > 0 ? (size_t)max : 1024;
    uint32_t n;

    /* The buffer getgrnam_r() needs has no fixed bound, so grow it until
     * the entry fits, up to a limit no real group comes near. */
    for (;;) {
        struct group grp, *found = NULL;
        char *buf = malloc(len);
        int rc;

        if (!buf)
            return false;
        rc = getgrnam_r(spec, &grp, buf, len, &found);
        if (rc == 0 && found) {
            *gid = found->gr_gid;
            free(buf);
            return true;
        }
        free(buf);
        if (rc != ERANGE || len >= ((size_t)1 << 20))
            break;
        len *= 2;
    }
    if (ernic_parse_u32(spec, &n) && n != (uint32_t)(gid_t)-1) {
        *gid = (gid_t)n;
        return true;
    }
    return false;
}

/*
 * Check that @st, found at @path, is the socket @created: the one the first
 * ernic_socket_access_ok() found there.  Prints why and returns false if it
 * isn't.
 */
static bool socket_is_created(const char *path, const struct stat *st,
                              const struct stat *created)
{
    if (S_ISSOCK(st->st_mode) && st->st_dev == created->st_dev &&
        st->st_ino == created->st_ino)
        return true;
    fprintf(stderr,
            "rocm-ernic: %s is no longer the socket the server created; "
            "keep the socket out of directories others can write to\n",
            path);
    return false;
}

/*
 * Both changes act on a descriptor for the socket that was checked, so
 * putting something else at @path, such as a symlink or a hard link to
 * another file, can't redirect them.  The descriptor comes from O_PATH: a
 * UNIX socket can't be opened for I/O, and fchmod() on the listening socket
 * would change the socket, not the file.  fchmod() refuses an O_PATH
 * descriptor, and fchmodat() takes AT_EMPTY_PATH only from Linux 6.6 and
 * glibc 2.39, so the mode is set through the descriptor's /proc/self/fd
 * link, as glibc's fchmodat() does for AT_SYMLINK_NOFOLLOW.  That link leads
 * to the file the descriptor holds, whatever is at @path now.
 */
bool ernic_socket_set_group(const char *path, const char *name, gid_t gid,
                            const struct stat *created)
{
    char fd_path[32];
    struct stat st;
    bool ok = false;
    int n;
    int fd = open(path, O_PATH | O_NOFOLLOW | O_CLOEXEC);

    if (fd < 0) {
        fprintf(stderr, "rocm-ernic: cannot open socket %s: %s\n", path,
                strerror(errno));
        return false;
    }
    if (fstat(fd, &st) < 0) {
        fprintf(stderr, "rocm-ernic: cannot stat socket %s: %s\n", path,
                strerror(errno));
        goto out;
    }
    if (!socket_is_created(path, &st, created))
        goto out;
    if (fchownat(fd, "", (uid_t)-1, gid, AT_EMPTY_PATH) < 0) {
        int e = errno;

        fprintf(stderr, "rocm-ernic: cannot give socket %s the group %s: %s\n",
                path, name, strerror(e));
        if (e == EPERM)
            fprintf(stderr, "rocm-ernic: the server has to run as root or "
                            "be a member of that group\n");
        goto out;
    }
    n = snprintf(fd_path, sizeof(fd_path), "/proc/self/fd/%d", fd);
    if (n < 0 || (size_t)n >= sizeof(fd_path)) {
        fprintf(stderr, "rocm-ernic: cannot name descriptor %d\n", fd);
        goto out;
    }
    if (chmod(fd_path, 0660) < 0) {
        fprintf(stderr,
                "rocm-ernic: cannot make socket %s mode 0660 through %s: %s\n",
                path, fd_path, strerror(errno));
        goto out;
    }
    ok = true;
out:
    close(fd);
    return ok;
}

/*
 * The umask the socket was created under only lets the owner in, and the
 * kernel applies it to a UNIX socket itself, so the mode check is a
 * backstop: a default ACL on the directory can only take bits away.  The ACL
 * is the hole.  A default ACL is copied onto the socket, and a named entry
 * in it lets that user or group connect whatever the mode says.
 */
bool ernic_socket_access_ok(const char *path, const gid_t *want_gid,
                            const struct stat *created, struct stat *st)
{
    mode_t want_mode = want_gid ? 0660 : 0600;

    if (lstat(path, st) < 0) {
        fprintf(stderr, "rocm-ernic: cannot stat socket %s: %s\n", path,
                strerror(errno));
        return false;
    }
    if (created && !socket_is_created(path, st, created))
        return false;
    if (!S_ISSOCK(st->st_mode)) {
        fprintf(stderr, "rocm-ernic: %s is not a socket\n", path);
        return false;
    }
    if ((st->st_mode & 07777) != want_mode) {
        fprintf(stderr,
                "rocm-ernic: socket %s is mode %04o, not %04o; a default ACL "
                "on its directory can do that, so remove it (setfacl -k) or "
                "use another directory\n",
                path, (unsigned)(st->st_mode & 07777), (unsigned)want_mode);
        return false;
    }
    if (want_gid && st->st_gid != *want_gid) {
        fprintf(stderr, "rocm-ernic: socket %s has group %u, not %u\n", path,
                (unsigned)st->st_gid, (unsigned)*want_gid);
        return false;
    }

    /* A file whose ACL says no more than its mode has no ACL stored, and a
     * file system without ACLs can't have one. */
    if (lgetxattr(path, "system.posix_acl_access", NULL, 0) >= 0) {
        fprintf(stderr,
                "rocm-ernic: socket %s has an ACL, from a default ACL on its "
                "directory, that may let other users connect; remove the "
                "default ACL (setfacl -k) or use another directory, and let "
                "QEMU in with --socket-group\n",
                path);
        return false;
    }
    if (errno != ENODATA && errno != ENOTSUP) {
        fprintf(stderr, "rocm-ernic: cannot check the ACL of socket %s: %s\n",
                path, strerror(errno));
        return false;
    }
    return true;
}

/*
 * The check and the removal both name the entry relative to its directory,
 * opened once, so changing a component of @path in between can't point the
 * removal at another directory.  The entry itself can still be replaced in
 * between, but only by someone who can write to that directory, who could
 * remove it anyway.
 *
 * The directory is opened with O_PATH, which needs no permission on the
 * directory itself, as binding the socket doesn't: O_RDONLY would refuse one
 * the server can write and search but not read.  fstatat() and unlinkat()
 * still check search and write permission.
 */
bool ernic_socket_remove_stale(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *name = slash ? slash + 1 : path;
    /* The directory keeps its trailing slash, so "/x.sock" opens "/". */
    char *dir = slash ? strndup(path, (size_t)(slash - path) + 1) : strdup(".");
    struct stat st;
    bool ok = false;
    int dfd = -1;

    if (!dir) {
        fprintf(stderr, "rocm-ernic: out of memory\n");
        return false;
    }

    if (*name == '\0') {
        fprintf(stderr, "rocm-ernic: socket path \"%s\" has no file name\n",
                path);
    } else if ((dfd = open(dir, O_PATH | O_DIRECTORY | O_CLOEXEC)) < 0) {
        fprintf(stderr, "rocm-ernic: cannot open socket directory %s: %s\n",
                dir, strerror(errno));
    } else if (fstatat(dfd, name, &st, AT_SYMLINK_NOFOLLOW) < 0) {
        if (errno == ENOENT) {
            ok = true;
        } else {
            fprintf(stderr, "rocm-ernic: cannot check %s: %s\n", path,
                    strerror(errno));
        }
    } else if (!S_ISSOCK(st.st_mode)) {
        fprintf(stderr,
                "rocm-ernic: %s exists and isn't a socket; not removing it\n",
                path);
    } else if (unlinkat(dfd, name, 0) < 0 && errno != ENOENT) {
        fprintf(stderr, "rocm-ernic: cannot remove stale socket %s: %s\n", path,
                strerror(errno));
    } else {
        ernic_startup_report("Removed stale socket file: %s", path);
        ok = true;
    }

    if (dfd >= 0) {
        close(dfd);
    }
    free(dir);
    return ok;
}
