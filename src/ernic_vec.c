/*
 * Growable arrays.  See ernic_vec.h.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#include "ernic_vec.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ernic_alloc.h"
#include "ernic_log.h"

/* The capacity a vector gets when it first grows */
#define ERNIC_VEC_MIN_CAP 8U

/*
 * The capacity to grow to for @p need elements, from @p cap: double it, or
 * more if that isn't enough.  ernic_xreallocarray() then refuses a capacity
 * whose size doesn't fit in a size_t.
 */
static size_t vec_grown_cap(size_t cap, size_t need)
{
    size_t new_cap = cap > SIZE_MAX / 2 ? SIZE_MAX : cap * 2;

    if (new_cap < need) {
        new_cap = need;
    }
    if (new_cap < ERNIC_VEC_MIN_CAP) {
        new_cap = ERNIC_VEC_MIN_CAP;
    }
    return new_cap;
}

static void index_check(size_t index, size_t len)
{
    if (index >= len) {
        ernic_log_fatal("ernic_vec: index %zu is past the end of %zu", index,
                        len);
    }
}

void ernic_ptr_vec_init(ErnicPtrVec *vec)
{
    vec->items = NULL;
    vec->len = 0;
    vec->cap = 0;
}

void ernic_ptr_vec_free(ErnicPtrVec *vec, ErnicVecFreeFn item_free)
{
    if (item_free) {
        for (size_t i = 0; i < vec->len; i++) {
            item_free(vec->items[i]);
        }
    }
    free((void *)vec->items);
    ernic_ptr_vec_init(vec);
}

void ernic_ptr_vec_push(ErnicPtrVec *vec, void *item)
{
    /* len + 1 can't wrap: len pointers are in memory */
    if (vec->len == vec->cap) {
        size_t cap = vec_grown_cap(vec->cap, vec->len + 1);

        vec->items = (void **)ernic_xreallocarray((void *)vec->items, cap,
                                                  sizeof(*vec->items));
        vec->cap = cap;
    }
    vec->items[vec->len++] = item;
}

size_t ernic_ptr_vec_len(const ErnicPtrVec *vec)
{
    return vec->len;
}

void *ernic_ptr_vec_get(const ErnicPtrVec *vec, size_t index)
{
    index_check(index, vec->len);
    return vec->items[index];
}

bool ernic_ptr_vec_remove_fast(ErnicPtrVec *vec, const void *item)
{
    for (size_t i = 0; i < vec->len; i++) {
        if (vec->items[i] == item) {
            (void)ernic_ptr_vec_remove_index_fast(vec, i);
            return true;
        }
    }
    return false;
}

void *ernic_ptr_vec_remove_index_fast(ErnicPtrVec *vec, size_t index)
{
    void *item;

    index_check(index, vec->len);
    item = vec->items[index];
    vec->items[index] = vec->items[--vec->len];
    return item;
}

void ernic_vec_init(ErnicVec *vec, size_t elem_size)
{
    if (elem_size == 0) {
        ernic_log_fatal("ernic_vec: an element size of 0");
    }
    vec->data = NULL;
    vec->elem_size = elem_size;
    vec->len = 0;
    vec->cap = 0;
}

void ernic_vec_free(ErnicVec *vec)
{
    free(vec->data);
    ernic_vec_init(vec, vec->elem_size);
}

/* Make room for @p need elements in all */
static void vec_reserve(ErnicVec *vec, size_t need)
{
    if (need > vec->cap) {
        size_t cap = vec_grown_cap(vec->cap, need);

        vec->data = ernic_xreallocarray(vec->data, cap, vec->elem_size);
        vec->cap = cap;
    }
}

void ernic_vec_append(ErnicVec *vec, const void *elem)
{
    unsigned char *old = NULL;

    /*
     * Grow into a new block, and free the old one only after the copy:
     * @p elem may be one of the vector's own elements, which realloc() could
     * free before it was read.
     */
    if (vec->len == vec->cap) {
        /* len + 1 can't wrap: len elements of at least a byte are in memory */
        size_t cap = vec_grown_cap(vec->cap, vec->len + 1);
        unsigned char *data = ernic_xreallocarray(NULL, cap, vec->elem_size);

        /* memcpy() from a NULL data, a vector that never grew, is undefined */
        if (vec->len > 0) {
            memcpy(data, vec->data, vec->len * vec->elem_size);
        }
        old = vec->data;
        vec->data = data;
        vec->cap = cap;
    }
    memcpy(vec->data + (vec->len * vec->elem_size), elem, vec->elem_size);
    vec->len++;
    free(old);
}

size_t ernic_vec_len(const ErnicVec *vec)
{
    return vec->len;
}

void *ernic_vec_get(const ErnicVec *vec, size_t index)
{
    index_check(index, vec->len);
    /* Can't wrap: the element is in memory */
    return vec->data + (index * vec->elem_size);
}

void ernic_vec_clear(ErnicVec *vec)
{
    vec->len = 0;
}

void ernic_vec_copy(ErnicVec *dst, const ErnicVec *src)
{
    if (dst->elem_size != src->elem_size) {
        ernic_log_fatal("ernic_vec: copying elements of %zu bytes to a "
                        "vector of %zu",
                        src->elem_size, dst->elem_size);
    }
    if (dst == src) {
        return;
    }
    dst->len = 0;
    vec_reserve(dst, src->len);
    /* memcpy() from a NULL data, a vector that never grew, is undefined */
    if (src->len > 0) {
        memcpy(dst->data, src->data, src->len * src->elem_size);
    }
    dst->len = src->len;
}
