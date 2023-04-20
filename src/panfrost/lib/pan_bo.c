/*
 * Copyright 2019 Collabora, Ltd.
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
 *
 * Authors (Collabora):
 *   Alyssa Rosenzweig <alyssa.rosenzweig@collabora.com>
 */
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <xf86drm.h>
#include <pthread.h>
#include "drm-uapi/panfrost_drm.h"

#include "pan_bo.h"
#include "pan_device.h"
#include "pan_util.h"
#include "wrap.h"

#include "util/os_mman.h"

#include "util/u_inlines.h"
#include "util/u_math.h"
#include "util/os_file.h"

/* This file implements a userspace BO cache. Allocating and freeing
 * GPU-visible buffers is very expensive, and even the extra kernel roundtrips
 * adds more work than we would like at this point. So caching BOs in userspace
 * solves both of these problems and does not require kernel updates.
 *
 * Cached BOs are sorted into a bucket based on rounding their size down to the
 * nearest power-of-two. Each bucket contains a linked list of free panfrost_bo
 * objects. Putting a BO into the cache is accomplished by adding it to the
 * corresponding bucket. Getting a BO from the cache consists of finding the
 * appropriate bucket and sorting. A cache eviction is a kernel-level free of a
 * BO and removing it from the bucket. We special case evicting all BOs from
 * the cache, since that's what helpful in practice and avoids extra logic
 * around the linked list.
 */

static struct panfrost_bo *
panfrost_bo_alloc(struct panfrost_device *dev, size_t size,
                  uint32_t flags, const char *label)
{
        struct drm_panfrost_create_bo create_bo = { .size = size };
        struct panfrost_bo *bo;
        int ret;

        if (dev->kernel_version->version_major > 1 ||
            dev->kernel_version->version_minor >= 1) {
                if (flags & PAN_BO_GROWABLE)
                        create_bo.flags |= PANFROST_BO_HEAP;
                if (!(flags & PAN_BO_EXECUTE))
                        create_bo.flags |= PANFROST_BO_NOEXEC;
        }

        void *cpu = NULL;

        bool cached = false;

        if (dev->kbase) {
                if (flags & PAN_BO_CACHEABLE) {
                        if (!(dev->debug & PAN_DBG_UNCACHED_CPU)) {
                                create_bo.flags |= MALI_BO_CACHED_CPU;
                                /* TODO: What if kbase decides not to cache it? */
                                cached = true;
                        }
                        if (dev->debug & PAN_DBG_UNCACHED_GPU)
                                create_bo.flags |= MALI_BO_UNCACHED_GPU;
                }

                unsigned mali_flags = (flags & PAN_BO_EVENT) ? 0x8200f : 0;

                struct base_ptr p = dev->mali.alloc(&dev->mali, size, create_bo.flags, mali_flags);

                if (p.gpu) {
                        cpu = p.cpu;
                        create_bo.offset = p.gpu;
                        create_bo.handle = kbase_alloc_gem_handle(&dev->mali, p.gpu, -1);
                        if (!cpu)
                                abort();
                        ret = 0;
                } else {
                        ret = -1;
                }
        } else {
                ret = drmIoctl(dev->fd, DRM_IOCTL_PANFROST_CREATE_BO, &create_bo);
        }
        if (ret) {
                fprintf(stderr, "DRM_IOCTL_PANFROST_CREATE_BO failed: %m\n");
                return NULL;
        }

        bo = pan_lookup_bo(dev, create_bo.handle);
        assert(!memcmp(bo, &((struct panfrost_bo){}), sizeof(*bo)));

        bo->size = create_bo.size;
        bo->ptr.gpu = create_bo.offset;
        bo->ptr.cpu = cpu;
        if ((uintptr_t) bo->ptr.cpu != bo->ptr.gpu)
                bo->free_ioctl = true;
        bo->gem_handle = create_bo.handle;
        bo->flags = flags;
        bo->dev = dev;
        bo->label = label;
        bo->cached = cached;
        bo->dmabuf_fd = -1;
        return bo;
}

static void
panfrost_bo_free(struct panfrost_bo *bo)
{
        struct panfrost_device *dev = bo->dev;
        struct drm_gem_close gem_close = { .handle = bo->gem_handle };
        int ret;

        if (dev->bo_log) {
                int fd = kbase_gem_handle_get(&dev->mali, bo->gem_handle).fd;

                struct timespec tp;
                clock_gettime(CLOCK_MONOTONIC_RAW, &tp);
                fprintf(dev->bo_log, "%"PRIu64".%09li memfree %"PRIx64" to %"PRIx64" size %zu label %s obj (%p,%i,%i)\n",
                        (uint64_t) tp.tv_sec, tp.tv_nsec, bo->ptr.gpu, bo->ptr.gpu + bo->size, bo->size, bo->label,
                        bo, bo->gem_handle, fd);
                fflush(NULL);
        }

        if (dev->kbase) {
                os_munmap(bo->ptr.cpu, bo->size);
                if (bo->munmap_ptr)
                        os_munmap(bo->munmap_ptr, bo->size);
                if (bo->free_ioctl)
                        dev->mali.free(&dev->mali, bo->ptr.gpu);
                kbase_free_gem_handle(&dev->mali, bo->gem_handle);
                ret = 0;
        } else {
                ret = drmIoctl(bo->dev->fd, DRM_IOCTL_GEM_CLOSE, &gem_close);
        }
        if (ret) {
                fprintf(stderr, "DRM_IOCTL_GEM_CLOSE failed: %m\n");
                assert(0);
        }

        /* BO will be freed with the stable_array, but zero to indicate free */
        memset(bo, 0, sizeof(*bo));
}

static bool
panfrost_bo_usage_finished(struct panfrost_bo *bo, bool readers)
{
        struct panfrost_device *dev = bo->dev;
        kbase k = &dev->mali;

        bool ret = true;

        pthread_mutex_lock(&dev->bo_usage_lock);
        pthread_mutex_lock(&dev->mali.queue_lock);

        util_dynarray_foreach(&bo->usage, struct panfrost_usage, u) {
                /* Skip if we are only waiting for writers */
                if (!u->write && !readers)
                        continue;

                /* Usages are ordered, so everything else is also invalid */
                if (u->queue >= k->event_slot_usage)
                        break;

                struct kbase_event_slot *slot = &k->event_slots[u->queue];
                uint64_t seqnum = u->seqnum;

                /* There is a race condition, where we can depend on an
                 * unsubmitted batch. In that cade, decrease the seqnum.
                 * Otherwise, skip invalid dependencies. TODO: do GC? */
                if (slot->last_submit == seqnum)
                        --seqnum;
                else if (slot->last_submit < seqnum)
                        continue;

                if (slot->last <= seqnum) {
                        ret = false;
                        break;
                }
        }

        pthread_mutex_unlock(&dev->mali.queue_lock);
        pthread_mutex_unlock(&dev->bo_usage_lock);

        return ret;
}

/* Returns true if the BO is ready, false otherwise.
 * access_type is encoding the type of access one wants to ensure is done.
 * Waiting is always done for writers, but if wait_readers is set then readers
 * are also waited for.
 */
bool
panfrost_bo_wait(struct panfrost_bo *bo, int64_t timeout_ns, bool wait_readers)
{
        struct panfrost_device *dev = bo->dev;
        struct drm_panfrost_wait_bo req = {
                .handle = bo->gem_handle,
		.timeout_ns = timeout_ns,
        };
        int ret;

        /* TODO: With driver-handled sync, is gpu_access even worth it? */

        /* If the BO has been exported or imported we can't rely on the cached
         * state, we need to call the WAIT_BO ioctl.
         */
        if (!(bo->flags & PAN_BO_SHARED)) {
                /* If ->gpu_access is 0, the BO is idle, no need to wait. */
                if (!bo->gpu_access)
                        return true;

                /* If the caller only wants to wait for writers and no
                 * writes are pending, we don't have to wait.
                 */
                if (!wait_readers && !(bo->gpu_access & PAN_BO_ACCESS_WRITE))
                        return true;
        }

        if (dev->kbase && (dev->arch >= 10)) {
                struct kbase_wait_ctx wait = kbase_wait_init(&dev->mali, timeout_ns);
                while (kbase_wait_for_event(&wait)) {
                        if (panfrost_bo_usage_finished(bo, wait_readers))
                                break;
                }
                kbase_wait_fini(wait);

                bool ret = panfrost_bo_usage_finished(bo, wait_readers);
                if (bo->flags & PAN_BO_SHARED)
                        ret &= kbase_poll_fd_until(bo->dmabuf_fd, wait_readers, wait.until);

                if (ret)
                        bo->gpu_access &= (wait_readers ? 0 : PAN_BO_ACCESS_READ);
                return ret;
        }

        /* The ioctl returns >= 0 value when the BO we are waiting for is ready
         * -1 otherwise.
         */
        if (dev->kbase)
                ret = kbase_wait_bo(&dev->mali, bo->gem_handle, timeout_ns,
                                    wait_readers);
        else
                ret = drmIoctl(dev->fd, DRM_IOCTL_PANFROST_WAIT_BO, &req);
        if (ret != -1) {
                /* Set gpu_access to 0 so that the next call to bo_wait()
                 * doesn't have to call the WAIT_BO ioctl.
                 */
                bo->gpu_access = 0;
                return true;
        }

        /* If errno is not ETIMEDOUT or EBUSY that means the handle we passed
         * is invalid, which shouldn't happen here.
         */
        assert(errno == ETIMEDOUT || errno == EBUSY);
        return false;
}

static void
panfrost_bo_mem_op(struct panfrost_bo *bo, size_t offset, size_t length, bool invalidate)
{
        struct panfrost_device *dev = bo->dev;

        assert(offset + length <= bo->size);

        if (!bo->cached)
                return;

        dev->mali.mem_sync(&dev->mali, bo->ptr.gpu, bo->ptr.cpu + offset, length,
                           invalidate);
}

void
panfrost_bo_mem_invalidate(struct panfrost_bo *bo, size_t offset, size_t length)
{
        panfrost_bo_mem_op(bo, offset, length, true);
}

void
panfrost_bo_mem_clean(struct panfrost_bo *bo, size_t offset, size_t length)
{
        panfrost_bo_mem_op(bo, offset, length, false);
}

/* Helper to calculate the bucket index of a BO */

static unsigned
pan_bucket_index(unsigned size)
{
        /* Round down to POT to compute a bucket index */

        unsigned bucket_index = util_logbase2(size);

        /* Clamp the bucket index; all huge allocations will be
         * sorted into the largest bucket */

        bucket_index = CLAMP(bucket_index, MIN_BO_CACHE_BUCKET,
                             MAX_BO_CACHE_BUCKET);

        /* Reindex from 0 */
        return (bucket_index - MIN_BO_CACHE_BUCKET);
}

static struct list_head *
pan_bucket(struct panfrost_device *dev, unsigned size)
{
        return &dev->bo_cache.buckets[pan_bucket_index(size)];
}

/* Tries to fetch a BO of sufficient size with the appropriate flags from the
 * BO cache. If it succeeds, it returns that BO and removes the BO from the
 * cache. If it fails, it returns NULL signaling the caller to allocate a new
 * BO. */

static struct panfrost_bo *
panfrost_bo_cache_fetch(struct panfrost_device *dev,
                        size_t size, uint32_t flags, const char *label,
                        bool dontwait)
{
        pthread_mutex_lock(&dev->bo_cache.lock);
        struct list_head *bucket = pan_bucket(dev, size);
        struct panfrost_bo *bo = NULL;

        /* Iterate the bucket looking for something suitable */
        list_for_each_entry_safe(struct panfrost_bo, entry, bucket,
                                 bucket_link) {
                if (entry->size < size || entry->flags != flags)
                        continue;

                /* If the oldest BO in the cache is busy, likely so is
                 * everything newer, so bail. */

                /* For kbase, BOs are not added to the cache until the GPU is
                 * done with them, so there is no need to wait. */
                if (!dev->kbase) {
                        if (!panfrost_bo_wait(entry, dontwait ? 0 : INT64_MAX,
                                              PAN_BO_ACCESS_RW))
                                break;
                }

                struct drm_panfrost_madvise madv = {
                        .handle = entry->gem_handle,
                        .madv = PANFROST_MADV_WILLNEED,
                };
                int ret = 0;

                /* This one works, splice it out of the cache */
                list_del(&entry->bucket_link);
                list_del(&entry->lru_link);

                if (dev->kbase) {
                        /* With kbase, BOs are never freed from the cache */
                        madv.retained = true;
                } else {
                        ret = drmIoctl(dev->fd, DRM_IOCTL_PANFROST_MADVISE, &madv);
                }
                if (!ret && !madv.retained) {
                        panfrost_bo_free(entry);
                        continue;
                }
                /* Let's go! */
                bo = entry;
                bo->label = label;
                break;
        }
        pthread_mutex_unlock(&dev->bo_cache.lock);

        return bo;
}

static void
panfrost_bo_cache_evict_stale_bos(struct panfrost_device *dev)
{
        struct timespec time;

        clock_gettime(CLOCK_MONOTONIC, &time);
        list_for_each_entry_safe(struct panfrost_bo, entry,
                                 &dev->bo_cache.lru, lru_link) {
                /* We want all entries that have been used more than 1 sec
                 * ago to be dropped, others can be kept.
                 * Note the <= 2 check and not <= 1. It's here to account for
                 * the fact that we're only testing ->tv_sec, not ->tv_nsec.
                 * That means we might keep entries that are between 1 and 2
                 * seconds old, but we don't really care, as long as unused BOs
                 * are dropped at some point.
                 */
                if (time.tv_sec - entry->last_used <= 2)
                        break;

                list_del(&entry->bucket_link);
                list_del(&entry->lru_link);
                panfrost_bo_free(entry);
        }
}

/* Tries to add a BO to the cache. Returns if it was
 * successful */

static bool
panfrost_bo_cache_put(struct panfrost_bo *bo)
{
        struct panfrost_device *dev = bo->dev;

        if (bo->flags & PAN_BO_SHARED || dev->debug & PAN_DBG_NO_CACHE)
                return false;

        /* Must be first */
        pthread_mutex_lock(&dev->bo_cache.lock);

        struct list_head *bucket = pan_bucket(dev, MAX2(bo->size, 4096));
        struct drm_panfrost_madvise madv;
        struct timespec time;

        madv.handle = bo->gem_handle;
        madv.madv = PANFROST_MADV_DONTNEED;
	madv.retained = 0;

        // TODO: Allow freeing madvise'd BOs with kbase... not that it really
        // matters for boards with 16 GB RAM
        if (!dev->kbase)
                drmIoctl(dev->fd, DRM_IOCTL_PANFROST_MADVISE, &madv);

        /* Add us to the bucket */
        list_addtail(&bo->bucket_link, bucket);

        /* Add us to the LRU list and update the last_used field. */
        list_addtail(&bo->lru_link, &dev->bo_cache.lru);
        clock_gettime(CLOCK_MONOTONIC, &time);
        bo->last_used = time.tv_sec;

        /* For kbase, the GPU can't be accessing this BO any more */
        if (dev->kbase)
                bo->gpu_access = 0;

        /* Let's do some cleanup in the BO cache while we hold the
         * lock.
         */
        panfrost_bo_cache_evict_stale_bos(dev);

        /* Update the label to help debug BO cache memory usage issues */
        bo->label = "Unused (BO cache)";

        /* Must be last */
        pthread_mutex_unlock(&dev->bo_cache.lock);
        return true;
}

/* Evicts all BOs from the cache. Called during context
 * destroy or during low-memory situations (to free up
 * memory that may be unused by us just sitting in our
 * cache, but still reserved from the perspective of the
 * OS) */

void
panfrost_bo_cache_evict_all(
                struct panfrost_device *dev)
{
        pthread_mutex_lock(&dev->bo_cache.lock);
        for (unsigned i = 0; i < ARRAY_SIZE(dev->bo_cache.buckets); ++i) {
                struct list_head *bucket = &dev->bo_cache.buckets[i];

                list_for_each_entry_safe(struct panfrost_bo, entry, bucket,
                                         bucket_link) {
                        list_del(&entry->bucket_link);
                        list_del(&entry->lru_link);
                        panfrost_bo_free(entry);
                }
        }
        pthread_mutex_unlock(&dev->bo_cache.lock);
}

void
panfrost_bo_mmap(struct panfrost_bo *bo)
{
        struct drm_panfrost_mmap_bo mmap_bo = { .handle = bo->gem_handle };
        int ret;

        if (bo->ptr.cpu)
                return;

        ret = drmIoctl(bo->dev->fd, DRM_IOCTL_PANFROST_MMAP_BO, &mmap_bo);
        if (ret) {
                fprintf(stderr, "DRM_IOCTL_PANFROST_MMAP_BO failed: %m\n");
                assert(0);
        }

        bo->ptr.cpu = os_mmap(NULL, bo->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                              bo->dev->fd, mmap_bo.offset);
        if (bo->ptr.cpu == MAP_FAILED) {
                bo->ptr.cpu = NULL;
                fprintf(stderr,
                        "mmap failed: result=%p size=0x%llx fd=%i offset=0x%llx %m\n",
                        bo->ptr.cpu, (long long)bo->size, bo->dev->fd,
                        (long long)mmap_bo.offset);
        }
}

static void
panfrost_bo_munmap(struct panfrost_bo *bo)
{
        /* We can't munmap BOs when using kbase, as that frees the storage and
         * the GPU might still be using the BO. */
        if (bo->dev->kbase)
                return;

        if (!bo->ptr.cpu)
                return;

        if (os_munmap(bo->ptr.cpu, bo->size)) {
                perror("munmap");
                abort();
        }

        bo->ptr.cpu = NULL;
}

struct panfrost_bo *
panfrost_bo_create(struct panfrost_device *dev, size_t size,
                   uint32_t flags, const char *label)
{
        struct panfrost_bo *bo;

        /* Kernel will fail (confusingly) with EPERM otherwise */
        assert(size > 0);

        /* To maximize BO cache usage, don't allocate tiny BOs */
        size = ALIGN_POT(size, 4096);

        /* GROWABLE BOs cannot be mmapped */
        if (flags & PAN_BO_GROWABLE)
                assert(flags & PAN_BO_INVISIBLE);

        /* Ideally, we get a BO that's ready in the cache, or allocate a fresh
         * BO. If allocation fails, we can try waiting for something in the
         * cache. But if there's no nothing suitable, we should flush the cache
         * to make space for the new allocation.
         */
        bo = panfrost_bo_cache_fetch(dev, size, flags, label, true);
        if (!bo)
                bo = panfrost_bo_alloc(dev, size, flags, label);
        if (!bo)
                bo = panfrost_bo_cache_fetch(dev, size, flags, label, false);
        if (!bo) {
                for (unsigned i = 0; i < 5; ++i) {
                        usleep(20 * 1000 * i * i);
                        if (dev->kbase)
                                kbase_ensure_handle_events(&dev->mali);
                        panfrost_bo_cache_evict_all(dev);
                        bo = panfrost_bo_alloc(dev, size, flags, label);
                        if (bo)
                                break;
                }
        }

        if (!bo) {
                unreachable("BO creation failed. We don't handle that yet.");
                return NULL;
        }

        /* Only mmap now if we know we need to. For CPU-invisible buffers, we
         * never map since we don't care about their contents; they're purely
         * for GPU-internal use. But we do trace them anyway. */

        if (!(flags & (PAN_BO_INVISIBLE | PAN_BO_DELAY_MMAP)))
                panfrost_bo_mmap(bo);

        if ((dev->debug & PAN_DBG_BO_CLEAR) && !(flags & PAN_BO_INVISIBLE)) {
                memset(bo->ptr.cpu, 0, bo->size);
                panfrost_bo_mem_clean(bo, 0, bo->size);
        }

        p_atomic_set(&bo->refcnt, 1);

        util_dynarray_init(&bo->usage, NULL);

        if (dev->debug & (PAN_DBG_TRACE | PAN_DBG_SYNC)) {
                if (flags & PAN_BO_INVISIBLE)
                        pandecode_inject_mmap(bo->ptr.gpu, NULL, bo->size, NULL);
                else if (!(flags & PAN_BO_DELAY_MMAP))
                        pandecode_inject_mmap(bo->ptr.gpu, bo->ptr.cpu, bo->size, NULL);
        }

        if (dev->bo_log) {
                struct timespec tp;
                clock_gettime(CLOCK_MONOTONIC_RAW, &tp);
                fprintf(dev->bo_log, "%"PRIu64".%09li alloc %"PRIx64" to %"PRIx64" size %zu label %s\n",
                        (uint64_t) tp.tv_sec, tp.tv_nsec, bo->ptr.gpu, bo->ptr.gpu + bo->size, bo->size, bo->label);
                fflush(NULL);
        }

        return bo;
}

void
panfrost_bo_reference(struct panfrost_bo *bo)
{
        if (bo) {
                ASSERTED int count = p_atomic_inc_return(&bo->refcnt);
                assert(count != 1);
        }
}

static void
panfrost_bo_fini(struct panfrost_bo *bo)
{
        struct panfrost_device *dev = bo->dev;

        /* When the reference count goes to zero, we need to cleanup */
        panfrost_bo_munmap(bo);

        if (dev->debug & (PAN_DBG_TRACE | PAN_DBG_SYNC))
                pandecode_inject_free(bo->ptr.gpu, bo->size);

        /* Rather than freeing the BO now, we'll cache the BO for later
         * allocations if we're allowed to.
         */
        if (!panfrost_bo_cache_put(bo))
                panfrost_bo_free(bo);
}

static void
panfrost_bo_free_gpu(void *data)
{
        struct panfrost_bo *bo = data;
        struct panfrost_device *dev = bo->dev;

        /* Don't free if there are still references */
        if (p_atomic_dec_return(&bo->gpu_refcnt))
                return;

        pthread_mutex_lock(&dev->bo_map_lock);

        /* Someone might have imported this BO while we were waiting for the
         * lock, let's make sure it's still not referenced before freeing it.
         */
        if (p_atomic_read(&bo->refcnt) != 0) {
                pthread_mutex_unlock(&dev->bo_map_lock);
                return;
        }

        if (dev->bo_log) {
                int fd = kbase_gem_handle_get(&dev->mali, bo->gem_handle).fd;

                struct timespec tp;
                clock_gettime(CLOCK_MONOTONIC_RAW, &tp);
                fprintf(dev->bo_log, "%"PRIu64".%09li gpufree %"PRIx64" to %"PRIx64" size %zu label %s obj (%p,%i,%i)\n",
                        (uint64_t) tp.tv_sec, tp.tv_nsec, bo->ptr.gpu, bo->ptr.gpu + bo->size, bo->size, bo->label,
                        bo, bo->gem_handle, fd);
                fflush(NULL);
        }

        panfrost_bo_fini(bo);

        pthread_mutex_unlock(&dev->bo_map_lock);
}

void
panfrost_bo_unreference(struct panfrost_bo *bo)
{
        if (!bo)
                return;

        /* Don't return to cache if there are still references */
        if (p_atomic_dec_return(&bo->refcnt))
                return;

        struct panfrost_device *dev = bo->dev;

        if (dev->bo_log) {
                int fd = kbase_gem_handle_get(&dev->mali, bo->gem_handle).fd;

                struct timespec tp;
                clock_gettime(CLOCK_MONOTONIC_RAW, &tp);
                fprintf(dev->bo_log, "%"PRIu64".%09li free %"PRIx64" to %"PRIx64" size %zu label %s obj (%p,%i,%i)\n",
                        (uint64_t) tp.tv_sec, tp.tv_nsec, bo->ptr.gpu, bo->ptr.gpu + bo->size, bo->size, bo->label,
                        bo, bo->gem_handle, fd);
                fflush(NULL);
        }

        pthread_mutex_lock(&dev->bo_map_lock);

        /* Someone might have imported this BO while we were waiting for the
         * lock, let's make sure it's still not referenced before freeing it.
         */
        if (p_atomic_read(&bo->refcnt) != 0) {
                pthread_mutex_unlock(&dev->bo_map_lock);
                return;
        }

        util_dynarray_fini(&bo->usage);

        if (dev->kbase) {
                /* Assume that all queues are using this BO, and so free the
                 * BO only after all currently-submitted jobs have finished.
                 * This could eventually be optimised to only wait on a subset
                 * of queues.
                 */
                bool added = dev->mali.callback_all_queues(&dev->mali,
                        &bo->gpu_refcnt, panfrost_bo_free_gpu, bo);

                if (added) {
                        pthread_mutex_unlock(&dev->bo_map_lock);
                        return;
                }
        }

        if (dev->bo_log) {
                int fd = kbase_gem_handle_get(&dev->mali, bo->gem_handle).fd;

                struct timespec tp;
                clock_gettime(CLOCK_MONOTONIC_RAW, &tp);
                fprintf(dev->bo_log, "%"PRIu64".%09li immfree %"PRIx64" to %"PRIx64" size %zu label %s obj (%p,%i,%i)\n",
                        (uint64_t) tp.tv_sec, tp.tv_nsec, bo->ptr.gpu, bo->ptr.gpu + bo->size, bo->size, bo->label,
                        bo, bo->gem_handle, fd);
                fflush(NULL);
        }

        panfrost_bo_fini(bo);

        pthread_mutex_unlock(&dev->bo_map_lock);
}

struct panfrost_bo *
panfrost_bo_import(struct panfrost_device *dev, int fd)
{
        struct panfrost_bo *bo;
        struct drm_panfrost_get_bo_offset get_bo_offset = {0,};
        ASSERTED int ret;
        kbase_handle handle = { .fd = -1 };
        unsigned gem_handle;

        if (dev->kbase) {
                gem_handle = dev->mali.import_dmabuf(&dev->mali, fd);
                if (gem_handle == -1)
                        return NULL;
        } else {
                ret = drmPrimeFDToHandle(dev->fd, fd, &gem_handle);
                assert(!ret);
        }

        pthread_mutex_lock(&dev->bo_map_lock);
        bo = pan_lookup_bo(dev, gem_handle);

        bool found = false;

        if (!bo->dev) {
                get_bo_offset.handle = gem_handle;
                if (dev->kbase) {
                        handle = kbase_gem_handle_get(&dev->mali, gem_handle);
                        get_bo_offset.offset = handle.va;
                } else {
                        ret = drmIoctl(dev->fd, DRM_IOCTL_PANFROST_GET_BO_OFFSET, &get_bo_offset);
                        assert(!ret);
                }

                bo->dev = dev;
                bo->size = lseek(fd, 0, SEEK_END);
                bo->ptr.gpu = (mali_ptr) get_bo_offset.offset;
                if (dev->kbase && (sizeof(void *) > 4 || get_bo_offset.offset < (1LL << 32))) {
                        bo->ptr.cpu = (void *)(uintptr_t) get_bo_offset.offset;
                } else if (dev->kbase) {
                        bo->ptr.cpu = dev->mali.mmap_import(&dev->mali, bo->ptr.gpu, bo->size);
                        bo->free_ioctl = true;
                }
                /* Sometimes this can fail and return -1. size of -1 is not
                 * a nice thing for mmap to try mmap. Be more robust also
                 * for zero sized maps and fail nicely too
                 */
                if ((bo->size == 0) || (bo->size == (size_t)-1)) {
                        pthread_mutex_unlock(&dev->bo_map_lock);
                        return NULL;
                }
                bo->flags = PAN_BO_SHARED;
                bo->gem_handle = gem_handle;
                util_dynarray_init(&bo->usage, NULL);
                if (dev->kbase) {
                        /* kbase always maps dma-bufs with caching */
                        bo->cached = true;

                        /* Importing duplicates the FD, so we cache the FD
                         * from the handle */
                        bo->dmabuf_fd = handle.fd;
                } else {
                        bo->dmabuf_fd = -1;
                }
                p_atomic_set(&bo->refcnt, 1);
        } else {
                found = true;

                /* bo->refcnt == 0 can happen if the BO
                 * was being released but panfrost_bo_import() acquired the
                 * lock before panfrost_bo_unreference(). In that case, refcnt
                 * is 0 and we can't use panfrost_bo_reference() directly, we
                 * have to re-initialize the refcnt().
                 * Note that panfrost_bo_unreference() checks
                 * refcnt value just after acquiring the lock to
                 * make sure the object is not freed if panfrost_bo_import()
                 * acquired it in the meantime.
                 */
                if (p_atomic_read(&bo->refcnt) == 0)
                        p_atomic_set(&bo->refcnt, 1);
                else
                        panfrost_bo_reference(bo);
        }
        pthread_mutex_unlock(&dev->bo_map_lock);

        if (dev->bo_log) {
                int new_fd = kbase_gem_handle_get(&dev->mali, bo->gem_handle).fd;

                struct timespec tp;
                clock_gettime(CLOCK_MONOTONIC_RAW, &tp);
                fprintf(dev->bo_log, "%"PRIu64".%09li import %"PRIx64" to %"PRIx64" size %zu fd %i new %i handle %i found %i\n",
                        (uint64_t) tp.tv_sec, tp.tv_nsec, bo->ptr.gpu, bo->ptr.gpu + bo->size, bo->size,
                        fd, new_fd, gem_handle, found);
                fflush(NULL);
        }

        return bo;
}

int
panfrost_bo_export(struct panfrost_bo *bo)
{
        struct panfrost_device *dev = bo->dev;

        if (bo->dmabuf_fd != -1) {
                assert(bo->flags & PAN_BO_SHARED);

                return os_dupfd_cloexec(bo->dmabuf_fd);
        }

        if (dev->kbase)
                return -1;

        struct drm_prime_handle args = {
                .handle = bo->gem_handle,
                .flags = DRM_CLOEXEC,
        };

        int ret = drmIoctl(bo->dev->fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &args);
        if (ret == -1)
                return -1;

        bo->flags |= PAN_BO_SHARED;
        return args.fd;
}

