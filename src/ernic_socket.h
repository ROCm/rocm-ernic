/*
 * The vfio-user socket's permissions.
 *
 * Whoever connects to the socket drives the device as the guest's QEMU, so
 * the server lets in only its own user, root, and the group named by
 * --socket-group.  These check and set that on the socket the server
 * created, and refuse one that anyone else could have put at its path.
 * Each prints why it failed to stderr.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef ERNIC_SOCKET_H
#define ERNIC_SOCKET_H

#include <stdbool.h>
#include <sys/types.h>

struct stat;

/*
 * Resolve --socket-group: a group name or, failing that, a numeric gid, as
 * chgrp(1) does.  Any gid but (gid_t)-1 is valid, including those above
 * INT_MAX; that one tells lchown() to leave the group alone.  Returns false
 * if it's neither.
 */
bool ernic_socket_resolve_group(const char *spec, gid_t *gid);

/*
 * Remove a socket left at @path by a server that didn't exit cleanly, so
 * vfu_create_ctx() can bind there.  Anything else at @path, a symlink
 * included, is left alone.  Returns true if nothing is at @path now.  Prints
 * why and returns false if something other than a socket is there, or if it
 * can't tell or can't remove it.
 */
bool ernic_socket_remove_stale(const char *path);

/*
 * Check that exactly the socket's owner, and root, can connect to it, plus
 * the group @want_gid if it isn't NULL: mode 0600, or 0660 with that group,
 * and no access ACL; and, if @created isn't NULL, that it's still the socket
 * an earlier call found.  Whatever is at @path is checked as it is, without
 * following a symlink.  Prints why and returns false if anyone else may be
 * able to connect, if the owner or group may not be able to, if it's another
 * file, or if it can't tell.  On success *st holds what was checked.
 */
bool ernic_socket_access_ok(const char *path, const gid_t *want_gid,
                            const struct stat *created, struct stat *st);

/*
 * Give the socket at @path the group @gid (@name, for messages), then mode
 * 0660 to let that group in.  @created is what the first
 * ernic_socket_access_ok() found at @path.  Prints why and returns false if
 * it can't, or if @path no longer leads to that socket.
 */
bool ernic_socket_set_group(const char *path, const char *name, gid_t gid,
                            const struct stat *created);

#endif /* ERNIC_SOCKET_H */
