/*
 * Unit tests for the growable arrays (ernic_vec.h).
 *
 * Each pointer item is a counter in slots[], which the free function
 * increments, so a test can tell which items a vector freed and how often.
 * The growth tests add enough items to make each vector grow several times,
 * then check that every one kept its place, and one appends the vector's own
 * elements as it grows.  The element vector's elements are an odd size, so
 * an element written at the wrong offset overlaps its neighbours.
 *
 * The program is linked with --wrap=realloc (tests/CMakeLists.txt), so a
 * case can fail the next growth.  That must abort, as must an index past the
 * end, an element size of 0, and a copy between vectors of different element
 * sizes.  Each of those runs in a child process, which must die by SIGABRT.
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

#include "ernic_vec.h"
#include "rocm-ernic-warnings.h"

#define N_SLOTS 1000U

/* An element of an odd size, which no alignment rounds up */
typedef struct {
    unsigned char b[3];
} Elem3;

static unsigned slots[N_SLOTS];
static unsigned null_freed; /* NULL items freed */
static int failures;

/* While set, the next realloc() fails and clears it */
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
void *__real_realloc(void *ptr, size_t size);
void *__wrap_realloc(void *ptr, size_t size);

void *__wrap_realloc(void *ptr, size_t size)
{
    if (fail_next_alloc) {
        fail_next_alloc = false;
        errno = ENOMEM;
        return NULL;
    }
    return __real_realloc(ptr, size);
}
/* NOLINTEND(misc-use-internal-linkage) */
/* NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) */
ROCM_ERNIC_WARN_RESERVED_IDENTIFIER_ON

static void count_free(void *item)
{
    unsigned *slot = item;

    if (slot) {
        (*slot)++;
    } else {
        null_freed++;
    }
}

static void reset_slots(void)
{
    memset(slots, 0, sizeof(slots));
    null_freed = 0;
}

static Elem3 elem3(unsigned i)
{
    Elem3 e = {{(unsigned char)(i & 0xFFU), (unsigned char)((i >> 8) & 0xFFU),
                (unsigned char)(0xA5U ^ (i & 0xFFU))}};

    return e;
}

static bool elem3_is(const void *p, unsigned i)
{
    Elem3 want = elem3(i);

    return memcmp(p, &want, sizeof(want)) == 0;
}

/* ---- Pointer vectors ------------------------------------------------- */

static void test_ptr_empty(void)
{
    ErnicPtrVec vec;

    reset_slots();
    ernic_ptr_vec_init(&vec);
    check(ernic_ptr_vec_len(&vec) == 0, "ptr empty: len");
    check(!ernic_ptr_vec_remove_fast(&vec, &slots[0]), "ptr empty: remove");
    ernic_ptr_vec_free(&vec, count_free);
    check(null_freed == 0 && slots[0] == 0, "ptr empty: freed something");
    check(ernic_ptr_vec_len(&vec) == 0, "ptr empty: len after free");
}

static void test_ptr_growth(void)
{
    ErnicPtrVec vec;
    bool ok = true;

    reset_slots();
    ernic_ptr_vec_init(&vec);
    for (unsigned i = 0; i < N_SLOTS; i++) {
        ernic_ptr_vec_push(&vec, &slots[i]);
    }
    ernic_ptr_vec_push(&vec, NULL);
    check(ernic_ptr_vec_len(&vec) == N_SLOTS + 1, "ptr growth: len");
    for (unsigned i = 0; i < N_SLOTS; i++) {
        if (ernic_ptr_vec_get(&vec, i) != &slots[i]) {
            ok = false;
        }
    }
    check(ok, "ptr growth: every item in place");
    check(ernic_ptr_vec_get(&vec, N_SLOTS) == NULL, "ptr growth: NULL item");

    /* Every item is freed once, the NULL one included */
    ernic_ptr_vec_free(&vec, count_free);
    for (unsigned i = 0; i < N_SLOTS; i++) {
        if (slots[i] != 1) {
            ok = false;
        }
    }
    check(ok, "ptr growth: free frees every item once");
    check(null_freed == 1, "ptr growth: free frees the NULL item");
    check(ernic_ptr_vec_len(&vec) == 0, "ptr growth: len after free");

    /* The vector is reusable after free */
    ernic_ptr_vec_push(&vec, &slots[5]);
    check(ernic_ptr_vec_len(&vec) == 1 &&
              ernic_ptr_vec_get(&vec, 0) == &slots[5],
          "ptr reuse: push");
    ernic_ptr_vec_free(&vec, NULL);
    check(slots[5] == 1, "ptr reuse: free without a function freed");
}

static void test_ptr_remove(void)
{
    ErnicPtrVec vec;

    ernic_ptr_vec_init(&vec);
    for (unsigned i = 0; i < 5; i++) {
        ernic_ptr_vec_push(&vec, &slots[i]);
    }
    /* A second copy of slot 1, which removing the first leaves */
    ernic_ptr_vec_push(&vec, &slots[1]);

    /* The last item, the second slot 1, takes the first one's place */
    check(ernic_ptr_vec_remove_fast(&vec, &slots[1]), "remove: found");
    check(ernic_ptr_vec_len(&vec) == 5, "remove: len");
    check(ernic_ptr_vec_get(&vec, 1) == &slots[1], "remove: last moved in");
    check(ernic_ptr_vec_remove_fast(&vec, &slots[1]), "remove: second copy");
    check(ernic_ptr_vec_get(&vec, 1) == &slots[4], "remove: next last in");
    check(!ernic_ptr_vec_remove_fast(&vec, &slots[1]), "remove: twice");
    check(!ernic_ptr_vec_remove_fast(&vec, &slots[9]), "remove: absent");
    check(ernic_ptr_vec_len(&vec) == 4, "remove: len after");

    /* [0, 4, 2, 3]: removing index 0 moves 3 there */
    check(ernic_ptr_vec_remove_index_fast(&vec, 0) == &slots[0],
          "remove index: item");
    check(ernic_ptr_vec_get(&vec, 0) == &slots[3], "remove index: moved in");
    /* [3, 4, 2]: removing the last moves nothing */
    check(ernic_ptr_vec_remove_index_fast(&vec, 2) == &slots[2],
          "remove index: last");
    check(ernic_ptr_vec_len(&vec) == 2 &&
              ernic_ptr_vec_get(&vec, 0) == &slots[3] &&
              ernic_ptr_vec_get(&vec, 1) == &slots[4],
          "remove index: the rest");
    check(ernic_ptr_vec_remove_index_fast(&vec, 1) == &slots[4] &&
              ernic_ptr_vec_remove_index_fast(&vec, 0) == &slots[3],
          "remove index: down to empty");
    check(ernic_ptr_vec_len(&vec) == 0, "remove index: empty");
    ernic_ptr_vec_free(&vec, NULL);
}

/* ---- Element vectors ------------------------------------------------- */

static void test_vec_growth(void)
{
    ErnicVec vec;
    bool ok = true;

    ernic_vec_init(&vec, sizeof(Elem3));
    check(ernic_vec_len(&vec) == 0, "vec empty: len");
    for (unsigned i = 0; i < N_SLOTS; i++) {
        Elem3 e = elem3(i);

        ernic_vec_append(&vec, &e);
    }
    check(ernic_vec_len(&vec) == N_SLOTS, "vec growth: len");
    for (unsigned i = 0; i < N_SLOTS; i++) {
        if (!elem3_is(ernic_vec_get(&vec, i), i)) {
            ok = false;
        }
    }
    check(ok, "vec growth: every element in place");

    /* An element is changed in place */
    ((Elem3 *)ernic_vec_get(&vec, 7))->b[0] = 0xEE;
    check(((const Elem3 *)ernic_vec_get(&vec, 7))->b[0] == 0xEE &&
              elem3_is(ernic_vec_get(&vec, 6), 6) &&
              elem3_is(ernic_vec_get(&vec, 8), 8),
          "vec get: changed in place, neighbours alone");

    ernic_vec_clear(&vec);
    check(ernic_vec_len(&vec) == 0, "vec clear: len");
    {
        Elem3 e = elem3(42);

        ernic_vec_append(&vec, &e);
    }
    check(ernic_vec_len(&vec) == 1 && elem3_is(ernic_vec_get(&vec, 0), 42),
          "vec clear: append after");

    ernic_vec_free(&vec);
    check(ernic_vec_len(&vec) == 0, "vec free: len");
    /* The vector is reusable after free, with the same element size */
    {
        Elem3 e = elem3(43);

        ernic_vec_append(&vec, &e);
    }
    check(ernic_vec_len(&vec) == 1 && elem3_is(ernic_vec_get(&vec, 0), 43),
          "vec reuse: append");
    ernic_vec_free(&vec);
}

static void test_vec_copy(void)
{
    ErnicVec src;
    ErnicVec dst;
    ErnicVec empty;
    bool ok = true;

    ernic_vec_init(&src, sizeof(Elem3));
    ernic_vec_init(&dst, sizeof(Elem3));
    ernic_vec_init(&empty, sizeof(Elem3));
    for (unsigned i = 0; i < 100; i++) {
        Elem3 e = elem3(i);

        ernic_vec_append(&src, &e);
    }
    for (unsigned i = 0; i < 3; i++) {
        Elem3 e = elem3(500 + i);

        ernic_vec_append(&dst, &e);
    }

    /* Into a vector too small for it, whose elements it replaces */
    ernic_vec_copy(&dst, &src);
    check(ernic_vec_len(&dst) == 100, "copy: len");
    for (unsigned i = 0; i < 100; i++) {
        if (!elem3_is(ernic_vec_get(&dst, i), i)) {
            ok = false;
        }
    }
    check(ok, "copy: every element");
    check(ernic_vec_get(&dst, 0) != ernic_vec_get(&src, 0),
          "copy: shares the source's storage");

    /* A shorter copy shrinks it */
    ernic_vec_clear(&src);
    {
        Elem3 e = elem3(9);

        ernic_vec_append(&src, &e);
    }
    ernic_vec_copy(&dst, &src);
    check(ernic_vec_len(&dst) == 1 && elem3_is(ernic_vec_get(&dst, 0), 9),
          "copy: shorter");

    /* Onto itself, which changes nothing */
    ernic_vec_copy(&dst, &dst);
    check(ernic_vec_len(&dst) == 1 && elem3_is(ernic_vec_get(&dst, 0), 9),
          "copy: onto itself");

    /* From a vector that never grew */
    ernic_vec_copy(&dst, &empty);
    check(ernic_vec_len(&dst) == 0, "copy: empty");

    ernic_vec_free(&empty);
    ernic_vec_free(&dst);
    ernic_vec_free(&src);
}

/* Handing a vector over by assignment, as ernic_vec.h allows */
static void test_vec_move(void)
{
    ErnicVec a;
    ErnicVec b;
    Elem3 e = elem3(77);

    ernic_vec_init(&a, sizeof(Elem3));
    ernic_vec_append(&a, &e);
    b = a;
    ernic_vec_init(&a, sizeof(Elem3));
    check(ernic_vec_len(&a) == 0, "move: source empty");
    check(ernic_vec_len(&b) == 1 && elem3_is(ernic_vec_get(&b, 0), 77),
          "move: destination");
    ernic_vec_free(&b);
}

/*
 * Appending one of the vector's own elements, through the pointer
 * ernic_vec_get() gave, including each time the vector is full and grows.
 * A growth that freed the old storage before copying the element would read
 * freed memory, which ASan and valgrind report.
 */
static void test_vec_append_own(void)
{
    ErnicVec vec;
    Elem3 e = elem3(1);
    bool ok = true;

    ernic_vec_init(&vec, sizeof(Elem3));
    ernic_vec_append(&vec, &e);
    /* Element i is a copy of element i / 2, so every one is elem3(1) */
    for (size_t i = 1; i < 100; i++) {
        ernic_vec_append(&vec, ernic_vec_get(&vec, i / 2));
    }
    check(ernic_vec_len(&vec) == 100, "append own: len");
    for (size_t i = 0; i < 100; i++) {
        if (!elem3_is(ernic_vec_get(&vec, i), 1)) {
            ok = false;
        }
    }
    check(ok, "append own: every element copied");
    ernic_vec_free(&vec);
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

/* Each frees what it made, so a function that returns doesn't leak */

static void ptr_push_no_memory(void)
{
    ErnicPtrVec vec;

    ernic_ptr_vec_init(&vec);
    fail_next_alloc = true;
    ernic_ptr_vec_push(&vec, NULL);
    ernic_ptr_vec_free(&vec, NULL);
}

static void ptr_get_past_end(void)
{
    ErnicPtrVec vec;

    ernic_ptr_vec_init(&vec);
    ernic_ptr_vec_push(&vec, &slots[0]);
    (void)ernic_ptr_vec_get(&vec, 1);
    ernic_ptr_vec_free(&vec, NULL);
}

static void ptr_remove_index_past_end(void)
{
    ErnicPtrVec vec;

    ernic_ptr_vec_init(&vec);
    (void)ernic_ptr_vec_remove_index_fast(&vec, 0);
    ernic_ptr_vec_free(&vec, NULL);
}

static void vec_append_no_memory(void)
{
    ErnicVec vec;
    Elem3 e = elem3(1);

    ernic_vec_init(&vec, sizeof(Elem3));
    fail_next_alloc = true;
    ernic_vec_append(&vec, &e);
    ernic_vec_free(&vec);
}

static void vec_get_past_end(void)
{
    ErnicVec vec;
    Elem3 e = elem3(1);

    ernic_vec_init(&vec, sizeof(Elem3));
    ernic_vec_append(&vec, &e);
    (void)ernic_vec_get(&vec, 1);
    ernic_vec_free(&vec);
}

static void vec_zero_elem_size(void)
{
    ErnicVec vec;

    ernic_vec_init(&vec, 0);
    ernic_vec_free(&vec);
}

static void vec_copy_sizes_differ(void)
{
    ErnicVec a;
    ErnicVec b;

    ernic_vec_init(&a, sizeof(Elem3));
    ernic_vec_init(&b, sizeof(uint32_t));
    ernic_vec_copy(&a, &b);
    ernic_vec_free(&b);
    ernic_vec_free(&a);
}

int main(void)
{
    test_ptr_empty();
    test_ptr_growth();
    test_ptr_remove();
    test_vec_growth();
    test_vec_copy();
    test_vec_move();
    test_vec_append_own();

    expect_abort(ptr_push_no_memory, "ptr push out of memory");
    expect_abort(ptr_get_past_end, "ptr get past the end");
    expect_abort(ptr_remove_index_past_end, "ptr remove index past the end");
    expect_abort(vec_append_no_memory, "vec append out of memory");
    expect_abort(vec_get_past_end, "vec get past the end");
    expect_abort(vec_zero_elem_size, "vec element size of 0");
    expect_abort(vec_copy_sizes_differ, "vec copy between element sizes");

    if (failures) {
        printf("\n%d failure(s)\n", failures);
    } else {
        printf("all ernic_vec tests passed\n");
    }
    return failures ? 1 : 0;
}
