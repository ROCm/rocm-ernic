/* Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * semgrep rule test for goto-usage (nix/analysis/semgrep-rules.yaml).
 *
 * Run by nix/analysis/semgrep.nix as `semgrep --test`: a `ruleid:` comment
 * says the next line must be reported, an `ok:` comment says it must not.
 * Each case is labelled with its category and the outcome it expects.
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
