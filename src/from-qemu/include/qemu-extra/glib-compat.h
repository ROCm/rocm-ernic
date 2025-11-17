/*
 * GLib compatibility header for standalone build
 * Just include GLib if available (libvfio-user already depends on it)
 */

#ifndef GLIB_COMPAT_H
#define GLIB_COMPAT_H

/* libvfio-user already depends on GLib, so just include it */
#include <glib.h>

#endif /* GLIB_COMPAT_H */
