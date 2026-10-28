/*
 * SipHash-2-4, a keyed hash for short inputs
 *
 * SipHash (Aumasson and Bernstein, "SipHash: a fast short-input PRF",
 * 2012) is a pseudorandom function: without the key, its output for one
 * input says nothing about its output for another.  That is what a
 * value an outsider must not predict needs, such as a TCP initial
 * sequence number (RFC 6528), and what a plain hash doesn't give.  It is
 * not a message authentication code for long messages, nor a
 * replacement for a cryptographic hash.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef ERNIC_SIPHASH_H
#define ERNIC_SIPHASH_H

#include <stddef.h>
#include <stdint.h>

#define ERNIC_SIPHASH_KEY_LEN 16

/**
 * ernic_siphash24() - SipHash-2-4 of a byte string
 *
 * @param key  the 16-byte key, as bytes (the reference implementation's
 *             k0 and k1 are read from it little-endian)
 * @param data the bytes to hash; may be NULL if @p len is 0
 * @param len  how many bytes @p data holds
 *
 * Returns the 64-bit result as a number.  The reference implementation
 * writes it out little-endian, so its test vectors are this value's
 * bytes, least significant first.
 */
uint64_t ernic_siphash24(const uint8_t key[ERNIC_SIPHASH_KEY_LEN],
                         const void *data, size_t len);

#endif /* ERNIC_SIPHASH_H */
