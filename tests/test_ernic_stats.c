/*
 * Unit tests for ernic_stats.c: the counters, and the stats file.
 *
 * ernic-exporter and ernicctl read the file whenever they like, so they
 * must never find it empty or half written, and a write that fails must
 * leave the last good file in place. Each file case points the statistics
 * at a file in a new directory and writes them. The section callback runs
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

#include "ernic_limits.h"
#include "ernic_stats.h"
#include "s3_alloc_fail.h"

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
    struct ernic_stats *st;
} Fixture;

/* Make a new directory and statistics whose file is @name in it. */
static bool fixture_init(const char *test, Fixture *f, const char *name)
{
    memcpy(f->dir, DIR_TEMPLATE, sizeof(DIR_TEMPLATE));
    f->st = NULL;
    if (!check(test, mkdtemp(f->dir) != NULL, "mkdtemp failed")) {
        return false;
    }
    snprintf(f->path, sizeof(f->path), "%s/%s", f->dir, name);
    f->st = ernic_stats_create();
    if (!check(test, f->st != NULL, "ernic_stats_create() failed") ||
        !check(test, ernic_stats_set_file(f->st, f->path) == 0,
               "ernic_stats_set_file() failed")) {
        ernic_stats_destroy(f->st);
        f->st = NULL;
        rmdir(f->dir);
        return false;
    }
    return true;
}

/* Remove everything in the directory, then the directory. */
static void fixture_cleanup(Fixture *f)
{
    DIR *d;
    struct dirent *de;

    ernic_stats_destroy(f->st);
    d = opendir(f->dir);
    if (d) {
        int fd = dirfd(d);

        while (fd >= 0 && (de = readdir(d)) != NULL) {
            if (strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0 &&
                unlinkat(fd, de->d_name, 0) != 0 && errno == EISDIR) {
                unlinkat(fd, de->d_name, AT_REMOVEDIR);
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

/* Whether @text has the stats file's opening and its write count is @count. */
static bool has_count(const char *text, uint64_t count)
{
    char line[64];

    if (!text || !g_str_has_prefix(text, "=== ROCm ERNIC Statistics ===\n")) {
        return false;
    }
    snprintf(line, sizeof(line), "\nWrite count : %" PRIu64 "\n", count);
    return strstr(text, line) != NULL;
}

/* Whether @text is a whole stats file, with no QPs, whose write count is
 * @count. */
static bool whole_file(const char *text, uint64_t count)
{
    return has_count(text, count) && g_str_has_suffix(text, WHOLE_TAIL);
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

/* ---- The file is replaced whole ---------------------------------------- */

static void test_first_write(void)
{
    const char *T = "first write";
    Fixture f;
    struct stat st;
    mode_t old_umask;
    bool wrote;

    if (!fixture_init(T, &f, STATS_NAME)) {
        return;
    }

    /* The mode doesn't depend on the umask: an exporter running as another
     * user must be able to read the file. */
    old_umask = umask(077);
    wrote = ernic_stats_write(f.st);
    umask(old_umask);

    check(T, wrote, "the write wasn't reported as a success");
    char *text = read_file(f.path);
    check(T, whole_file(text, 0), "the file isn't a whole stats file");
    if (check(T, lstat(f.path, &st) == 0, "no stats file")) {
        check(T, S_ISREG(st.st_mode), "the stats file isn't a regular file");
        check(T, (st.st_mode & 07777) == 0644, "the stats file isn't 0644");
    }
    check(T, count_entries(f.dir) == 1, "a temporary file was left behind");
    g_free(text);

    check(T, ernic_stats_write(f.st), "the second write failed");
    text = read_file(f.path);
    check(T, whole_file(text, 1), "the write count didn't go up");
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
    ernic_stats_write(f.st);
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
    ernic_stats_set_section(f.st, probe_section, &probe);
    ernic_stats_write(f.st);

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
    ernic_stats_write(f.st);
    char *first = read_file(f.path);

    ernic_stats_set_section(f.st, probe_section, &probe);
    check(T, !ernic_stats_write(f.st), "a failed write reported success");

    char *after = read_file(f.path);
    check(T, first && after && strcmp(after, first) == 0,
          "a failed write replaced the last file");
    check(T, count_entries(f.dir) == 1, "a temporary file was left behind");
    g_free(after);

    /* Removing the section removes it, and the failure didn't count */
    ernic_stats_set_section(f.st, NULL, &probe);
    ernic_stats_write(f.st);
    after = read_file(f.path);
    check(T, whole_file(after, 1), "a failed write counted as a write");
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
    check(T, !ernic_stats_write(f.st), "a failed rename reported success");

    check(T, lstat(f.path, &st) == 0 && S_ISDIR(st.st_mode),
          "the directory at the stats path was replaced");
    check(T, count_entries(f.dir) == 1, "a temporary file was left behind");

    rmdir(f.path);
    ernic_stats_write(f.st);
    char *text = read_file(f.path);
    check(T, whole_file(text, 0), "a failed rename counted as a write");
    g_free(text);
    fixture_cleanup(&f);
}

static void test_no_directory(void)
{
    const char *T = "no directory";
    Fixture f;

    if (!fixture_init(T, &f, "missing/" STATS_NAME)) {
        return;
    }
    check(T, !ernic_stats_write(f.st),
          "a write into a missing directory reported success");
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
    check(T,
          g_file_set_contents(other, UNTOUCHED, sizeof(UNTOUCHED) - 1, NULL) !=
              FALSE,
          "can't write the symlink's target");
    check(T, symlink(OTHER, f.path) == 0, "symlink failed");
    ernic_stats_write(f.st);

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
    Fixture f;

    if (!fixture_init(T, &f, STATS_NAME)) {
        return;
    }
    check(T, ernic_stats_set_file(f.st, NULL) == 0, "clearing the file failed");
    check(T, !ernic_stats_write(f.st),
          "writing with no stats file reported success");
    check(T, count_entries(f.dir) == 0, "something was created");
    fixture_cleanup(&f);
}

/* ---- What the file says ------------------------------------------------ */

/* Whether @text has the line @line, whole. */
static bool has_line(const char *text, const char *line)
{
    size_t len = strlen(line);

    for (const char *p = text; p && (p = strstr(p, line)) != NULL; p++) {
        if ((p == text || p[-1] == '\n') && p[len] == '\n') {
            return true;
        }
    }
    return false;
}

static void test_instance_block(void)
{
    const char *T = "instance block";
    Fixture f;
    struct ernic_stats_instance inst = {
        .socket_path = "/run/ernic.sock",
        .backend = "tcp:node=0",
        .nic_profile = "compat (hash 0x1)",
        .pci_vid = 0x1dd8,
        .pci_did = 0x1002,
        .mac = {0x72, 0x6f, 0x63, 0x6d, 0x2d, 0x6e},
    };

    if (!fixture_init(T, &f, STATS_NAME)) {
        return;
    }

    /* Nothing set yet */
    ernic_stats_write(f.st);
    char *text = read_file(f.path);
    check(T,
          text && has_line(text, "  socket      : (not set)") &&
              has_line(text, "  backend     : (not set)") &&
              has_line(text, "  pci         : (not set)") &&
              has_line(text, "  mac         : 00:00:00:00:00:00") &&
              has_line(text, "  connection  : (not set)") &&
              has_line(text, "  nic_profile : (not set)"),
          "an unset instance block doesn't say so");
    g_free(text);

    check(T, ernic_stats_set_instance(f.st, &inst) == 0,
          "ernic_stats_set_instance() failed");
    ernic_stats_set_connection(f.st, "connected (PCI FLR)");
    ernic_stats_write(f.st);
    text = read_file(f.path);
    check(T,
          text && has_line(text, "  socket      : /run/ernic.sock") &&
              has_line(text, "  backend     : tcp:node=0") &&
              has_line(text, "  pci         : 0x1dd8:0x1002") &&
              has_line(text, "  mac         : 72:6f:63:6d:2d:6e") &&
              has_line(text, "  connection  : connected (PCI FLR)") &&
              has_line(text, "  nic_profile : compat (hash 0x1)"),
          "the instance block isn't what was set");
    g_free(text);

    /* Copies, so the caller's strings needn't last */
    char socket_path[] = "/run/other.sock";
    inst.socket_path = socket_path;
    inst.backend = NULL;
    inst.nic_profile = NULL;
    check(T, ernic_stats_set_instance(f.st, &inst) == 0,
          "setting the block again failed");
    memset(socket_path, 'x', sizeof(socket_path) - 1);
    ernic_stats_set_connection(f.st, NULL);
    ernic_stats_write(f.st);
    text = read_file(f.path);
    check(T,
          text && has_line(text, "  socket      : /run/other.sock") &&
              has_line(text, "  backend     : (not set)") &&
              has_line(text, "  connection  : (not set)") &&
              has_line(text, "  nic_profile : (not set)"),
          "setting the block again didn't replace it");
    g_free(text);
    fixture_cleanup(&f);
}

static void test_device_counters(void)
{
    const char *T = "device counters";
    Fixture f;
    struct ernic_dev_counters dc;

    if (!fixture_init(T, &f, STATS_NAME)) {
        return;
    }
    ernic_stats_bar0_access(f.st, false);
    ernic_stats_bar0_access(f.st, true);
    ernic_stats_bar0_access(f.st, true);
    ernic_stats_doorbell_access(f.st, false);
    ernic_stats_doorbell_access(f.st, true);
    ernic_stats_doorbell_access(f.st, true);
    ernic_stats_doorbell_access(f.st, true);
    ernic_stats_irq(f.st);
    ernic_stats_command(f.st);
    ernic_stats_command(f.st);
    ernic_stats_device_reset(f.st);
    ernic_stats_eth_bytes(f.st, 100, true);
    ernic_stats_eth_bytes(f.st, 60, false);
    ernic_stats_rdma_bytes(f.st, ERNIC_STAT_NO_QP, 1, ERNIC_STAT_SEND);
    ernic_stats_rdma_bytes(f.st, ERNIC_STAT_NO_QP, 2, ERNIC_STAT_RECV);
    ernic_stats_rdma_bytes(f.st, ERNIC_STAT_NO_QP, 3, ERNIC_STAT_RDMA_READ);
    ernic_stats_rdma_bytes(f.st, ERNIC_STAT_NO_QP, 4, ERNIC_STAT_RDMA_WRITE);

    check(T, ernic_stats_dev_counters(f.st, &dc) == 0,
          "ernic_stats_dev_counters() failed");
    check(T,
          dc.commands == 2 && dc.total_bytes_sent == 1 &&
              dc.total_bytes_received == 2 && dc.total_bytes_rdma_read == 3 &&
              dc.total_bytes_rdma_write == 4,
          "the device totals aren't what was counted");

    ernic_stats_write(f.st);
    char *text = read_file(f.path);
    check(T,
          text && has_line(text, "  commands                : 2") &&
              has_line(text, "  bar0_reads              : 1") &&
              has_line(text, "  bar0_writes             : 2") &&
              has_line(text, "  uar_reads               : 1") &&
              has_line(text, "  uar_writes              : 3") &&
              has_line(text, "  mmio_reads_total        : 2") &&
              has_line(text, "  mmio_writes_total       : 5") &&
              has_line(text, "  interrupts              : 1") &&
              has_line(text, "  reset_count             : 1") &&
              has_line(text, "  total_bytes_sent        : 1") &&
              has_line(text, "  total_bytes_received    : 2") &&
              has_line(text, "  total_bytes_rdma_read   : 3") &&
              has_line(text, "  total_bytes_rdma_write  : 4") &&
              has_line(text, "  total_ip_bytes_tx       : 100") &&
              has_line(text, "  total_ip_bytes_rx       : 60"),
          "the file's device statistics aren't what was counted");
    /* Totals only: no QP has anything counted against it */
    check(T, whole_file(text, 0), "a QP was listed");
    g_free(text);
    fixture_cleanup(&f);
}

static void test_qp_counters(void)
{
    const char *T = "QP counters";
    const uint32_t last = ERNIC_MAX_QP - 1;
    Fixture f;
    struct ernic_qp_counters qc;

    if (!fixture_init(T, &f, STATS_NAME)) {
        return;
    }

    /* Counted out of order; listed in order */
    ernic_stats_qp_doorbell(f.st, last, true);
    ernic_stats_qp_doorbell(f.st, 3, true);
    ernic_stats_qp_doorbell(f.st, 3, false);
    ernic_stats_qp_wqe(f.st, 3, ERNIC_STAT_WR_SEND);
    ernic_stats_qp_wqe(f.st, 3, ERNIC_STAT_WR_SEND);
    ernic_stats_qp_wqe(f.st, 3, ERNIC_STAT_WR_RDMA_WRITE);
    ernic_stats_qp_wqe(f.st, 3, ERNIC_STAT_WR_COUNT);
    ernic_stats_qp_cqe(f.st, 3);
    ernic_stats_rdma_bytes(f.st, 3, 10, ERNIC_STAT_SEND);
    ernic_stats_rdma_bytes(f.st, 3, 20, ERNIC_STAT_RECV);
    ernic_stats_rdma_bytes(f.st, 3, 30, ERNIC_STAT_RDMA_READ);
    ernic_stats_rdma_bytes(f.st, 3, 40, ERNIC_STAT_RDMA_WRITE);
    /* Nothing moved: no entry for QP 5 */
    ernic_stats_rdma_bytes(f.st, 5, 0, ERNIC_STAT_SEND);

    check(T, ernic_stats_qp_counters(f.st, 3, &qc) == 0,
          "ernic_stats_qp_counters() failed");
    check(T,
          qc.doorbell_send == 1 && qc.doorbell_recv == 1 &&
              qc.wqes_processed == 4 && qc.cqes_posted == 1 &&
              qc.bytes_sent == 10 && qc.bytes_received == 20 &&
              qc.bytes_rdma_read == 30 && qc.bytes_rdma_write == 40,
          "QP 3's counters aren't what was counted");
    check(T,
          ernic_stats_qp_counters(f.st, 4, &qc) == 0 && qc.doorbell_send == 0 &&
              qc.wqes_processed == 0,
          "a QP with nothing counted isn't zeros");

    ernic_stats_write(f.st);
    char *text = read_file(f.path);
    char last_line[32];
    snprintf(last_line, sizeof(last_line), "  QP %" PRIu32 ":", last);
    const char *qp3 = text ? strstr(text, "\n  QP 3:\n") : NULL;
    const char *qp_last = text ? strstr(text, last_line) : NULL;
    check(T, qp3 && qp_last && qp3 < qp_last, "the QPs aren't listed in order");
    check(T, text && !strstr(text, "  QP 5:"),
          "a QP with nothing moved was listed");
    check(T,
          text && has_line(text, "    doorbell_send  : 1") &&
              has_line(text, "    wqes_processed : 4") &&
              has_line(text, "    bytes_rdma_write : 40") &&
              has_line(text, "      SEND                  : 2") &&
              has_line(text, "      RDMA_WRITE            : 1") &&
              has_line(text, "      (other opcodes)       : 1"),
          "QP 3's block isn't what was counted");
    g_free(text);

    /* A destroyed QP's counters go, and a new one starts from zero */
    ernic_stats_qp_forget(f.st, 3);
    ernic_stats_qp_forget(f.st, last);
    check(T,
          ernic_stats_qp_counters(f.st, 3, &qc) == 0 && qc.doorbell_send == 0 &&
              qc.bytes_rdma_write == 0,
          "a forgotten QP kept its counters");
    ernic_stats_write(f.st);
    text = read_file(f.path);
    check(T, whole_file(text, 1), "a forgotten QP is still listed");
    g_free(text);
    fixture_cleanup(&f);
}

/* No QP has an id at or above ERNIC_MAX_QP, so one counts toward nothing. */
static void test_qp_out_of_range(void)
{
    const char *T = "QP out of range";
    Fixture f;
    struct ernic_qp_counters qc;
    struct ernic_dev_counters dc;

    if (!fixture_init(T, &f, STATS_NAME)) {
        return;
    }
    ernic_stats_qp_doorbell(f.st, ERNIC_MAX_QP, true);
    ernic_stats_qp_wqe(f.st, ERNIC_MAX_QP, ERNIC_STAT_WR_SEND);
    ernic_stats_qp_cqe(f.st, ERNIC_MAX_QP);
    ernic_stats_qp_forget(f.st, ERNIC_MAX_QP);
    ernic_stats_rdma_bytes(f.st, ERNIC_MAX_QP, 7, ERNIC_STAT_SEND);

    check(T,
          ernic_stats_qp_counters(f.st, ERNIC_MAX_QP, &qc) == 0 &&
              qc.doorbell_send == 0 && qc.bytes_sent == 0,
          "an out-of-range QP has counters");
    /* Its bytes still moved, so the device totals have them */
    check(T,
          ernic_stats_dev_counters(f.st, &dc) == 0 && dc.total_bytes_sent == 7,
          "an out-of-range QP's bytes are missing from the totals");
    ernic_stats_write(f.st);
    char *text = read_file(f.path);
    check(T, whole_file(text, 0), "an out-of-range QP was listed");
    g_free(text);
    fixture_cleanup(&f);
}

/* ---- NULL and allocation failures -------------------------------------- */

static void test_null(void)
{
    const char *T = "NULL";
    struct ernic_stats_instance inst = {.socket_path = "s"};
    struct ernic_dev_counters dc;
    struct ernic_qp_counters qc;
    struct ernic_stats *st;

    /* Each is a no-op on NULL statistics */
    ernic_stats_destroy(NULL);
    ernic_stats_bar0_access(NULL, true);
    ernic_stats_doorbell_access(NULL, true);
    ernic_stats_irq(NULL);
    ernic_stats_eth_bytes(NULL, 1, true);
    ernic_stats_command(NULL);
    ernic_stats_device_reset(NULL);
    ernic_stats_rdma_bytes(NULL, 0, 1, ERNIC_STAT_SEND);
    ernic_stats_qp_doorbell(NULL, 0, true);
    ernic_stats_qp_wqe(NULL, 0, ERNIC_STAT_WR_SEND);
    ernic_stats_qp_cqe(NULL, 0);
    ernic_stats_qp_forget(NULL, 0);
    ernic_stats_set_connection(NULL, "connected");
    ernic_stats_set_section(NULL, probe_section, NULL);
    check(T, ernic_stats_set_file(NULL, "x") == 0, "set_file failed");
    check(T, ernic_stats_set_instance(NULL, &inst) == 0, "set_instance failed");
    check(T, !ernic_stats_write(NULL), "a write reported success");

    /* Reading back fails, and zeroes what it would have filled */
    memset(&dc, 0xff, sizeof(dc));
    memset(&qc, 0xff, sizeof(qc));
    check(T, ernic_stats_dev_counters(NULL, &dc) == -EINVAL && dc.commands == 0,
          "dev_counters(NULL) didn't fail cleanly");
    check(T,
          ernic_stats_qp_counters(NULL, 0, &qc) == -EINVAL &&
              qc.doorbell_send == 0,
          "qp_counters(NULL) didn't fail cleanly");

    st = ernic_stats_create();
    if (!check(T, st != NULL, "ernic_stats_create() failed")) {
        return;
    }
    check(T, ernic_stats_dev_counters(st, NULL) == -EINVAL,
          "dev_counters() took a NULL @out");
    check(T, ernic_stats_qp_counters(st, 0, NULL) == -EINVAL,
          "qp_counters() took a NULL @out");
    check(T, ernic_stats_set_instance(st, NULL) == 0,
          "set_instance(NULL) failed");
    ernic_stats_destroy(st);
}

static void test_alloc_failures(void)
{
    const char *T = "allocation failures";
    struct ernic_stats_instance inst = {
        .socket_path = "/run/ernic.sock",
        .backend = "loopback",
        .nic_profile = "compat",
        .pci_vid = 1,
    };
    Fixture f;

    s3_alloc_fail_arm(0);
    check(T, ernic_stats_create() == NULL, "create survived no memory");
    s3_alloc_fail_disarm();

    if (!fixture_init(T, &f, STATS_NAME)) {
        return;
    }
    ernic_stats_set_connection(f.st, "connected");

    /* The temporary file's name */
    s3_alloc_fail_arm(0);
    check(T, !ernic_stats_write(f.st), "a write survived no memory");
    check(T, s3_alloc_fail_disarm(), "the write allocated nothing");
    check(T, count_entries(f.dir) == 0, "something was created");

    /* Each of the block's three strings; a failure changes nothing */
    for (unsigned nth = 0; nth < 3; nth++) {
        s3_alloc_fail_arm(nth);
        check(T, ernic_stats_set_instance(f.st, &inst) == -ENOMEM,
              "set_instance survived no memory");
        check(T, s3_alloc_fail_disarm(), "set_instance allocated too little");
    }
    ernic_stats_write(f.st);
    char *text = read_file(f.path);
    check(T,
          text && has_line(text, "  socket      : (not set)") &&
              has_line(text, "  pci         : (not set)") &&
              has_line(text, "  connection  : connected"),
          "a failed set_instance changed the block");
    g_free(text);

    /* A failed set_file leaves no file */
    s3_alloc_fail_arm(0);
    check(T, ernic_stats_set_file(f.st, f.path) == -ENOMEM,
          "set_file survived no memory");
    s3_alloc_fail_disarm();
    check(T, !ernic_stats_write(f.st), "a failed set_file left a file");
    fixture_cleanup(&f);
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
    test_instance_block();
    test_device_counters();
    test_qp_counters();
    test_qp_out_of_range();
    test_null();
    test_alloc_failures();

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all statistics tests passed\n");
    return 0;
}
