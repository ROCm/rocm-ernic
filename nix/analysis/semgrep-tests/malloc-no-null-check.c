/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * semgrep rule test for malloc-no-null-check (nix/analysis/semgrep-rules.yaml).
 *
 * Run by nix/analysis/semgrep.nix as `semgrep --test`: a `ruleid:` comment
 * says the next line must be reported, an `ok:` comment says it must not.
 * Each case is labelled with its category and the outcome it expects.
 * semgrep --test checks every rule against this file, so the gotos in the
 * goto guards carry goto-usage annotations too.
 */

#include <stdlib.h>

struct item {
    int field;
};

void report_oom(void);

/* positive: assigned, then dereferenced with no check -> reported */
void assign_then_deref(void)
{
    int *p;

    p = malloc(sizeof(*p));
    // ruleid: malloc-no-null-check
    *p = 1;
}

/* positive: declared with malloc, then used through -> -> reported */
void declare_then_arrow(void)
{
    struct item *it = malloc(sizeof(*it));

    // ruleid: malloc-no-null-check
    it->field = 1;
}

/* negative: an early return on !p guards the use -> not reported */
int guarded_by_early_return(void)
{
    int *p = malloc(8 * sizeof(*p));

    if (!p)
        return -1;
    // ok: malloc-no-null-check
    p[0] = 1;
    free(p);
    return 0;
}

/* negative: a braced p == NULL check guards the use -> not reported */
int guarded_by_null_compare(void)
{
    int *p = malloc(sizeof(*p));

    if (p == NULL) {
        return -1;
    }
    // ok: malloc-no-null-check
    *p = 2;
    free(p);
    return 0;
}

/* negative: a guard that logs, then returns -> not reported */
int guarded_by_block(void)
{
    struct item *it = malloc(sizeof(*it));

    if (it == NULL) {
        report_oom();
        return -1;
    }
    // ok: malloc-no-null-check
    it->field = 3;
    free(it);
    return 0;
}

/* negative: a guard with a bare return -> not reported */
void guarded_by_bare_return(void)
{
    int *p = malloc(sizeof(*p));

    if (!p)
        return;
    // ok: malloc-no-null-check
    *p = 4;
    free(p);
}

/* negative: a guard that jumps to a cleanup label -> not reported */
int guarded_by_goto(void)
{
    int *p = malloc(sizeof(*p));
    int ret = -1;

    if (!p)
        // ruleid: goto-usage
        goto out;
    // ok: malloc-no-null-check
    *p = 5;
    ret = 0;
    free(p);
out:
    return ret;
}

/* negative: a braced p == NULL guard that logs, then jumps -> not reported */
int guarded_by_block_goto(void)
{
    int *p = malloc(sizeof(*p));
    int ret = -1;

    if (p == NULL) {
        report_oom();
        // ruleid: goto-usage
        goto out;
    }
    // ok: malloc-no-null-check
    p[0] = 6;
    ret = 0;
    free(p);
out:
    return ret;
}

/* negative: a guard that ends the process -> not reported */
void guarded_by_exit(void)
{
    int *p = malloc(sizeof(*p));

    if (!p)
        exit(EXIT_FAILURE);
    // ok: malloc-no-null-check
    *p = 7;
    free(p);
}

/* negative: a guard that aborts -> not reported */
void guarded_by_abort(void)
{
    int *p = malloc(sizeof(*p));

    if (p == NULL)
        abort();
    // ok: malloc-no-null-check
    *p = 8;
    free(p);
}

/* corner: a NULL check that only logs and falls through -> reported */
void logged_but_not_guarded(void)
{
    int *p = malloc(sizeof(*p));

    if (!p) {
        report_oom();
    }
    // ruleid: malloc-no-null-check
    *p = 9;
    free(p);
}

/* corner: the guard tests a different pointer -> reported */
void guard_on_other_pointer(const int *q)
{
    int *p = malloc(sizeof(*p));

    if (!q)
        return;
    // ruleid: malloc-no-null-check
    *p = 10;
    free(p);
}

/* boundary: the use comes one line before the check -> reported */
int used_before_check(void)
{
    int *p = malloc(sizeof(*p));

    // ruleid: malloc-no-null-check
    p[0] = 3;
    if (!p)
        return -1;
    free(p);
    return 0;
}

/* corner: the use is inside the if that tests the pointer -> not reported */
void used_inside_guard(void)
{
    int *p = malloc(sizeof(*p));

    if (p) {
        // ok: malloc-no-null-check
        *p = 4;
        free(p);
    }
}

/* corner: calloc is outside the rule's scope -> not reported */
void calloc_not_covered(void)
{
    int *p = calloc(1, sizeof(*p));

    // ok: malloc-no-null-check
    *p = 5;
}

/* corner: observing a pointer is not a guard for later use -> reported */
void observed_then_used(void)
{
    int *p = malloc(sizeof(*p));

    if (p) {
        (void)p;
    }
    // ruleid: malloc-no-null-check
    *p = 6;
    free(p);
}

/* corner: returning on success leaves NULL for the use -> reported */
void reversed_guard(void)
{
    int *p = malloc(sizeof(*p));

    if (p)
        return;
    // ruleid: malloc-no-null-check
    *p = 7;
}
