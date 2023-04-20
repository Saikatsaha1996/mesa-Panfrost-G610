/*
 * Copyright (C) 2022 Icecream95
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

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <string.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <poll.h>
#include <pthread.h>

#include "util/macros.h"
#include "pan_base.h"

#include "mali_kbase_ioctl.h"

bool
kbase_open(kbase k, int fd, unsigned cs_queue_count, bool verbose)
{
        *k = (struct kbase_) {0};
        k->fd = fd;
        k->cs_queue_count = cs_queue_count;
        k->page_size = sysconf(_SC_PAGE_SIZE);
        k->verbose = verbose;

        if (k->fd == -1)
           return kbase_open_csf_noop(k);

        struct kbase_ioctl_version_check ver = { 0 };

        if (ioctl(k->fd, KBASE_IOCTL_VERSION_CHECK_RESERVED, &ver) == 0) {
                return kbase_open_csf(k);
        } else if (ioctl(k->fd, KBASE_IOCTL_VERSION_CHECK, &ver) == 0) {
                if (ver.major == 3)
                        return kbase_open_old(k);
                else
                        return kbase_open_new(k);
        }

        return false;
}

/* If fd != -1, ownership is passed in */
int
kbase_alloc_gem_handle_locked(kbase k, base_va va, int fd)
{
        kbase_handle h = {
                .va = va,
                .fd = fd
        };

        unsigned size = util_dynarray_num_elements(&k->gem_handles, kbase_handle);

        kbase_handle *handles = util_dynarray_begin(&k->gem_handles);

        for (unsigned i = 0; i < size; ++i) {
                if (handles[i].fd == -2) {
                        handles[i] = h;
                        return i;
                }
        }

        util_dynarray_append(&k->gem_handles, kbase_handle, h);

        return size;
}

int
kbase_alloc_gem_handle(kbase k, base_va va, int fd)
{
        pthread_mutex_lock(&k->handle_lock);

        int ret = kbase_alloc_gem_handle_locked(k, va, fd);

        pthread_mutex_unlock(&k->handle_lock);

        return ret;
}

void
kbase_free_gem_handle(kbase k, int handle)
{
        pthread_mutex_lock(&k->handle_lock);

        unsigned size = util_dynarray_num_elements(&k->gem_handles, kbase_handle);

        int fd;

        if (handle >= size) {
                pthread_mutex_unlock(&k->handle_lock);
                return;
        }

        if (handle + 1 < size) {
                kbase_handle *ptr = util_dynarray_element(&k->gem_handles, kbase_handle, handle);
                fd = ptr->fd;
                ptr->fd = -2;
        } else {
                fd = (util_dynarray_pop(&k->gem_handles, kbase_handle)).fd;
        }

        if (fd != -1)
                close(fd);

        pthread_mutex_unlock(&k->handle_lock);
}

kbase_handle
kbase_gem_handle_get(kbase k, int handle)
{
        kbase_handle h = { .fd = -1 };

        pthread_mutex_lock(&k->handle_lock);

        unsigned size = util_dynarray_num_elements(&k->gem_handles, kbase_handle);

        if (handle < size)
                h = *util_dynarray_element(&k->gem_handles, kbase_handle, handle);

        pthread_mutex_unlock(&k->handle_lock);

        return h;
}

int
kbase_wait_bo(kbase k, int handle, int64_t timeout_ns, bool wait_readers)
{
        struct kbase_wait_ctx wait = kbase_wait_init(k, timeout_ns);

        while (kbase_wait_for_event(&wait)) {
                pthread_mutex_lock(&k->handle_lock);
                if (handle >= util_dynarray_num_elements(&k->gem_handles, kbase_handle)) {
                        pthread_mutex_unlock(&k->handle_lock);
                        kbase_wait_fini(wait);
                        errno = EINVAL;
                        return -1;
                }
                kbase_handle *ptr = util_dynarray_element(&k->gem_handles, kbase_handle, handle);
                if (!ptr->use_count) {
                        pthread_mutex_unlock(&k->handle_lock);
                        kbase_wait_fini(wait);
                        return 0;
                }
                pthread_mutex_unlock(&k->handle_lock);
        }

        kbase_wait_fini(wait);
        errno = ETIMEDOUT;
        return -1;
}

static void
adjust_time(struct timespec *tp, int64_t ns)
{
        ns += tp->tv_nsec;
        tp->tv_nsec = ns % 1000000000;
        tp->tv_sec += ns / 1000000000;
}

static int64_t
ns_until(struct timespec tp)
{
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        int64_t sec = (tp.tv_sec - now.tv_sec) * 1000000000;
        int64_t ns = tp.tv_nsec - now.tv_nsec;

        /* Clamp the value to zero to avoid errors from ppoll */
        return MAX2(sec + ns, 0);
}

static void
kbase_wait_signal(kbase k)
{
        /* We must acquire the event condition lock, otherwise another
         * thread could be between the trylock and the cond_wait, and
         * not notice the broadcast. */
        pthread_mutex_lock(&k->event_cnd_lock);
        pthread_cond_broadcast(&k->event_cnd);
        pthread_mutex_unlock(&k->event_cnd_lock);
}

struct kbase_wait_ctx
kbase_wait_init(kbase k, int64_t timeout_ns)
{
        struct timespec tp;
        clock_gettime(CLOCK_MONOTONIC, &tp);

        adjust_time(&tp, timeout_ns);

        return (struct kbase_wait_ctx) {
                .k = k,
                .until = tp,
        };
}

bool
kbase_wait_for_event(struct kbase_wait_ctx *ctx)
{
        kbase k = ctx->k;

        /* Return instantly the first time so that a check outside the
         * wait_for_Event loop is not required */
        if (!ctx->has_cnd_lock) {
                pthread_mutex_lock(&k->event_cnd_lock);
                ctx->has_cnd_lock = true;
                return true;
        }

        if (!ctx->has_lock) {
                if (pthread_mutex_trylock(&k->event_read_lock) == 0) {
                        ctx->has_lock = true;
                        pthread_mutex_unlock(&k->event_cnd_lock);
                } else {
                        int ret = pthread_cond_timedwait(&k->event_cnd,
                                         &k->event_cnd_lock, &ctx->until);
                        return ret != ETIMEDOUT;
                }
        }

        bool event = k->poll_event(k, ns_until(ctx->until));
        k->handle_events(k);
        kbase_wait_signal(k);
        return event;
}

void
kbase_wait_fini(struct kbase_wait_ctx ctx)
{
        kbase k = ctx.k;

        if (ctx.has_lock) {
                pthread_mutex_unlock(&k->event_read_lock);
                kbase_wait_signal(k);
        } else if (ctx.has_cnd_lock) {
                pthread_mutex_unlock(&k->event_cnd_lock);
        }
}

void
kbase_ensure_handle_events(kbase k)
{
        /* If we don't manage to take the lock, then events have recently/will
         * soon be handled, there is no need to do anything. */
        if (pthread_mutex_trylock(&k->event_read_lock) == 0) {
                k->handle_events(k);
                pthread_mutex_unlock(&k->event_read_lock);
                kbase_wait_signal(k);
        }
}

bool
kbase_poll_fd_until(int fd, bool wait_shared, struct timespec tp)
{
        struct pollfd pfd = {
                .fd = fd,
                .events = wait_shared ? POLLOUT : POLLIN,
        };

        uint64_t timeout = ns_until(tp);

        struct timespec t = {
                .tv_sec = timeout / 1000000000,
                .tv_nsec = timeout % 1000000000,
        };

        int ret = ppoll(&pfd, 1, &t, NULL);

        if (ret == -1 && errno != EINTR)
                perror("kbase_poll_fd_until");

        return ret != 0;
}
