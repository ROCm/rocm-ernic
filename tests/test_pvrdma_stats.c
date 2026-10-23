/*
 *
 * Unit tests for the stats file pvrdma_stats.c writes.
 *
 * ernic-exporter and ernicctl read the file whenever they like, so they
 * must never find it empty or half written, and a write that fails must
 * leave the last good file in place. Each case points a device at a file
 * in a new directory and writes the stats. The section callback runs
 * part-way through a write, so a case can use it to read the file as a
 * reader would at that moment, or to make the write fail.
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
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <glib.h>

#include "pvrdma.h"
#include "pvrdma_stats.h"

#define DIR_TEMPLATE "/tmp/ernic-stats-XXXXXX"
#define STATS_NAME   "ernic.stats"

/* How every file with no QPs ends; one cut short doesn't. */
static const char WHOLE_TAIL[] = "Per-QP Statistics:\n"
                                 "  (no QPs created yet)\n\n";

static int failures;

static bool check(const char *test, bool ok, const char *what)
{
    if (!ok) {
        printf("FAIL %s: %s\n", test, what);
        failures++;
    }
    return ok;
}

typedef struct {
    char dir[sizeof(DIR_TEMPLATE)];
    char path[sizeof(DIR_TEMPLATE) + sizeof("/missing/" STATS_NAME)];
    PVRDMADev *dev;
} Fixture;

/* Make a new directory and a device whose stats file is @name in it. */
static bool fixture_init(const char *test, Fixture *f, const char *name)
{
    memcpy(f->dir, DIR_TEMPLATE, sizeof(DIR_TEMPLATE));
    f->dev = NULL;
    if (!check(test, mkdtemp(f->dir) != NULL, "mkdtemp failed")) {
        return false;
    }
    snprintf(f->path, sizeof(f->path), "%s/%s", f->dir, name);
    f->dev = g_new0(PVRDMADev, 1);
    f->dev->stats.stats_file = g_strdup(f->path);
    return true;
}

/* Remove everything in the directory, then the directory. */
static void fixture_cleanup(Fixture *f)
{
    DIR *d;
    struct dirent *de;

    pvrdma_stats_set_section(NULL, NULL);
    if (f->dev) {
        g_free(f->dev->stats.stats_file);
        g_free(f->dev);
    }
    d = opendir(f->dir);
    if (d) {
        while ((de = readdir(d)) != NULL) {
            if (strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0 &&
                unlinkat(dirfd(d), de->d_name, 0) != 0 && errno == EISDIR) {
                unlinkat(dirfd(d), de->d_name, AT_REMOVEDIR);
            }
        }
        closedir(d);
    }
    rmdir(f->dir);
}

/* The directory's entries, not counting "." and "..", or -1. */
static int count_entries(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    int n = 0;

    if (!d) {
        return -1;
    }
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0) {
            n++;
        }
    }
    closedir(d);
    return n;
}

/* The file's contents, to g_free(), or NULL if it can't be read. */
static char *read_file(const char *path)
{
    char *text = NULL;

    if (!g_file_get_contents(path, &text, NULL, NULL)) {
        return NULL;
    }
    return text;
}

/* Whether @text is a whole stats file whose write count is @count. */
static bool whole_file(const char *text, uint64_t count)
{
    char line[64];

    if (!text || !g_str_has_prefix(text, "=== ROCm ERNIC Statistics ===\n") ||
        !g_str_has_suffix(text, WHOLE_TAIL)) {
        return false;
    }
    snprintf(line, sizeof(line), "\nWrite count : %" PRIu64 "\n", count);
    return strstr(text, line) != NULL;
}

typedef struct {
    const char *path;
    bool look;        /* read the file at @path */
    bool break_write; /* make the write fail */
    char *seen;       /* what reading it found, or NULL */
} Probe;

static void probe_section(FILE *fp, void *opaque)
{
    Probe *p = opaque;

    if (p->look) {
        g_free(p->seen);
        p->seen = read_file(p->path);
    }
    if (p->break_write) {
        /* Writing out what's buffered then fails, as on a full disk */
        close(fileno(fp));
    }
}

static void test_first_write(void)
{
    const char *T = "first write";
    Fixture f;
    struct stat st;
    mode_t old_umask;

    if (!fixture_init(T, &f, STATS_NAME)) {
        return;
    }

    /* The mode doesn't depend on the umask: an exporter running as another
     * user must be able to read the file. */
    old_umask = umask(077);
    pvrdma_write_stats_impl(f.dev);
    umask(old_umask);

    char *text = read_file(f.path);
    check(T, whole_file(text, 0), "the file isn't a whole stats file");
    if (check(T, lstat(f.path, &st) == 0, "no stats file")) {
        check(T, S_ISREG(st.st_mode), "the stats file isn't a regular file");
        check(T, (st.st_mode & 07777) == 0644, "the stats file isn't 0644");
    }
    check(T, f.dev->stats.stats_write_count == 1,
          "the write count didn't go up");
    check(T, count_entries(f.dir) == 1, "a temporary file was left behind");
    g_free(text);
    fixture_cleanup(&f);
}

static void test_reader_sees_whole_file(void)
{
    const char *T = "reader during a write";
    Fixture f;
    Probe probe = {.look = true};
    char buf[16384];
    ssize_t n;
    int fd;

    if (!fixture_init(T, &f, STATS_NAME)) {
        return;
    }
    pvrdma_write_stats_impl(f.dev);
    char *first = read_file(f.path);
    if (!check(T, whole_file(first, 0), "the first write isn't whole")) {
        g_free(first);
        fixture_cleanup(&f);
        return;
    }

    /* A reader that opened the file before the write, and one that opens
     * it in the middle of the write, both see the first file, whole. */
    fd = open(f.path, O_RDONLY | O_CLOEXEC);
    check(T, fd >= 0, "can't open the stats file");
    probe.path = f.path;
    pvrdma_stats_set_section(probe_section, &probe);
    pvrdma_write_stats_impl(f.dev);

    check(T, probe.seen && strcmp(probe.seen, first) == 0,
          "a reader in the middle of a write didn't see the last file");
    if (fd >= 0) {
        n = read(fd, buf, sizeof(buf) - 1);
        buf[n > 0 ? n : 0] = '\0';
        check(T, strcmp(buf, first) == 0,
              "a reader holding the file open saw it change");
        close(fd);
    }

    char *second = read_file(f.path);
    check(T, whole_file(second, 1), "the second write isn't whole");
    check(T, f.dev->stats.stats_write_count == 2,
          "the write count didn't go up");
    check(T, count_entries(f.dir) == 1, "a temporary file was left behind");
    g_free(second);
    g_free(first);
    g_free(probe.seen);
    fixture_cleanup(&f);
}

static void test_write_error_keeps_last_file(void)
{
    const char *T = "write error";
    Fixture f;
    Probe probe = {.break_write = true};

    if (!fixture_init(T, &f, STATS_NAME)) {
        return;
    }
    pvrdma_write_stats_impl(f.dev);
    char *first = read_file(f.path);

    pvrdma_stats_set_section(probe_section, &probe);
    pvrdma_write_stats_impl(f.dev);

    char *after = read_file(f.path);
    check(T, first && after && strcmp(after, first) == 0,
          "a failed write replaced the last file");
    check(T, f.dev->stats.stats_write_count == 1,
          "a failed write counted as a write");
    check(T, count_entries(f.dir) == 1, "a temporary file was left behind");
    g_free(after);
    g_free(first);
    fixture_cleanup(&f);
}

static void test_rename_fails(void)
{
    const char *T = "rename fails";
    Fixture f;
    struct stat st;

    if (!fixture_init(T, &f, STATS_NAME)) {
        return;
    }

    /* A file can't replace a directory */
    check(T, mkdir(f.path, 0700) == 0, "mkdir failed");
    pvrdma_write_stats_impl(f.dev);

    check(T, lstat(f.path, &st) == 0 && S_ISDIR(st.st_mode),
          "the directory at the stats path was replaced");
    check(T, f.dev->stats.stats_write_count == 0,
          "a failed rename counted as a write");
    check(T, count_entries(f.dir) == 1, "a temporary file was left behind");
    fixture_cleanup(&f);
}

static void test_no_directory(void)
{
    const char *T = "no directory";
    Fixture f;

    if (!fixture_init(T, &f, "missing/" STATS_NAME)) {
        return;
    }
    pvrdma_write_stats_impl(f.dev);
    check(T, f.dev->stats.stats_write_count == 0,
          "a write into a missing directory counted as a write");
    check(T, count_entries(f.dir) == 0, "something was created");
    fixture_cleanup(&f);
}

static void test_symlink_is_replaced(void)
{
    const char *T = "symlink";
    static const char OTHER[] = "other";
    static const char UNTOUCHED[] = "untouched\n";
    Fixture f;
    char other[sizeof(f.dir) + sizeof(OTHER) + 1];
    struct stat st;

    if (!fixture_init(T, &f, STATS_NAME)) {
        return;
    }
    snprintf(other, sizeof(other), "%s/%s", f.dir, OTHER);

    /* The stats file replaces a symlink at its path; it never writes
     * through one to the file it names */
    check(T, g_file_set_contents(other, UNTOUCHED, sizeof(UNTOUCHED) - 1, NULL),
          "can't write the symlink's target");
    check(T, symlink(OTHER, f.path) == 0, "symlink failed");
    pvrdma_write_stats_impl(f.dev);

    char *text = read_file(other);
    check(T, text && strcmp(text, UNTOUCHED) == 0,
          "the symlink's target was written to");
    check(T, lstat(f.path, &st) == 0 && S_ISREG(st.st_mode),
          "the symlink wasn't replaced");
    g_free(text);
    text = read_file(f.path);
    check(T, whole_file(text, 0), "the file isn't a whole stats file");
    check(T, count_entries(f.dir) == 2, "a temporary file was left behind");
    g_free(text);
    fixture_cleanup(&f);
}

static void test_no_stats_file(void)
{
    const char *T = "no stats file";
    PVRDMADev *dev = g_new0(PVRDMADev, 1);

    pvrdma_write_stats_impl(dev);
    check(T, dev->stats.stats_write_count == 0,
          "writing with no stats file counted as a write");
    g_free(dev);
}

int main(void)
{
    test_first_write();
    test_reader_sees_whole_file();
    test_write_error_keeps_last_file();
    test_rename_fails();
    test_no_directory();
    test_symlink_is_replaced();
    test_no_stats_file();

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all stats file tests passed\n");
    return 0;
}
