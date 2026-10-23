/*
 *
 * Unit tests for the log level in error-report.c.
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
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <fcntl.h>
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

#include "parse_int.h"
#include "qemu/error-report.h"

#define NTHREADS    8
#define RACE_ROUNDS 200
#define NO_LEVEL    (-1)

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
    size_t name_len = strlen(ENV_NAME), n = 0, out = 0;
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
    env = calloc(n + 2, sizeof(*env));
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
        free(env);
        free(entry);
        return -1;
    }
    /* Like dup2(), this clears close-on-exec on the copy. */
    if (posix_spawn_file_actions_adddup2(&actions, fd, STDERR_FILENO) == 0 &&
        posix_spawn(&pid, g_self, &actions, NULL, argv, env) != 0) {
        pid = -1;
    }
    posix_spawn_file_actions_destroy(&actions);
    free(env);
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

    if (pipe2(fds, O_CLOEXEC) != 0) {
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
