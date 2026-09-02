/*
 * Unit tests for the strict integer parsers in parse_int.h.
 *
 * These replace atoi() in the TCP backend's env and port parsing. Unlike
 * atoi(), they must reject empty input, trailing garbage, and out-of-range
 * values rather than truncating or returning 0.
 *
 * SPDX-License-Identifier: MIT
 */

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "parse_int.h"

struct int_case {
    const char *name;
    const char *input; /* NULL exercises the NULL path */
    bool expect_ok;
    int expect_val; /* checked only when expect_ok */
};

struct port_case {
    const char *name;
    const char *input;
    bool expect_ok;
    uint16_t expect_val;
};

static int run_int(const struct int_case *tc)
{
    int v = 0xDEAD;
    bool ok = ernic_parse_int(tc->input, &v);
    if (ok != tc->expect_ok) {
        printf("FAIL int %-16s: ok=%d expected %d\n", tc->name, ok,
               tc->expect_ok);
        return 1;
    }
    if (ok && v != tc->expect_val) {
        printf("FAIL int %-16s: val=%d expected %d\n", tc->name, v,
               tc->expect_val);
        return 1;
    }
    return 0;
}

static int run_port(const struct port_case *tc)
{
    uint16_t p = 0xBEEF;
    bool ok = ernic_parse_port(tc->input, &p);
    if (ok != tc->expect_ok) {
        printf("FAIL port %-16s: ok=%d expected %d\n", tc->name, ok,
               tc->expect_ok);
        return 1;
    }
    if (ok && p != tc->expect_val) {
        printf("FAIL port %-16s: val=%u expected %u\n", tc->name, (unsigned)p,
               (unsigned)tc->expect_val);
        return 1;
    }
    return 0;
}

int main(void)
{
    static const struct int_case int_cases[] = {
        {"null", NULL, false, 0},
        {"empty", "", false, 0},
        {"valid", "1234", true, 1234},
        {"zero", "0", true, 0},
        {"negative", "-5", true, -5},
        {"trailing-garbage", "123abc", false, 0},
        {"non-numeric", "abc", false, 0},
        {"trailing-space", "123 ", false, 0},
        {"overflow", "99999999999999999999", false, 0},
        {"int-max", "2147483647", true, INT_MAX},
    };

    static const struct port_case port_cases[] = {
        {"valid", "8080", true, 8080},    {"min", "1", true, 1},
        {"max", "65535", true, 65535},    {"zero", "0", false, 0},
        {"too-large", "65536", false, 0}, {"way-too-large", "99999", false, 0},
        {"negative", "-1", false, 0},     {"garbage", "80x", false, 0},
        {"empty", "", false, 0},
    };

    int failures = 0;
    size_t ni = sizeof(int_cases) / sizeof(int_cases[0]);
    size_t np = sizeof(port_cases) / sizeof(port_cases[0]);
    for (size_t i = 0; i < ni; i++) {
        failures += run_int(&int_cases[i]);
    }
    for (size_t i = 0; i < np; i++) {
        failures += run_port(&port_cases[i]);
    }

    if (failures) {
        printf("parse_int: %d case(s) FAILED\n", failures);
        return 1;
    }
    printf("parse_int: all %zu cases passed\n", ni + np);
    return 0;
}
