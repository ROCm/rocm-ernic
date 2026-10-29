/*
 * Unit tests for loading a P4 program when memory runs out.
 *
 * A load the engine refuses must say why, and running out of memory is no
 * exception: p4_engine_load_error() is all an operator gets.  The test is
 * linked with --wrap for the allocators (s3_alloc_fail.h) and for
 * json_tokener_new_ex(), whose own allocations happen inside json-c where
 * --wrap can't reach them, so each of the loader's allocations can be
 * failed in turn.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "p4/p4_engine.h"
#include "p4/p4_json.h"
#include "rocm-ernic-warnings.h"
#include "s3_alloc_fail.h"

static int failures;
static bool tokener_fails;

static void check(bool cond, const char *what)
{
    if (!cond) {
        printf("p4_alloc_fail: FAIL %s\n", what);
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
struct json_tokener *__real_json_tokener_new_ex(int depth);
struct json_tokener *__wrap_json_tokener_new_ex(int depth);

struct json_tokener *__wrap_json_tokener_new_ex(int depth)
{
    return tokener_fails ? NULL : __real_json_tokener_new_ex(depth);
}
/* NOLINTEND(misc-use-internal-linkage) */
/* NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp) */
ROCM_ERNIC_WARN_RESERVED_IDENTIFIER_ON

static char *read_fixture(const char *name, size_t *len)
{
    char path[512];
    FILE *f;
    char *buf;
    long n;

    (void)snprintf(path, sizeof(path), "%s/%s", P4_FIXTURE_DIR, name);
    f = fopen(path, "rbe"); /* e: O_CLOEXEC */
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = malloc((size_t)n);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

/*
 * The parser, failing each of its allocations: -ENOMEM, a reason, and the
 * caller's pointer left as it was.
 */
static void test_parse(const char *doc, size_t len)
{
    struct p4_program *prog = NULL;
    struct p4_program *before;
    char why[128];

    /* A real program to stand in *out, so "untouched" means something. */
    check(p4_json_parse(doc, len, &prog, why, sizeof(why)) == 0 && prog,
          "the fixture does not parse");
    before = prog;

    tokener_fails = true;
    check(p4_json_parse(doc, len, &prog, why, sizeof(why)) == -ENOMEM &&
              why[0] != '\0' && prog == before,
          "a parse with no memory for the tokener was not refused with a "
          "reason");
    tokener_fails = false;

    /* The program is the parser's only allocation outside json-c. */
    s3_alloc_fail_arm(0);
    check(p4_json_parse(doc, len, &prog, why, sizeof(why)) == -ENOMEM &&
              why[0] != '\0' && prog == before,
          "a parse with no memory for the program was not refused with a "
          "reason");
    check(s3_alloc_fail_disarm(), "the parse allocated nothing");

    p4_json_free(prog);
}

/* Stage the whole of @doc for a commit. */
static bool stage(struct p4_engine *eng, const char *doc, size_t len)
{
    return p4_engine_load_begin(eng, len) == 0 &&
           p4_engine_load_chunk(eng, 0, doc, len) == 0;
}

/*
 * A begin with no memory for the program's buffer says so, rather than
 * leaving the reason the last load was refused.
 */
static void test_begin(size_t len)
{
    static const char junk[] = "not json";
    struct p4_engine *eng = p4_engine_create();
    const char *why;

    check(eng != NULL, "no engine");
    if (!eng)
        return;

    check(stage(eng, junk, sizeof(junk) - 1u) &&
              p4_engine_load_commit(eng) == -EINVAL &&
              p4_engine_load_error(eng) != NULL,
          "non-JSON was not refused by the parser with a reason");

    s3_alloc_fail_arm(0);
    check(p4_engine_load_begin(eng, len) == -ENOMEM &&
              (why = p4_engine_load_error(eng)) != NULL &&
              strstr(why, "memory") != NULL,
          "a begin with no memory for the program's buffer was not refused "
          "with its own reason");
    check(s3_alloc_fail_disarm(), "the begin allocated nothing");

    p4_engine_destroy(eng);
}

/*
 * What an operator sees: a commit refused for want of memory has a reason,
 * and leaves the running program in place.
 */
static void test_commit(const char *doc, size_t len)
{
    struct p4_engine *eng = p4_engine_create();
    const char *why;

    check(eng != NULL, "no engine");
    if (!eng)
        return;

    check(stage(eng, doc, len) && p4_engine_load_commit(eng) == 0,
          "the fixture does not load");

    check(stage(eng, doc, len), "a load did not begin");
    tokener_fails = true;
    check(p4_engine_load_commit(eng) == -ENOMEM &&
              (why = p4_engine_load_error(eng)) != NULL &&
              strstr(why, "memory") != NULL,
          "a commit with no memory for the tokener was not refused with a "
          "reason");
    tokener_fails = false;
    check(p4_engine_has_program(eng),
          "a commit with no memory for the tokener dropped the running "
          "program");

    /* The program is the commit's first allocation. */
    check(stage(eng, doc, len), "a load did not begin");
    s3_alloc_fail_arm(0);
    check(p4_engine_load_commit(eng) == -ENOMEM &&
              (why = p4_engine_load_error(eng)) != NULL &&
              strstr(why, "memory") != NULL,
          "a commit with no memory for the program was not refused with a "
          "reason");
    check(s3_alloc_fail_disarm(), "the commit allocated nothing");
    check(p4_engine_has_program(eng),
          "a commit with no memory for the program dropped the running "
          "program");

    /* Then its tables, once the program has parsed. */
    check(stage(eng, doc, len), "a load did not begin");
    s3_alloc_fail_arm(1);
    check(p4_engine_load_commit(eng) == -ENOMEM &&
              (why = p4_engine_load_error(eng)) != NULL &&
              strstr(why, "tables") != NULL,
          "a commit with no memory for the program's tables was not refused "
          "with a reason");
    check(s3_alloc_fail_disarm(), "the commit allocated too little");
    check(p4_engine_has_program(eng),
          "a commit with no memory for the program's tables dropped the "
          "running program");

    p4_engine_destroy(eng);
}

int main(void)
{
    size_t len = 0;
    char *doc = read_fixture("minimal_forward.json", &len);

    check(doc != NULL, "minimal_forward.json can't be read");
    if (doc) {
        test_parse(doc, len);
        test_begin(len);
        test_commit(doc, len);
        free(doc);
    }

    if (failures) {
        printf("p4_alloc_fail: %d check(s) FAILED\n", failures);
        return 1;
    }
    printf("p4_alloc_fail: all checks passed\n");
    return 0;
}
