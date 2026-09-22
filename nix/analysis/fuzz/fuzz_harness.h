/*
 * Prototypes for the libFuzzer entry points the harnesses define.
 *
 * libFuzzer calls these by name from its own runtime, so they cannot be
 * static, and without a visible declaration every harness trips
 * -Wmissing-prototypes under the project warning flags (the harnesses are
 * compiled by tests/nix/ as well as by nix/analysis/fuzz.nix).
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#ifndef FUZZ_HARNESS_H
#define FUZZ_HARNESS_H

#include <stddef.h>
#include <stdint.h>

/* Driven once per input. Returns 0; libFuzzer reserves other values. */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/* Optional one-time setup, called before the first input. Only the
 * harnesses that need persistent state define it. */
int LLVMFuzzerInitialize(int *argc, char ***argv);

#endif /* FUZZ_HARNESS_H */
