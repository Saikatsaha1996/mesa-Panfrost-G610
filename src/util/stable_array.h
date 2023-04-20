/*
 * Copyright (C) 2022 Icecream95 <ixn@disroot.org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef STABLE_ARRAY_H
#define STABLE_ARRAY_H

#include "util/simple_mtx.h"
#include "util/u_math.h"

/* A thread-safe automatically growing array where elements have stable locations
 *
 * This data structure has these properties:
 *
 *  1. Accessing an element is constant time (if allocation is not required).
 *
 *  2. Elements are not moved in memory, so it is safe to store a pointer to
 *     something in a stable_array.
 *
 *  3. The data structure is thread-safe. To improve performance, there is
 *     also a fast path that does not require atomics.
 *
 *  4. Although the data structure is not lock-free, there is a limit on the
 *     number of times that a lock is ever acquired--a maximum of 32 times the
 *     number of accessing threads. In practice, contention will never be an
 *     issue for long-lived stable_arrays.
 *
 *  5. Memory usage is similar to util_dynarray, with each allocation being
 *     twice as large as the last. Freeing buckets is currently never done.
 *
 * The data structure is faster than util_sparse_array, but is not sparse.
 */

struct stable_array
{
   uint8_t *buckets[32];
   simple_mtx_t lock;
   size_t eltsize;
};

static inline void
stable_array_init_bytes(struct stable_array *buf, size_t eltsize)
{
   memset(buf, 0, sizeof(*buf));
   buf->eltsize = eltsize;
   simple_mtx_init(&buf->lock, mtx_plain);
}

static inline void
stable_array_fini(struct stable_array *buf)
{
   simple_mtx_destroy(&buf->lock);
   for (unsigned i = 0; i < ARRAY_SIZE(buf->buckets); ++i) {
      if (buf->buckets[i])
         free(buf->buckets[i]);
   }
}

struct stable_array_index
{
   unsigned bucket;
   unsigned idx;
};

static inline struct stable_array_index
stable_array_get_index(unsigned idx)
{
   struct stable_array_index i = {0};
   i.bucket = util_logbase2(idx);
   i.idx = i.bucket ? (idx -= (1 << i.bucket)) : idx;
   return i;
}

static inline void *
stable_array_get_bytes(struct stable_array *buf, unsigned idx, size_t eltsize)
{
   assert(eltsize == buf->eltsize);

   struct stable_array_index i = stable_array_get_index(idx);

   uint8_t *bucket = p_atomic_read(&buf->buckets[i.bucket]);

   if (!bucket) {
      simple_mtx_lock(&buf->lock);
      bucket = buf->buckets[i.bucket];

      if (!bucket) {
         /* The first two buckets both have two elements */
         bucket = (uint8_t *)calloc(1U << MAX2(i.bucket, 1), eltsize);

         p_atomic_set(&buf->buckets[i.bucket], bucket);
      }
      simple_mtx_unlock(&buf->lock);
   }

   return bucket + eltsize * i.idx;
}

static inline void *
stable_array_get_existing_bytes(struct stable_array *buf, unsigned idx, size_t eltsize)
{
   assert(eltsize == buf->eltsize);

   struct stable_array_index i = stable_array_get_index(idx);

   return buf->buckets[i.bucket] + eltsize * i.idx;
}

#define stable_array_init(buf, type) stable_array_init_bytes((buf), sizeof(type))
#define stable_array_get(buf, type, idx) ((type*)stable_array_get_bytes((buf), (idx), sizeof(type)))
#define stable_array_get_existing(buf, type, idx) ((type*)stable_array_get_existing_bytes((buf), (idx), sizeof(type)))

#endif
