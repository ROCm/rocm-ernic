/*
 *
 * Unit tests for the log level in ernic_log.c, the reporters that print
 * through it, and its limit on how often a message is logged.
 *
 * The level is looked up from ERNIC_LOG_LEVEL on first use, so each case
 * runs in a new process that starts with it unresolved: this program,
 * started again with --case and the case's index, and ERNIC_LOG_LEVEL set
 * in its environment. The child starts its threads and has them call
 * ernic_log_get_level() at once, so several make the first call together.
 * One of them may call ernic_log_set_level() instead, as the command line
 * does. The parent reads the child's stderr and counts the warnings about
 * a bad ERNIC_LOG_LEVEL: there must be exactly one per process, however
 * many threads make the first call.
 *
 * The racing cases are run many times. Built with ThreadSanitizer, a data
 * race on the level fails the child, and so the test.
 *
 * Before the children, this process checks what doesn't depend on the
 * first call: which strings parse to a level, each level's name, and that
 * each reporter, ours and QEMU's, prints at its own level and above and
 * nothing below, with its output captured from stdout and stderr, and that
 * a log limit lets lines through and holds them back when it should.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <spawn.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ernic_gnu.h"
#include "ernic_log.h"
#include "parse_int.h"
#include "qemu/error-report.h"

#define NTHREADS    8
#define RACE_ROUNDS 200
#define NO_LEVEL    (-1)

/* POSIX leaves declaring environ to the program; only _GNU_SOURCE makes
 * <unistd.h> do it */
extern char **environ;

static const char ENV_NAME[] = "ERNIC_LOG_LEVEL";
static const char WARNING[] = "Ignoring invalid ERNIC_LOG_LEVEL";

typedef struct {
    const char *description;
    /* ERNIC_LOG_LEVEL, or NULL to leave it unset. */
    const char *env;
    /* Passed to ernic_log_set_level() before the threads start. */
    int set_before;
    /* Passed to ernic_log_set_level() by one thread, racing the others. */
    int set_racing;
    /* Passed to ernic_log_set_level() after the threads are joined. */
    int set_after;
    /* Getter threads; 1 for the cases that don't race. */
    int threads;
    int rounds;
    /* What each getter thread must see. While set_racing is pending, a
     * getter may see the environment's level or set_racing's. */
    ErnicLogLevel want_threads;
    /* What ernic_log_get_level() returns after everything is done. */
    ErnicLogLevel want_final;
    int want_warnings;
} Case;

static const Case cases[] = {
    {"unset: the default is warn", NULL, NO_LEVEL, NO_LEVEL, NO_LEVEL, 1, 1,
     ERNIC_LOG_WARN, ERNIC_LOG_WARN, 0},
    {"empty: the default is warn, with no warning", "", NO_LEVEL, NO_LEVEL,
     NO_LEVEL, 1, 1, ERNIC_LOG_WARN, ERNIC_LOG_WARN, 0},
    {"info is honoured", "info", NO_LEVEL, NO_LEVEL, NO_LEVEL, 1, 1,
     ERNIC_LOG_INFO, ERNIC_LOG_INFO, 0},
    {"names are case-insensitive", "DEBUG", NO_LEVEL, NO_LEVEL, NO_LEVEL, 1, 1,
     ERNIC_LOG_DEBUG, ERNIC_LOG_DEBUG, 0},
    {"a bad value warns and leaves warn", "loud", NO_LEVEL, NO_LEVEL, NO_LEVEL,
     1, 1, ERNIC_LOG_WARN, ERNIC_LOG_WARN, 1},
    {"a level set first wins, and the environment is never read", "loud",
     ERNIC_LOG_ERROR, NO_LEVEL, NO_LEVEL, 1, 1, ERNIC_LOG_ERROR,
     ERNIC_LOG_ERROR, 0},
    {"a level set later replaces the environment's", "info", NO_LEVEL, NO_LEVEL,
     ERNIC_LOG_NONE, 1, 1, ERNIC_LOG_INFO, ERNIC_LOG_NONE, 0},
    {"threads racing the first call agree", "debug", NO_LEVEL, NO_LEVEL,
     NO_LEVEL, NTHREADS, RACE_ROUNDS, ERNIC_LOG_DEBUG, ERNIC_LOG_DEBUG, 0},
    {"threads racing the first call warn once", "loud", NO_LEVEL, NO_LEVEL,
     NO_LEVEL, NTHREADS, RACE_ROUNDS, ERNIC_LOG_WARN, ERNIC_LOG_WARN, 1},
    {"a level set during the first call isn't overwritten", "info", NO_LEVEL,
     ERNIC_LOG_DEBUG, NO_LEVEL, NTHREADS, RACE_ROUNDS, ERNIC_LOG_INFO,
     ERNIC_LOG_DEBUG, 0},
};

#define NCASES (sizeof(cases) / sizeof(cases[0]))

/* This program, which the children run. Read from /proc/self/exe rather
 * than taken from argv[0], which names a program that may not be this one.
 * Under valgrind, executing /proc/self/exe itself would run valgrind, but
 * readlink() on it gives this program's path. */
static char g_self[PATH_MAX];

typedef struct {
    pthread_barrier_t *barrier;
    int set_level;
    /* What ernic_log_get_level() returned, if got is set. */
    ErnicLogLevel seen;
    bool got;
} ThreadArg;

static void *log_level_thread(void *opaque)
{
    ThreadArg *arg = opaque;

    pthread_barrier_wait(arg->barrier);
    if (arg->set_level != NO_LEVEL) {
        ernic_log_set_level((ErnicLogLevel)arg->set_level);
    } else {
        arg->seen = ernic_log_get_level();
        arg->got = true;
    }
    return NULL;
}

/* Runs in the child. Returns the number of failed checks. */
static int run_case(const Case *c)
{
    pthread_t threads[NTHREADS + 1];
    ThreadArg args[NTHREADS + 1];
    pthread_barrier_t barrier;
    int nthreads = c->threads + (c->set_racing != NO_LEVEL ? 1 : 0);
    int failed = 0;

    if (c->threads < 1 || c->threads > NTHREADS) {
        printf("FAIL %s: %d threads, want 1 to %d\n", c->description,
               c->threads, NTHREADS);
        return 1;
    }
    if (c->set_before != NO_LEVEL) {
        ernic_log_set_level((ErnicLogLevel)c->set_before);
    }

    if (pthread_barrier_init(&barrier, NULL, (unsigned)nthreads) != 0) {
        printf("FAIL %s: pthread_barrier_init failed\n", c->description);
        return 1;
    }
    for (int i = 0; i < nthreads; i++) {
        args[i].barrier = &barrier;
        args[i].set_level = i == c->threads ? c->set_racing : NO_LEVEL;
        args[i].seen = ERNIC_LOG_NONE;
        args[i].got = false;
        if (pthread_create(&threads[i], NULL, log_level_thread, &args[i]) !=
            0) {
            /* The started threads are waiting at the barrier, so they
             * can't be joined; _exit() rather than hang. */
            printf("FAIL %s: pthread_create failed\n", c->description);
            fflush(stdout);
            _exit(1);
        }
    }
    for (int i = 0; i < nthreads; i++) {
        pthread_join(threads[i], NULL);
    }
    pthread_barrier_destroy(&barrier);

    for (int i = 0; i < c->threads; i++) {
        ErnicLogLevel seen = args[i].seen;

        if (!args[i].got) {
            printf("FAIL %s: thread %d didn't run\n", c->description, i);
            failed++;
        } else if (seen != c->want_threads &&
                   (c->set_racing == NO_LEVEL || (int)seen != c->set_racing)) {
            printf("FAIL %s: thread %d saw %s, want %s\n", c->description, i,
                   ernic_log_level_name(seen),
                   ernic_log_level_name(c->want_threads));
            failed++;
        }
    }

    if (c->set_after != NO_LEVEL) {
        ernic_log_set_level((ErnicLogLevel)c->set_after);
    }

    ErnicLogLevel final = ernic_log_get_level();

    if (final != c->want_final) {
        printf("FAIL %s: final level %s, want %s\n", c->description,
               ernic_log_level_name(final),
               ernic_log_level_name(c->want_final));
        failed++;
    }
    return failed;
}

/*
 * The environment for a child: this process's, without ERNIC_LOG_LEVEL,
 * plus ERNIC_LOG_LEVEL=value if value isn't NULL. Sanitizer options and the
 * like pass through. The new string is stored in *entry. The caller frees
 * both the array and *entry. Returns NULL if there's no memory.
 */
static char **child_env(const char *value, char **entry)
{
    size_t name_len = strlen(ENV_NAME);
    size_t n = 0;
    size_t out = 0;
    char **env;

    *entry = NULL;
    if (value) {
        size_t value_len = strlen(value);

        if (value_len > SIZE_MAX - name_len - 2) { /* '=' and '\0' */
            return NULL;
        }
        *entry = malloc(name_len + value_len + 2);
        if (!*entry) {
            return NULL;
        }
        memcpy(*entry, ENV_NAME, name_len);
        (*entry)[name_len] = '=';
        memcpy(*entry + name_len + 1, value, value_len + 1);
    }
    while (environ[n]) {
        n++;
    }
    env = (char **)calloc(n + 2, sizeof(*env));
    if (!env) {
        free(*entry);
        *entry = NULL;
        return NULL;
    }
    for (size_t i = 0; i < n; i++) {
        if (strncmp(environ[i], ENV_NAME, name_len) != 0 ||
            environ[i][name_len] != '=') {
            env[out++] = environ[i];
        }
    }
    env[out] = *entry;
    return env;
}

static int count_warnings(const char *text)
{
    int n = 0;

    for (const char *p = strstr(text, WARNING); p;
         p = strstr(p + sizeof(WARNING) - 1, WARNING)) {
        n++;
    }
    return n;
}

/* Starts the child for case index with its stderr on fd. Returns its pid,
 * or -1. */
static pid_t start_child(size_t index, int fd)
{
    posix_spawn_file_actions_t actions;
    char arg0[] = "ernic-test-log-level";
    char opt[] = "--case";
    char arg[24];
    char *argv[] = {arg0, opt, arg, NULL};
    char *entry;
    char **env;
    pid_t pid = -1;
    int rc;

    rc = snprintf(arg, sizeof(arg), "%zu", index);
    if (rc < 0 || (size_t)rc >= sizeof(arg)) {
        return -1;
    }
    env = child_env(cases[index].env, &entry);
    if (!env) {
        return -1;
    }
    if (posix_spawn_file_actions_init(&actions) != 0) {
        free((void *)env);
        free(entry);
        return -1;
    }
    /* Like dup2(), this clears close-on-exec on the copy. */
    if (posix_spawn_file_actions_adddup2(&actions, fd, STDERR_FILENO) == 0 &&
        posix_spawn(&pid, g_self, &actions, NULL, argv, env) != 0) {
        pid = -1;
    }
    posix_spawn_file_actions_destroy(&actions);
    free((void *)env);
    free(entry);
    return pid;
}

/* Runs one round of a case in a child process, with the child's stderr
 * captured. Returns true if the round passed. */
static bool run_round(size_t index, int round)
{
    const Case *c = &cases[index];
    char buf[65536];
    size_t len = 0;
    bool ok = true;
    int fds[2];
    int status;
    pid_t pid;

    if (ernic_gnu_pipe2(fds, 0) != 0) {
        printf("FAIL %s: pipe2 failed\n", c->description);
        return false;
    }

    /* The child writes to the same stdout. */
    fflush(stdout);
    pid = start_child(index, fds[1]);
    close(fds[1]);
    if (pid < 0) {
        printf("FAIL %s: starting the child failed\n", c->description);
        close(fds[0]);
        return false;
    }

    for (;;) {
        ssize_t n = read(fds[0], buf + len, sizeof(buf) - 1 - len);

        if (n <= 0) {
            break;
        }
        len += (size_t)n;
        if (len == sizeof(buf) - 1) {
            /* Keep draining so the child can't block on a full pipe. */
            char discard[4096];

            while (read(fds[0], discard, sizeof(discard)) > 0) {
            }
            break;
        }
    }
    buf[len] = '\0';
    close(fds[0]);

    if (waitpid(pid, &status, 0) != pid) {
        printf("FAIL %s: waitpid failed\n", c->description);
        ok = false;
    } else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("FAIL %s: round %d: child exited abnormally (status 0x%x)\n",
               c->description, round, (unsigned)status);
        ok = false;
    }

    int warnings = count_warnings(buf);

    if (warnings != c->want_warnings) {
        printf("FAIL %s: round %d: %d warning(s), want %d\n", c->description,
               round, warnings, c->want_warnings);
        ok = false;
    }
    if (!ok && len > 0) {
        printf("child's stderr:\n%s", buf);
    }
    return ok;
}

/* ---- In this process ------------------------------------------------- */

/* Returns 1 if a check failed, else 0. */
static int check_parse_level(void)
{
    static const struct {
        const char *in;
        bool want_ok;
        ErnicLogLevel want;
    } rows[] = {
        {"none", true, ERNIC_LOG_NONE},   {"0", true, ERNIC_LOG_NONE},
        {"error", true, ERNIC_LOG_ERROR}, {"1", true, ERNIC_LOG_ERROR},
        {"warn", true, ERNIC_LOG_WARN},   {"warning", true, ERNIC_LOG_WARN},
        {"2", true, ERNIC_LOG_WARN},      {"info", true, ERNIC_LOG_INFO},
        {"3", true, ERNIC_LOG_INFO},      {"debug", true, ERNIC_LOG_DEBUG},
        {"4", true, ERNIC_LOG_DEBUG},     {"WARNING", true, ERNIC_LOG_WARN},
        {"InFo", true, ERNIC_LOG_INFO},   {NULL, false, ERNIC_LOG_NONE},
        {"", false, ERNIC_LOG_NONE},      {"5", false, ERNIC_LOG_NONE},
        {"-1", false, ERNIC_LOG_NONE},    {"01", false, ERNIC_LOG_NONE},
        {" info", false, ERNIC_LOG_NONE}, {"info ", false, ERNIC_LOG_NONE},
        {"inf", false, ERNIC_LOG_NONE},   {"debugging", false, ERNIC_LOG_NONE},
    };
    int failed = 0;

    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        /* A refused name must leave the output alone. */
        ErnicLogLevel got = ERNIC_LOG_INFO;
        bool ok = ernic_log_parse_level(rows[i].in, &got);
        ErnicLogLevel want = rows[i].want_ok ? rows[i].want : ERNIC_LOG_INFO;

        if (ok != rows[i].want_ok || got != want) {
            printf("FAIL parse '%s': %s, level %s; want %s, level %s\n",
                   rows[i].in ? rows[i].in : "(NULL)",
                   ok ? "accepted" : "refused", ernic_log_level_name(got),
                   rows[i].want_ok ? "accepted" : "refused",
                   ernic_log_level_name(want));
            failed++;
        }
    }
    if (!failed) {
        printf("PASS each level name and number parses, and nothing else\n");
    }
    return failed ? 1 : 0;
}

/* Returns 1 if a check failed, else 0. */
static int check_level_names(void)
{
    /* Deliberately out of range: the level a caller passes is any int.
     * NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange) */
    const ErnicLogLevel bad = (ErnicLogLevel)(ERNIC_LOG_DEBUG + 1);
    const struct {
        ErnicLogLevel level;
        const char *want;
    } rows[] = {
        {ERNIC_LOG_NONE, "none"},   {ERNIC_LOG_ERROR, "error"},
        {ERNIC_LOG_WARN, "warn"},   {ERNIC_LOG_INFO, "info"},
        {ERNIC_LOG_DEBUG, "debug"}, {bad, "unknown"},
    };
    int failed = 0;

    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        const char *got = ernic_log_level_name(rows[i].level);

        if (strcmp(got, rows[i].want) != 0) {
            printf("FAIL name of level %d: '%s', want '%s'\n",
                   (int)rows[i].level, got, rows[i].want);
            failed++;
        }
    }
    if (!failed) {
        printf("PASS each level has its name, and any other is 'unknown'\n");
    }
    return failed ? 1 : 0;
}

static void report_error(void)
{
    error_report("%s", "the message");
}

static void report_warn(void)
{
    warn_report("%s", "the message");
}

static void report_info(void)
{
    info_report("%s", "the message");
}

static void report_startup(void)
{
    ernic_startup_report("%s", "the message");
}

static void log_error(void)
{
    ernic_log(ERNIC_LOG_ERROR, "%s", "the message");
}

static void log_warn(void)
{
    ernic_log(ERNIC_LOG_WARN, "%s", "the message");
}

static void log_info(void)
{
    ernic_log(ERNIC_LOG_INFO, "%s", "the message");
}

static void log_debug(void)
{
    ernic_log(ERNIC_LOG_DEBUG, "%s", "the message");
}

static void log_none(void)
{
    ernic_log(ERNIC_LOG_NONE, "%s", "the message");
}

/*
 * Runs report() with stdout and stderr both sent to a scratch file, and
 * puts what they got in buf. Returns false if the redirection failed.
 */
static bool capture(void (*report)(void), char *buf, size_t len)
{
    FILE *tmp = tmpfile();
    int saved_out = -1;
    int saved_err = -1;
    bool ok = false;
    size_t n;

    buf[0] = '\0';
    if (!tmp) {
        return false;
    }
    fflush(stdout);
    fflush(stderr);
    saved_out = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 0);
    saved_err = fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 0);
    if (saved_out >= 0 && saved_err >= 0 &&
        dup2(fileno(tmp), STDOUT_FILENO) >= 0 &&
        dup2(fileno(tmp), STDERR_FILENO) >= 0) {
        report();
        fflush(stdout);
        fflush(stderr);
        ok = true;
    }
    if (saved_out >= 0) {
        ok = dup2(saved_out, STDOUT_FILENO) >= 0 && ok;
        close(saved_out);
    }
    if (saved_err >= 0) {
        ok = dup2(saved_err, STDERR_FILENO) >= 0 && ok;
        close(saved_err);
    }
    if (fseek(tmp, 0, SEEK_SET) == 0) {
        n = fread(buf, 1, len - 1, tmp);
        buf[n] = '\0';
    } else {
        ok = false;
    }
    fclose(tmp);
    return ok;
}

/* A reporter's lowest level for one that never prints. */
#define NEVER (ERNIC_LOG_DEBUG + 1)

/*
 * Each reporter prints at its own level and above, and nothing below it.
 * Returns 1 if a check failed, else 0.
 */
static int check_reporters(void)
{
    static const struct {
        const char *name;
        void (*report)(void);
        int level; /* the lowest level that prints it, or NEVER */
        const char *text;
    } reporters[] = {
        {"error_report", report_error, ERNIC_LOG_ERROR, "ERROR: the message\n"},
        {"warn_report", report_warn, ERNIC_LOG_WARN, "WARN: the message\n"},
        {"info_report", report_info, ERNIC_LOG_INFO, "INFO: the message\n"},
        {"ernic_startup_report", report_startup, ERNIC_LOG_WARN,
         "the message\n"},
        {"ernic_log(error)", log_error, ERNIC_LOG_ERROR,
         "ERROR: the message\n"},
        {"ernic_log(warn)", log_warn, ERNIC_LOG_WARN, "WARN: the message\n"},
        {"ernic_log(info)", log_info, ERNIC_LOG_INFO, "INFO: the message\n"},
        {"ernic_log(debug)", log_debug, ERNIC_LOG_DEBUG,
         "DEBUG: the message\n"},
        {"ernic_log(none)", log_none, NEVER, ""},
    };
    int failed = 0;

    for (int lvl = ERNIC_LOG_NONE; lvl <= ERNIC_LOG_DEBUG; lvl++) {
        ernic_log_set_level((ErnicLogLevel)lvl);
        for (size_t i = 0; i < sizeof(reporters) / sizeof(reporters[0]); i++) {
            const char *want =
                lvl >= reporters[i].level ? reporters[i].text : "";
            char got[256];

            if (!capture(reporters[i].report, got, sizeof(got))) {
                printf("FAIL %s at %s: can't capture its output\n",
                       reporters[i].name,
                       ernic_log_level_name((ErnicLogLevel)lvl));
                failed++;
            } else if (strcmp(got, want) != 0) {
                printf("FAIL %s at %s: printed '%s', want '%s'\n",
                       reporters[i].name,
                       ernic_log_level_name((ErnicLogLevel)lvl), got, want);
                failed++;
            }
        }
    }
    if (!failed) {
        printf("PASS each reporter prints at its level and above only\n");
    }
    return failed ? 1 : 0;
}

/*
 * ernic_log_limit() at @now: whether it let the line through, and if so,
 * that it reported @want_held lines held back; if not, that it reported 0.
 * Returns 1 if not, else 0.
 */
static int limit_step(ErnicLogLimit *lim, uint64_t now, bool want_ok,
                      uint64_t want_held, const char *what)
{
    uint64_t held = UINT64_MAX;
    bool ok = ernic_log_limit(lim, now, &held);

    if (!want_ok) {
        want_held = 0;
    }
    if (ok != want_ok || held != want_held) {
        printf("FAIL log limit, %s, at %" PRIu64 " ms: %s, %" PRIu64
               " held; want %s, %" PRIu64 " held\n",
               what, now, ok ? "logged" : "held back", held,
               want_ok ? "logged" : "held back", want_held);
        return 1;
    }
    return 0;
}

/*
 * A limit lets ERNIC_LOG_LIMIT_BURST lines through in each
 * ERNIC_LOG_LIMIT_MS, and the first line after an interval's burst reports
 * how many were held back. Returns 1 if a check failed, else 0.
 */
static int check_log_limit(void)
{
    const uint64_t t0s[] = {0, UINT64_C(123456789)};
    int failed = 0;

    /* From a zeroed limit, on a clock starting at 0, as a test's does, and
     * on one well past it, as CLOCK_MONOTONIC is. */
    for (size_t i = 0; i < sizeof(t0s) / sizeof(t0s[0]); i++) {
        const uint64_t t0 = t0s[i];
        ErnicLogLimit lim = {0};

        for (uint32_t n = 0; n < ERNIC_LOG_LIMIT_BURST; n++) {
            failed += limit_step(&lim, t0, true, 0, "the burst");
        }
        for (uint32_t n = 0; n < 25; n++) {
            failed += limit_step(&lim, t0, false, 0, "past the burst");
        }
        /* The last millisecond of the interval is still in it. */
        failed += limit_step(&lim, t0 + ERNIC_LOG_LIMIT_MS - 1, false, 0,
                             "the interval's last ms");
        failed += limit_step(&lim, t0 + ERNIC_LOG_LIMIT_MS, true, 26,
                             "the next interval's first line");
        /* Reported once, so the count starts again. */
        failed += limit_step(&lim, t0 + ERNIC_LOG_LIMIT_MS, true, 0,
                             "the next interval's second line");
    }

    /* A quiet interval lets the next line through with nothing to say. */
    {
        ErnicLogLimit lim = {0};

        failed += limit_step(&lim, 1000, true, 0, "a lone line");
        failed += limit_step(&lim, 1000 + (3 * ERNIC_LOG_LIMIT_MS), true, 0,
                             "a line after a quiet spell");
    }

    /* A clock set back starts a new interval rather than holding every line
     * until it catches up. */
    {
        const uint64_t t0 = UINT64_C(1000000);
        ErnicLogLimit lim = {0};

        for (uint32_t n = 0; n <= ERNIC_LOG_LIMIT_BURST; n++) {
            failed += limit_step(&lim, t0, n < ERNIC_LOG_LIMIT_BURST, 0,
                                 "a burst before the clock goes back");
        }
        failed += limit_step(&lim, t0 - 1, true, 1, "a clock set back");
    }

    /* Each limit is its own: one held back doesn't hold back another. */
    {
        ErnicLogLimit a = {0};
        ErnicLogLimit b = {0};

        for (uint32_t n = 0; n <= ERNIC_LOG_LIMIT_BURST; n++) {
            failed += limit_step(&a, 0, n < ERNIC_LOG_LIMIT_BURST, 0,
                                 "one limit's flood");
        }
        failed += limit_step(&b, 0, true, 0, "another limit");
    }

    if (!failed) {
        printf("PASS a log limit lets a burst through per interval and "
               "counts the rest\n");
    }
    return failed ? 1 : 0;
}

int main(int argc, char **argv)
{
    int failures = 0;

    if (argc == 3 && strcmp(argv[1], "--case") == 0) {
        int index;

        if (!ernic_parse_int(argv[2], &index) || index < 0 ||
            (size_t)index >= NCASES) {
            printf("FAIL: bad case index '%s'\n", argv[2]);
            return 1;
        }
        return run_case(&cases[index]) ? 1 : 0;
    }
    if (argc != 1) {
        printf("usage: %s\n", argv[0]);
        return 1;
    }

    ssize_t self_len = readlink("/proc/self/exe", g_self, sizeof(g_self) - 1);

    if (self_len < 0 || (size_t)self_len >= sizeof(g_self) - 1) {
        printf("FAIL: can't read /proc/self/exe\n");
        return 1;
    }
    g_self[self_len] = '\0';

    /* These set this process's level, which the children, started afresh,
     * don't inherit. */
    failures += check_parse_level();
    failures += check_level_names();
    failures += check_reporters();
    failures += check_log_limit();

    for (size_t i = 0; i < NCASES; i++) {
        int round;

        /* Stop a case at its first failed round, so the log stays short. */
        for (round = 0; round < cases[i].rounds; round++) {
            if (!run_round(i, round)) {
                failures++;
                break;
            }
        }
        if (round == cases[i].rounds) {
            printf("PASS %s\n", cases[i].description);
        }
    }

    if (failures) {
        printf("log_level: %d case(s) FAILED\n", failures);
        return 1;
    }
    printf("log_level: all cases passed\n");
    return 0;
}
