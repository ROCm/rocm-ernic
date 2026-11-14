/*
 * Growable arrays
 *
 * ErnicPtrVec is an array of pointers, and ErnicVec an array of elements of
 * a size fixed when it is set up, each growing as items are added.  Their
 * storage comes from ernic_alloc.h's functions, so adding to one aborts
 * through ernic_log_fatal() when out of memory rather than fail: they are
 * for lists whose length the server bounds, not a guest or peer.  An index
 * past the end aborts too.
 *
 * A vector holds no lock.  Protect it with the lock of the structure it sits
 * in, as any other field.  Assigning one vector to another hands over its
 * storage; the one assigned from must be set up again before it is used.
 *
 * Copyright (C) Advanced Micro Devices, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE_GPL.md file in the top-level directory.
 */

#ifndef ERNIC_VEC_H
#define ERNIC_VEC_H

#include <stdbool.h>
#include <stddef.h>

/* Frees an item a vector is freed with; see ernic_ptr_vec_free() */
typedef void (*ErnicVecFreeFn)(void *item);

/*
 * An array of pointers.  Set it up with ernic_ptr_vec_init() and touch the
 * fields only through the functions here.
 */
typedef struct ErnicPtrVec {
    void **items; /* NULL until the first push */
    size_t len;
    size_t cap; /* how many items fit in items */
} ErnicPtrVec;

/*
 * An array of elements of one size.  Set it up with ernic_vec_init() and
 * touch the fields only through the functions here.
 */
typedef struct ErnicVec {
    unsigned char *data; /* NULL until the first element is added */
    size_t elem_size;
    size_t len;
    size_t cap; /* how many elements fit in data */
} ErnicVec;

/**
 * Set up an empty pointer vector
 *
 * @param vec the vector
 */
void ernic_ptr_vec_init(ErnicPtrVec *vec);

/**
 * Free a pointer vector's storage, passing each item to @p item_free
 *
 * @param vec       the vector
 * @param item_free called on each item, NULL ones included, in order, or
 *                  NULL to leave the items alone
 *
 * The vector is left empty, as ernic_ptr_vec_init() leaves it, so it may be
 * used again.
 */
void ernic_ptr_vec_free(ErnicPtrVec *vec, ErnicVecFreeFn item_free);

/**
 * Add an item at the end
 *
 * @param vec  the vector
 * @param item the item, which may be NULL
 *
 * Aborts through ernic_log_fatal() when out of memory.
 */
void ernic_ptr_vec_push(ErnicPtrVec *vec, void *item);

/**
 * How many items the vector holds
 *
 * @param vec the vector
 */
size_t ernic_ptr_vec_len(const ErnicPtrVec *vec);

/**
 * An item
 *
 * @param vec   the vector
 * @param index the item's index, which must be less than its length
 *
 * Returns the item.  Aborts through ernic_log_fatal() if @p index is past the
 * end.
 */
void *ernic_ptr_vec_get(const ErnicPtrVec *vec, size_t index);

/**
 * Remove the first item equal to @p item, moving the last into its place
 *
 * @param vec  the vector
 * @param item the item
 *
 * Returns whether the vector held @p item.  The order of the rest changes.
 */
bool ernic_ptr_vec_remove_fast(ErnicPtrVec *vec, const void *item);

/**
 * Remove an item, moving the last into its place
 *
 * @param vec   the vector
 * @param index the item's index, which must be less than its length
 *
 * Returns the item removed.  Aborts through ernic_log_fatal() if @p index is
 * past the end.
 */
void *ernic_ptr_vec_remove_index_fast(ErnicPtrVec *vec, size_t index);

/**
 * Set up an empty vector
 *
 * @param vec       the vector
 * @param elem_size the size of each element, which must not be 0
 *
 * Aborts through ernic_log_fatal() if @p elem_size is 0.
 */
void ernic_vec_init(ErnicVec *vec, size_t elem_size);

/**
 * Free a vector's storage
 *
 * @param vec the vector
 *
 * The vector is left empty, with the same element size, so it may be used
 * again.
 */
void ernic_vec_free(ErnicVec *vec);

/**
 * Copy an element to the end
 *
 * @param vec  the vector
 * @param elem the element, of the vector's element size, which may be one of
 *             the vector's own
 *
 * Aborts through ernic_log_fatal() when out of memory.
 */
void ernic_vec_append(ErnicVec *vec, const void *elem);

/**
 * How many elements the vector holds
 *
 * @param vec the vector
 */
size_t ernic_vec_len(const ErnicVec *vec);

/**
 * An element, in place
 *
 * @param vec   the vector
 * @param index the element's index, which must be less than its length
 *
 * Returns a pointer to the element, which stays valid until the vector next
 * grows or is freed.  Aborts through ernic_log_fatal() if @p index is past
 * the end.
 */
void *ernic_vec_get(const ErnicVec *vec, size_t index);

/**
 * Remove every element, keeping the storage for the next
 *
 * @param vec the vector
 */
void ernic_vec_clear(ErnicVec *vec);

/**
 * Make @p dst a copy of @p src
 *
 * @param dst the vector to replace the elements of
 * @param src the vector to copy, with the same element size as @p dst
 *
 * Aborts through ernic_log_fatal() when out of memory, or if the element
 * sizes differ.
 */
void ernic_vec_copy(ErnicVec *dst, const ErnicVec *src);

#endif /* ERNIC_VEC_H */
