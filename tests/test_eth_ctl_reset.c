/*
 * Unit test for the ROCM_ERNIC_ETH_CTL_RESET bit definition.
 *
 * Regression coverage for signed-shift undefined behaviour: the macro was
 * defined as (1 << 31), which shifts into the sign bit of a signed int
 * (UB in C). The _Generic check below fails to build against the old
 * signed form: (1 << 31) has type int, so it takes the "default" branch
 * and the assertion fails. The fixed (1u << 31) has type unsigned int and
 * the value 0x80000000.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdio.h>

#include "rocm_ernic_eth.h"

/* Correct bit pattern regardless of signedness. */
_Static_assert(ROCM_ERNIC_ETH_CTL_RESET == 0x80000000u,
               "ROCM_ERNIC_ETH_CTL_RESET must be bit 31");

/* Must be an unsigned constant: a signed (1 << 31) shifts into the sign
 * bit (UB). Assert the type directly so this holds regardless of value and
 * without tripping -Wtype-limits. */
_Static_assert(_Generic(ROCM_ERNIC_ETH_CTL_RESET, unsigned int: 1, default: 0),
               "ROCM_ERNIC_ETH_CTL_RESET must be unsigned (use 1u << 31)");

int main(void)
{
    int failures = 0;

    /* The reset bit sets bit 31 and nothing else. */
    uint32_t ctl = 0;
    ctl |= ROCM_ERNIC_ETH_CTL_RESET;
    if (ctl != 0x80000000u) {
        printf("FAIL: set reset bit => %#x\n", ctl);
        failures++;
    }
    if (!(ctl & ROCM_ERNIC_ETH_CTL_RESET)) {
        printf("FAIL: reset bit not detected\n");
        failures++;
    }

    /* Clearing via ~MASK leaves the low bits and clears bit 31. */
    ctl = 0xFFFFFFFFu;
    ctl &= ~ROCM_ERNIC_ETH_CTL_RESET;
    if (ctl != 0x7FFFFFFFu) {
        printf("FAIL: clear reset bit => %#x\n", ctl);
        failures++;
    }

    if (failures) {
        printf("eth_ctl_reset: %d check(s) FAILED\n", failures);
        return 1;
    }
    printf("eth_ctl_reset: all checks passed\n");
    return 0;
}
