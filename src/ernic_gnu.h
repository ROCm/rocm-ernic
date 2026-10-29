/*
 * The GNU and Linux extensions the project uses.
 *
 * Every other file is compiled against exactly ISO C11, POSIX.1-2008 and
 * glibc's _DEFAULT_SOURCE (see ernic_set_compiler_flags()).  glibc offers
 * its GNU extensions only under _GNU_SOURCE, which turns on everything
 * else too: the C23 and POSIX.1-2024 library interfaces, among them
 * const-preserving strchr() and friends and a strtol() that accepts "0b".
 * So ernic_gnu.c is the one file that defines it, and calls the extensions
 * on the others' behalf.  This header needs nothing beyond POSIX.1-2008.
 *
 * The ernic-feature-macros test fails if any other file is compiled with
 * _GNU_SOURCE or another feature-test macro.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef ERNIC_GNU_H
#define ERNIC_GNU_H

#include <pthread.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/types.h>

struct rlimit;

/*
 * accept4(): accept a connection on @sockfd with @flags (SOCK_NONBLOCK, or
 * 0), always adding SOCK_CLOEXEC.  Returns the new descriptor, or -1 with
 * errno set.
 */
int ernic_gnu_accept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen,
                      int flags);

/*
 * pipe2(): create a pipe with @flags (O_NONBLOCK, or 0), always adding
 * O_CLOEXEC.  Returns 0, or -1 with errno set.
 */
int ernic_gnu_pipe2(int fds[2], int flags);

/*
 * open() with O_PATH: a descriptor that names @path without opening it for
 * I/O, which works on a UNIX socket and needs no read permission.  @flags
 * may add O_DIRECTORY and O_NOFOLLOW; O_CLOEXEC is always added.  Returns
 * the descriptor, or -1 with errno set.
 */
int ernic_gnu_open_path(const char *path, int flags);

/*
 * fchownat() with AT_EMPTY_PATH: change the owner and group of the file
 * @fd names, which may be an ernic_gnu_open_path() descriptor (fchown()
 * refuses one).  (uid_t)-1 or (gid_t)-1 leaves that one alone.  Returns 0,
 * or -1 with errno set.
 */
int ernic_gnu_fchown_path(int fd, uid_t owner, gid_t group);

/*
 * pthread_setname_np() and pthread_getname_np(): set or get @thread's name,
 * which Linux limits to 15 bytes and a NUL.  Return 0, or an error number.
 */
int ernic_gnu_pthread_setname_np(pthread_t thread, const char *name);
int ernic_gnu_pthread_getname_np(pthread_t thread, char *name, size_t len);

/*
 * prlimit() on RLIMIT_NOFILE: set and/or get the descriptor limit of process
 * @pid, or of the caller if @pid is 0.  Either pointer may be NULL.  Returns
 * 0, or -1 with errno set.
 */
int ernic_gnu_prlimit_nofile(pid_t pid, const struct rlimit *new_limit,
                             struct rlimit *old_limit);

#endif /* ERNIC_GNU_H */
