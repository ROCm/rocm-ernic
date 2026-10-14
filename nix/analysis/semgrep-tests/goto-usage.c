/*
 * semgrep rule test for goto-usage (nix/analysis/semgrep-rules.yaml).
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

/* positive: a goto to a cleanup label -> reported */
int goto_cleanup(int fail)
{
    if (fail)
        // ruleid: goto-usage
        goto out;
    return 0;
out:
    return 1;
}

/* negative: a label with no goto (a switch case) -> not reported */
int switch_labels(int v)
{
    switch (v) {
    // ok: goto-usage
    case 1:
        return 1;
    default:
        return 0;
    }
}

/* boundary: two gotos to the same label -> both reported */
int two_gotos(int a, int b)
{
    if (a)
        // ruleid: goto-usage
        goto done;
    if (b)
        // ruleid: goto-usage
        goto done;
    return 0;
done:
    return 1;
}

/* corner: a goto that jumps backwards -> reported */
int backwards(int n)
{
again:
    n--;
    if (n > 0)
        // ruleid: goto-usage
        goto again;
    return n;
}
