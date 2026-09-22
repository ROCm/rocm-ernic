/*
 * Standalone driver for the libFuzzer harnesses in nix/analysis/fuzz/.
 *
 * The harnesses are normally built and driven by libFuzzer via
 * nix/analysis/fuzz.nix, which runs only in the opt-in fuzz workflow. That
 * left them able to stop compiling -- a changed parser signature under
 * src/from-qemu/utils/ would not be noticed until someone asked for a fuzz
 * run. Linking each harness against this main() instead lets the ordinary
 * CMake build compile them on every PR, so the drift becomes a build
 * failure where it is introduced.
 *
 * -fsanitize=fuzzer is clang-only, but LLVMFuzzerTestOneInput() is just a
 * function taking a buffer, so this driver needs no libFuzzer and builds
 * under gcc as well.
 *
 * With no arguments, replays a fixed set of generated inputs (the build-
 * and-run smoke test ctest registers). With arguments, replays those files
 * instead, which reproduces a crash input saved by a real fuzz run:
 *
 *     ./fuzz_replay_dhcp_server crash-da39a3ee5e6b4b0d
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fuzz_harness.h"

/* Cap on a replayed file. Larger inputs are truncated rather than
 * rejected: the point is to reach the parser, not to be faithful. */
#define FUZZ_REPLAY_MAX_INPUT (1U << 20)

/*
 * Input lengths for the generated smoke run. Chosen to straddle the
 * boundaries the harnessed parsers care about: nothing, a single byte, an
 * Ethernet header, an Ethernet+IP+TCP header, and either side of the
 * 548-byte minimum dhcp_server_process() enforces.
 */
static const size_t generated_sizes[] = {0, 1, 14, 54, 64, 547, 548, 1024};

/*
 * Deterministic filler. A fixed sequence keeps a failure reproducible from
 * the test name alone -- with rand() a red run could not be re-run.
 */
static void fill_pattern(uint8_t *buf, size_t size, uint32_t seed)
{
    uint32_t state = seed * 1664525U + 1013904223U;

    for (size_t i = 0; i < size; i++) {
        state = state * 1664525U + 1013904223U;
        buf[i] = (uint8_t)(state >> 24);
    }
}

/* Hand one input to the harness in an exact-size heap buffer, so ASan
 * traps an over-read even when the harness copies it again. */
static void run_one(const uint8_t *data, size_t size)
{
    uint8_t *buf = malloc(size ? size : 1);

    if (!buf) {
        fprintf(stderr, "fuzz-replay: out of memory\n");
        exit(EXIT_FAILURE);
    }
    if (size) {
        memcpy(buf, data, size);
    }

    (void)LLVMFuzzerTestOneInput(buf, size);

    free(buf);
}

static int replay_file(const char *path)
{
    FILE *f = fopen(path, "rb");

    if (!f) {
        fprintf(stderr, "fuzz-replay: cannot open %s\n", path);
        return -1;
    }

    uint8_t *buf = malloc(FUZZ_REPLAY_MAX_INPUT);
    if (!buf) {
        fprintf(stderr, "fuzz-replay: out of memory\n");
        fclose(f);
        return -1;
    }

    size_t n = fread(buf, 1, FUZZ_REPLAY_MAX_INPUT, f);
    if (ferror(f)) {
        fprintf(stderr, "fuzz-replay: error reading %s\n", path);
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);

    printf("fuzz-replay: %s (%zu bytes)\n", path, n);
    run_one(buf, n);

    free(buf);
    return 0;
}

static int replay_generated(void)
{
    size_t count = sizeof(generated_sizes) / sizeof(generated_sizes[0]);
    size_t largest = 0;

    for (size_t i = 0; i < count; i++) {
        if (generated_sizes[i] > largest) {
            largest = generated_sizes[i];
        }
    }

    uint8_t *buf = malloc(largest ? largest : 1);
    if (!buf) {
        fprintf(stderr, "fuzz-replay: out of memory\n");
        return -1;
    }

    /* Each length twice: all-zero, then the fixed pattern. Zeros reach the
     * "every field is 0" path that a pattern fill never produces. */
    for (size_t i = 0; i < count; i++) {
        size_t size = generated_sizes[i];

        memset(buf, 0, size);
        run_one(buf, size);

        fill_pattern(buf, size, (uint32_t)i);
        run_one(buf, size);
    }

    printf("fuzz-replay: %zu generated inputs OK\n", count * 2);

    free(buf);
    return 0;
}

int main(int argc, char **argv)
{
#ifdef FUZZ_HARNESS_HAS_INITIALIZE
    if (LLVMFuzzerInitialize(&argc, &argv) != 0) {
        fprintf(stderr, "fuzz-replay: harness initialization failed\n");
        return EXIT_FAILURE;
    }
#endif

    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            if (replay_file(argv[i]) != 0) {
                return EXIT_FAILURE;
            }
        }
        return EXIT_SUCCESS;
    }

    return replay_generated() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
