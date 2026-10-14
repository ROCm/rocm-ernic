/*
 * semgrep rule test for malloc-no-null-check (nix/analysis/semgrep-rules.yaml).
 *
 * Run by nix/analysis/semgrep.nix as `semgrep --test`: a `ruleid:` comment
 * says the next line must be reported, an `ok:` comment says it must not.
 * Each case is labelled with its category and the outcome it expects.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <stdlib.h>

struct item {
    int field;
};

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
