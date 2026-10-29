/*
 * The GNU and Linux extensions the project uses.  See ernic_gnu.h.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * The only file allowed to define _GNU_SOURCE (see ernic_gnu.h).  It also
 * turns on glibc's C23 and POSIX.1-2024 interfaces here, so keep this file
 * to the wrappers.
 */
#define _GNU_SOURCE

#include "ernic_gnu.h"

#include <fcntl.h>
#include <pthread.h>
#include <stddef.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

int ernic_gnu_accept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen,
                      int flags)
{
    return accept4(sockfd, addr, addrlen, flags | SOCK_CLOEXEC);
}

int ernic_gnu_pipe2(int fds[2], int flags)
{
    return pipe2(fds, flags | O_CLOEXEC);
}

int ernic_gnu_open_path(const char *path, int flags)
{
    return open(path, flags | O_PATH | O_CLOEXEC);
}

int ernic_gnu_fchown_path(int fd, uid_t owner, gid_t group)
{
    return fchownat(fd, "", owner, group, AT_EMPTY_PATH);
}

int ernic_gnu_pthread_setname_np(pthread_t thread, const char *name)
{
    return pthread_setname_np(thread, name);
}

int ernic_gnu_pthread_getname_np(pthread_t thread, char *name, size_t len)
{
    return pthread_getname_np(thread, name, len);
}

int ernic_gnu_prlimit_nofile(pid_t pid, const struct rlimit *new_limit,
                             struct rlimit *old_limit)
{
    return prlimit(pid, RLIMIT_NOFILE, new_limit, old_limit);
}
