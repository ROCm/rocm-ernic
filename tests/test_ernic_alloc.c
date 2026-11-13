/*
 * Unit tests for the allocation helpers (ernic_alloc.h).
 *
 * Each helper is checked for what it returns: a zeroed array, a copy cut
 * at the right length, a formatted string, and a usable block for a zero
 * size.  The copies are checked under ASan for reading no further than
 * they should, from a buffer with no terminator.
 *
 * The program is linked with --wrap=malloc,--wrap=calloc
 * (tests/CMakeLists.txt), so a case can fail the next allocation.  Every
 * helper must then abort, as must ernic_xcalloc() given an array too large
 * for a size_t and ernic_xasprintf() given arguments it can't format.  Each
 * of those runs in a child process, which must die by SIGABRT.  The
 * wrappers also return NULL for an allocation of nothing, as the C library
 * may, so a helper that passed a zero size on would abort.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ernic_alloc.h"
#include "rocm-ernic-warnings.h"

static int failures;

/* While set, the next malloc() or calloc() fails and clears it */
static bool fail_next_alloc;

static void check(bool ok, const char *what)
{
    if (!ok) {
        printf("FAIL %s\n", what);
        failures++;
    }
}

/*
 * The names are --wrap's, so they are reserved identifiers by necessity, and
 * they can't be static, since the linker has to find them.
 */
ROCM_ERNIC_WARN_RESERVED_IDENTIFIER_OFF
/* NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) */
/* NOLINTBEGIN(misc-use-internal-linkage) */
void *__real_malloc(size_t size);
void *__real_calloc(size_t nmemb, size_t size);
void *__wrap_malloc(size_t size);
void *__wrap_calloc(size_t nmemb, size_t size);

void *__wrap_malloc(size_t size)
{
    if (fail_next_alloc) {
        fail_next_alloc = false;
        errno = ENOMEM;
        return NULL;
    }
    return size ? __real_malloc(size) : NULL;
}

void *__wrap_calloc(size_t nmemb, size_t size)
{
    if (fail_next_alloc) {
        fail_next_alloc = false;
        errno = ENOMEM;
        return NULL;
    }
    return nmemb && size ? __real_calloc(nmemb, size) : NULL;
}
/* NOLINTEND(misc-use-internal-linkage) */
/* NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) */
ROCM_ERNIC_WARN_RESERVED_IDENTIFIER_ON

/* ---- What the helpers return ----------------------------------------- */

static void test_xmalloc(void)
{
    char *p = ernic_xmalloc(64);
    char *zero = ernic_xmalloc(0);

    memset(p, 0xA5, 64);
    check(p[63] == (char)0xA5, "xmalloc: block writable");
    check(zero != NULL, "xmalloc: zero bytes gave NULL");
    free(p);
    free(zero);
}

static void test_xcalloc(void)
{
    uint64_t *a = ernic_xcalloc(32, sizeof(*a));
    bool zeroed = true;

    for (size_t i = 0; i < 32; i++) {
        if (a[i] != 0) {
            zeroed = false;
        }
    }
    check(zeroed, "xcalloc: not zeroed");
    free(a);

    a = ernic_xcalloc(0, sizeof(*a));
    check(a != NULL, "xcalloc: zero elements gave NULL");
    free(a);
    a = ernic_xcalloc(4, 0);
    check(a != NULL, "xcalloc: zero-size elements gave NULL");
    free(a);
}

static void test_xstrdup(void)
{
    char *s = ernic_xstrdup("hello");
    char *empty = ernic_xstrdup("");

    check(strcmp(s, "hello") == 0, "xstrdup: copy");
    check(empty[0] == '\0', "xstrdup: empty string");
    free(s);
    free(empty);
}

static void test_xstrndup(void)
{
    /* No terminator: a copy reading past n bytes leaves the array */
    static const char unterminated[4] = {'a', 'b', 'c', 'd'};
    char *s;

    s = ernic_xstrndup(unterminated, sizeof(unterminated));
    check(strcmp(s, "abcd") == 0, "xstrndup: whole unterminated buffer");
    free(s);

    s = ernic_xstrndup(unterminated, 2);
    check(strcmp(s, "ab") == 0, "xstrndup: cut short");
    free(s);

    s = ernic_xstrndup("xy", 10);
    check(strcmp(s, "xy") == 0, "xstrndup: shorter than n");
    free(s);

    s = ernic_xstrndup("xy", 0);
    check(s[0] == '\0', "xstrndup: n of 0");
    free(s);
}

static void test_xasprintf(void)
{
    char long_arg[1000];
    char *s;

    s = ernic_xasprintf("%s:%u", "host", 80U);
    check(strcmp(s, "host:80") == 0, "xasprintf: format");
    free(s);

    s = ernic_xasprintf("%s", "");
    check(s[0] == '\0', "xasprintf: empty result");
    free(s);

    /* Longer than any small first guess at the size */
    memset(long_arg, 'z', sizeof(long_arg) - 1);
    long_arg[sizeof(long_arg) - 1] = '\0';
    s = ernic_xasprintf("[%s]", long_arg);
    check(strlen(s) == sizeof(long_arg) + 1 && s[0] == '[' &&
              s[sizeof(long_arg)] == ']' &&
              strspn(s + 1, "z") == sizeof(long_arg) - 1,
          "xasprintf: long result");
    free(s);
}

/* ---- What makes them abort ------------------------------------------- */

/* Runs @p fn in a child process, which must die by SIGABRT */
static void expect_abort(void (*fn)(void), const char *what)
{
    int status;
    pid_t pid;

    (void)fflush(stdout);
    pid = fork();
    if (pid < 0) {
        printf("FAIL %s: fork failed\n", what);
        failures++;
        return;
    }
    if (pid == 0) {
        /* The fatal message is expected; keep it out of the test log. */
        if (!freopen("/dev/null", "w", stderr)) {
            _exit(2);
        }
        alarm(30);
        fn();
        _exit(0);
    }

    if (waitpid(pid, &status, 0) != pid) {
        printf("FAIL %s: waitpid failed\n", what);
        failures++;
    } else if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGABRT) {
        printf("FAIL %s did not abort\n", what);
        failures++;
    }
}

/* Each frees what it got, so a helper that returns doesn't leak */

static void xmalloc_no_memory(void)
{
    fail_next_alloc = true;
    free(ernic_xmalloc(16));
}

static void xmalloc_zero_no_memory(void)
{
    fail_next_alloc = true;
    free(ernic_xmalloc(0));
}

static void xcalloc_no_memory(void)
{
    fail_next_alloc = true;
    free(ernic_xcalloc(4, 4));
}

static void xcalloc_overflow(void)
{
    free(ernic_xcalloc((SIZE_MAX / 8) + 1, 8));
}

static void xstrdup_no_memory(void)
{
    fail_next_alloc = true;
    free(ernic_xstrdup("abc"));
}

static void xstrndup_no_memory(void)
{
    fail_next_alloc = true;
    free(ernic_xstrndup("abc", 2));
}

static void xasprintf_no_memory(void)
{
    fail_next_alloc = true;
    free(ernic_xasprintf("%d", 42));
}

/* A wide character the C locale has no multibyte form for */
static void xasprintf_unformattable(void)
{
    static const wchar_t snowman[] = {0x2603, 0};

    free(ernic_xasprintf("%ls", snowman));
}

int main(void)
{
    test_xmalloc();
    test_xcalloc();
    test_xstrdup();
    test_xstrndup();
    test_xasprintf();

    expect_abort(xmalloc_no_memory, "xmalloc out of memory");
    expect_abort(xmalloc_zero_no_memory, "xmalloc of 0 out of memory");
    expect_abort(xcalloc_no_memory, "xcalloc out of memory");
    expect_abort(xcalloc_overflow, "xcalloc overflow");
    expect_abort(xstrdup_no_memory, "xstrdup out of memory");
    expect_abort(xstrndup_no_memory, "xstrndup out of memory");
    expect_abort(xasprintf_no_memory, "xasprintf out of memory");
    expect_abort(xasprintf_unformattable, "xasprintf unformattable");

    if (failures) {
        printf("\n%d failure(s)\n", failures);
    } else {
        printf("all ernic_alloc tests passed\n");
    }
    return failures ? 1 : 0;
}
