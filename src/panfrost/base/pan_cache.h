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

#ifndef PAN_CACHE_H
#define PAN_CACHE_H

#ifdef __aarch64__

static void
cache_clean(volatile void *addr)
{
        __asm__ volatile ("dc cvac, %0" :: "r" (addr) : "memory");
}

static void
cache_invalidate(volatile void *addr)
{
        __asm__ volatile ("dc civac, %0" :: "r" (addr) : "memory");
}

typedef void (*cacheline_op)(volatile void *addr);

#define CACHELINE_SIZE 64

static void
cacheline_op_range(volatile void *start, size_t length, cacheline_op op)
{
        volatile void *ptr = (volatile void *)((uintptr_t) start & ~((uintptr_t) CACHELINE_SIZE - 1));
        volatile void *end = (volatile void *) ALIGN_POT((uintptr_t) start + length, CACHELINE_SIZE);
        for (; ptr < end; ptr += CACHELINE_SIZE)
                op(ptr);
}

static void
cache_clean_range(volatile void *start, size_t length)
{
        /* TODO: Do an invalidate at the start of the range? */
        cacheline_op_range(start, length, cache_clean);
}

static void
cache_invalidate_range(volatile void *start, size_t length)
{
        cacheline_op_range(start, length, cache_invalidate);
}

#endif /* __aarch64__ */

/* The #ifdef covers both 32-bit and 64-bit ARM */
#ifdef __ARM_ARCH
static void
cache_barrier(void)
{
        __asm__ volatile ("dsb sy" ::: "memory");
}

static void
memory_barrier(void)
{
        __asm__ volatile ("dmb sy" ::: "memory");
}
#else

/* TODO: How to do cache barriers when emulated? */
static void
cache_barrier(void)
{
}

static void
memory_barrier(void)
{
}
#endif
#endif
