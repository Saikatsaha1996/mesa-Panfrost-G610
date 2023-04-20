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
#include <stdarg.h>
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

#ifdef HAVE_VALGRIND
#include <valgrind.h>
#else
#define RUNNING_ON_VALGRIND 0
#endif

#include "util/macros.h"
#include "util/list.h"
#include "util/u_atomic.h"
#include "util/os_file.h"

#include "pan_base.h"
#include "pan_cache.h"

#include "drm-uapi/panfrost_drm.h"

#define PAN_BASE_API (PAN_BASE_VER & 0xff)
#if (PAN_BASE_VER & 0x100) == 0x100
#define PAN_BASE_NOOP
#endif

#if PAN_BASE_API >= 2
#include "csf/mali_gpu_csf_registers.h"

#define MALI_USE_CSF 1
#endif

#include "mali_kbase_gpuprops.h"

#ifndef PAN_BASE_NOOP
#define kbase_mmap mmap
#endif

#if PAN_BASE_API >= 1
#include "mali_base_kernel.h"
#include "mali_kbase_ioctl.h"

#ifdef PAN_BASE_NOOP
#include "pan_base_noop.h"
#else
#define kbase_ioctl ioctl
#endif
#else

#include "old/mali-ioctl.h"
#include "old/mali-ioctl-midgard.h"
#include "old/mali-props.h"
#endif

#define LOG(fmt, ...) do { \
                if (k->verbose) { \
                        struct timespec tp; \
                        clock_gettime(CLOCK_MONOTONIC_RAW, &tp); \
                        printf("%"PRIu64".%09li\t" fmt, (uint64_t) tp.tv_sec, tp.tv_nsec __VA_OPT__(,) __VA_ARGS__); \
                } \
        } while (0)

#if PAN_BASE_API == 0
static int
kbase_ioctl(int fd, unsigned long request, ...)
{
        int ioc_size = _IOC_SIZE(request);

        assert(ioc_size);

        va_list args;

        va_start(args, request);
        int *ptr = va_arg(args, void *);
        va_end(args);

        *ptr = (_IOC_TYPE(request) - 0x80) * 256 + _IOC_NR(request);

        int ret = ioctl(fd, request, ptr);
        if (ret)
                return ret;

        int r = *ptr;
        switch (r) {
        case MALI_ERROR_OUT_OF_GPU_MEMORY:
                errno = ENOSPC;
                return -1;
        case MALI_ERROR_OUT_OF_MEMORY:
                errno = ENOMEM;
                return -1;
        case MALI_ERROR_FUNCTION_FAILED:
                errno = EINVAL;
                return -1;
        default:
                return 0;
        }
}
#endif

#if PAN_BASE_API >= 1
static bool
kbase_get_mali_gpuprop(kbase k, unsigned name, uint64_t *value)
{
        int i = 0;
        uint64_t x = 0;
        while (i < k->gpuprops_size) {
                x = 0;
                memcpy(&x, k->gpuprops + i, 4);
                i += 4;

                int size = 1 << (x & 3);
                int this_name = x >> 2;

                x = 0;
                memcpy(&x, k->gpuprops + i, size);
                i += size;

                if (this_name == name) {
                        *value = x;
                        return true;
                }
        }

        return false;
}
#else
static bool
kbase_get_mali_gpuprop(kbase k, unsigned name, uint64_t *value)
{
        struct kbase_ioctl_gpu_props_reg_dump *props = k->gpuprops;

        switch (name) {
        case KBASE_GPUPROP_PRODUCT_ID:
                *value = props->core.product_id;
                return true;
        case KBASE_GPUPROP_RAW_SHADER_PRESENT:
                *value = props->raw.shader_present;
                return true;
        case KBASE_GPUPROP_RAW_TEXTURE_FEATURES_0:
                *value = props->raw.texture_features[0];
                return true;
        case KBASE_GPUPROP_RAW_TILER_FEATURES:
                *value = props->raw.tiler_features;
                return true;
        case KBASE_GPUPROP_RAW_GPU_ID:
                *value = props->raw.gpu_id;
                return true;
        default:
                return false;
        }
}
#endif

static bool
alloc_handles(kbase k)
{
        util_dynarray_init(&k->gem_handles, NULL);
        return true;
}

static bool
free_handles(kbase k)
{
        util_dynarray_fini(&k->gem_handles);
        return true;
}

static bool
set_flags(kbase k)
{
        struct kbase_ioctl_set_flags flags = {
                .create_flags = 0
        };

        int ret = kbase_ioctl(k->fd, KBASE_IOCTL_SET_FLAGS, &flags);

        if (ret == -1) {
                perror("ioctl(KBASE_IOCTL_SET_FLAGS)");
                return false;
        }
        return true;
}

static bool
mmap_tracking(kbase k)
{
        k->tracking_region = kbase_mmap(NULL, k->page_size, PROT_NONE,
                                        MAP_SHARED, k->fd,
                                        BASE_MEM_MAP_TRACKING_HANDLE);

        if (k->tracking_region == MAP_FAILED) {
                perror("mmap(BASE_MEM_MAP_TRACKING_HANDLE)");
                k->tracking_region = NULL;
                return false;
        }
        return true;
}

static bool
munmap_tracking(kbase k)
{
        if (k->tracking_region)
                return munmap(k->tracking_region, k->page_size) == 0;
        return true;
}

#if PAN_BASE_API >= 1
static bool
get_gpuprops(kbase k)
{
        struct kbase_ioctl_get_gpuprops props = { 0 };

        int ret = kbase_ioctl(k->fd, KBASE_IOCTL_GET_GPUPROPS, &props);
        if (ret == -1) {
                perror("ioctl(KBASE_IOCTL_GET_GPUPROPS(0))");
                return false;
        } else if (!ret) {
                fprintf(stderr, "GET_GPUPROPS returned zero size\n");
                return false;
        }

        k->gpuprops_size = ret;
        k->gpuprops = calloc(k->gpuprops_size, 1);

        props.size = k->gpuprops_size;
        props.buffer = (uint64_t)(uintptr_t) k->gpuprops;

        ret = kbase_ioctl(k->fd, KBASE_IOCTL_GET_GPUPROPS, &props);
        if (ret == -1) {
                perror("ioctl(KBASE_IOCTL_GET_GPUPROPS(size))");
                return false;
        }

        return true;
}
#else
static bool
get_gpuprops(kbase k)
{
        k->gpuprops = calloc(1, sizeof(struct kbase_ioctl_gpu_props_reg_dump));

        int ret = kbase_ioctl(k->fd, KBASE_IOCTL_GPU_PROPS_REG_DUMP, k->gpuprops);
        if (ret == -1) {
                perror("ioctl(KBASE_IOCTL_GPU_PROPS_REG_DUMP)");
                return false;
        }

        return true;
}
#endif

static bool
free_gpuprops(kbase k)
{
        free(k->gpuprops);
        return true;
}

#if PAN_BASE_API >= 2
static bool
mmap_user_reg(kbase k)
{
        k->csf_user_reg = kbase_mmap(NULL, k->page_size, PROT_READ,
                                     MAP_SHARED, k->fd,
                                     BASEP_MEM_CSF_USER_REG_PAGE_HANDLE);

        if (k->csf_user_reg == MAP_FAILED) {
                perror("mmap(BASEP_MEM_CSF_USER_REG_PAGE_HANDLE)");
                k->csf_user_reg = NULL;
                return false;
        }
        return true;
}

static bool
munmap_user_reg(kbase k)
{
        if (k->csf_user_reg)
                return munmap(k->csf_user_reg, k->page_size) == 0;
        return true;
}
#endif

#if PAN_BASE_API >= 1
static bool
init_mem_exec(kbase k)
{
        struct kbase_ioctl_mem_exec_init init = {
                .va_pages = 0x100000,
        };

        int ret = kbase_ioctl(k->fd, KBASE_IOCTL_MEM_EXEC_INIT, &init);

        if (ret == -1) {
                perror("ioctl(KBASE_IOCTL_MEM_EXEC_INIT)");
                return false;
        }
        return true;
}

static bool
init_mem_jit(kbase k)
{
        struct kbase_ioctl_mem_jit_init init = {
                .va_pages = 1 << 25,
                .max_allocations = 255,
                .phys_pages = 1 << 25,
        };

        int ret = kbase_ioctl(k->fd, KBASE_IOCTL_MEM_JIT_INIT, &init);

        if (ret == -1) {
                perror("ioctl(KBASE_IOCTL_MEM_JIT_INIT)");
                return false;
        }
        return true;
}
#endif

#if PAN_BASE_API >= 2
static struct base_ptr
kbase_alloc(kbase k, size_t size, unsigned pan_flags, unsigned mali_flags);

static bool
alloc_event_mem(kbase k)
{
        k->event_mem = kbase_alloc(k, k->page_size * 2,
                                   PANFROST_BO_NOEXEC,
                                   BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
                                   BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR |
                                   BASE_MEM_SAME_VA | BASE_MEM_CSF_EVENT);
        k->kcpu_event_mem = (struct base_ptr) {
                .cpu = k->event_mem.cpu + k->page_size,
                .gpu = k->event_mem.gpu + k->page_size,
        };
        return k->event_mem.cpu;
}

static bool
free_event_mem(kbase k)
{
        if (k->event_mem.cpu)
                return munmap(k->event_mem.cpu, k->page_size * 2) == 0;
        return true;
}
#endif

#if PAN_BASE_API >= 2
static bool
cs_group_create(kbase k, struct kbase_context *c)
{
        /* TODO: What about compute-only contexts? */
        union kbase_ioctl_cs_queue_group_create_1_6 create = {
                .in = {
                        /* Mali *still* only supports a single tiler unit */
                        .tiler_mask = 1,
                        .fragment_mask = ~0ULL,
                        .compute_mask = ~0ULL,

                        .cs_min = k->cs_queue_count,

                        .priority = 1,
                        .tiler_max = 1,
                        .fragment_max = 64,
                        .compute_max = 64,
                }
        };

        int ret = kbase_ioctl(k->fd, KBASE_IOCTL_CS_QUEUE_GROUP_CREATE_1_6, &create);

        if (ret == -1) {
                perror("ioctl(KBASE_IOCTL_CS_QUEUE_GROUP_CREATE_1_6)");
                return false;
        }

        c->csg_handle = create.out.group_handle;
        c->csg_uid = create.out.group_uid;

        /* Should be at least 1 */
        assert(c->csg_uid);

        return true;
}

static bool
cs_group_term(kbase k, struct kbase_context *c)
{
        if (!c->csg_uid)
                return true;

        struct kbase_ioctl_cs_queue_group_term term = {
                .group_handle = c->csg_handle
        };

        int ret = kbase_ioctl(k->fd, KBASE_IOCTL_CS_QUEUE_GROUP_TERMINATE, &term);

        if (ret == -1) {
                perror("ioctl(KBASE_IOCTL_CS_QUEUE_GROUP_TERMINATE)");
                return false;
        }
        return true;
}
#endif

#if PAN_BASE_API >= 2
static bool
tiler_heap_create(kbase k, struct kbase_context *c)
{
        c->tiler_heap_chunk_size = 1 << 21; /* 2 MB */

        union kbase_ioctl_cs_tiler_heap_init init = {
                .in = {
                        .chunk_size = c->tiler_heap_chunk_size,
                        .initial_chunks = 5,
                        .max_chunks = 200,
                        .target_in_flight = 65535,
                }
        };

        int ret = kbase_ioctl(k->fd, KBASE_IOCTL_CS_TILER_HEAP_INIT, &init);

        if (ret == -1) {
                perror("ioctl(KBASE_IOCTL_CS_TILER_HEAP_INIT)");
                return false;
        }

        c->tiler_heap_va = init.out.gpu_heap_va;
        c->tiler_heap_header = init.out.first_chunk_va;

        return true;
}

static bool
tiler_heap_term(kbase k, struct kbase_context *c)
{
        if (!c->tiler_heap_va)
                return true;

        struct kbase_ioctl_cs_tiler_heap_term term = {
                .gpu_heap_va = c->tiler_heap_va
        };

        int ret = kbase_ioctl(k->fd, KBASE_IOCTL_CS_TILER_HEAP_TERM, &term);

        if (ret == -1) {
                perror("ioctl(KBASE_IOCTL_CS_TILER_HEAP_TERM)");
                return false;
        }
        return true;
}
#endif

typedef bool (* kbase_func)(kbase k);

struct kbase_op {
        kbase_func part;
        kbase_func cleanup;
        const char *label;
};

static struct kbase_op kbase_main[] = {
        { alloc_handles, free_handles, "Allocate handle array" },
#if PAN_BASE_API >= 1
        { set_flags, NULL, "Set flags" },
#endif
        { mmap_tracking, munmap_tracking, "Map tracking handle" },
#if PAN_BASE_API == 0
        { set_flags, NULL, "Set flags" },
#endif
        { get_gpuprops, free_gpuprops, "Get GPU properties" },
#if PAN_BASE_API >= 2
        { mmap_user_reg, munmap_user_reg, "Map user register page" },
#endif
#if PAN_BASE_API >= 1
        { init_mem_exec, NULL, "Initialise EXEC_VA zone" },
        { init_mem_jit, NULL, "Initialise JIT allocator" },
#endif
#if PAN_BASE_API >= 2
        { alloc_event_mem, free_event_mem, "Allocate event memory" },
#endif
};

static void
kbase_close(kbase k)
{
        while (k->setup_state) {
                unsigned i = k->setup_state - 1;
                if (kbase_main[i].cleanup)
                        kbase_main[i].cleanup(k);
                --k->setup_state;
        }

        pthread_mutex_destroy(&k->handle_lock);
        pthread_mutex_destroy(&k->event_read_lock);
        pthread_mutex_destroy(&k->event_cnd_lock);
        pthread_mutex_destroy(&k->queue_lock);
        pthread_cond_destroy(&k->event_cnd);

        close(k->fd);
}

static bool
kbase_get_pan_gpuprop(kbase k, unsigned name, uint64_t *value)
{
        unsigned conv[] = {
                [DRM_PANFROST_PARAM_GPU_PROD_ID] = KBASE_GPUPROP_PRODUCT_ID,
                [DRM_PANFROST_PARAM_SHADER_PRESENT] = KBASE_GPUPROP_RAW_SHADER_PRESENT,
                [DRM_PANFROST_PARAM_TEXTURE_FEATURES0] = KBASE_GPUPROP_RAW_TEXTURE_FEATURES_0,
                [DRM_PANFROST_PARAM_THREAD_TLS_ALLOC] = KBASE_GPUPROP_TLS_ALLOC,
                [DRM_PANFROST_PARAM_TILER_FEATURES] = KBASE_GPUPROP_RAW_TILER_FEATURES,
        };

        if (name < ARRAY_SIZE(conv) && conv[name])
                return kbase_get_mali_gpuprop(k, conv[name], value);

        switch (name) {
        case DRM_PANFROST_PARAM_AFBC_FEATURES:
                *value = 0;
                return true;
        case DRM_PANFROST_PARAM_GPU_REVISION: {
                if (!kbase_get_mali_gpuprop(k, KBASE_GPUPROP_RAW_GPU_ID, value))
                        return false;
                *value &= 0xffff;
                return true;
        }
        default:
                return false;
        }
}

static void
kbase_free(kbase k, base_va va)
{
        struct kbase_ioctl_mem_free f = {
                .gpu_addr = va
        };

        int ret = kbase_ioctl(k->fd, KBASE_IOCTL_MEM_FREE, &f);

        if (ret == -1)
                perror("ioctl(KBASE_IOCTL_MEM_FREE)");
}

static struct base_ptr
kbase_alloc(kbase k, size_t size, unsigned pan_flags, unsigned mali_flags)
{
        struct base_ptr r = {0};

        unsigned pages = DIV_ROUND_UP(size, k->page_size);

        union kbase_ioctl_mem_alloc a = {
                .in = {
                        .va_pages = pages,
                        .commit_pages = pages,
                }
        };

        size_t alloc_size = size;
        unsigned flags = mali_flags;
        bool exec_align = false;

        if (!flags) {
                flags = BASE_MEM_PROT_CPU_RD | BASE_MEM_PROT_CPU_WR |
                        BASE_MEM_PROT_GPU_RD | BASE_MEM_PROT_GPU_WR |
                        BASE_MEM_SAME_VA;

                /* Add COHERENT_LOCAL to keep GPU cores coherent with each
                 * other. */
                if (PAN_BASE_API >= 1)
                        flags |= BASE_MEM_COHERENT_LOCAL;
        }

        if (pan_flags & PANFROST_BO_HEAP) {
                size_t align_size = 2 * 1024 * 1024 / k->page_size; /* 2 MB */

                a.in.va_pages = ALIGN_POT(a.in.va_pages, align_size);
                a.in.commit_pages = 0;
                a.in.extension = align_size;
                flags |= BASE_MEM_GROW_ON_GPF;
        }

#if PAN_BASE_API >= 1
        if (pan_flags & MALI_BO_CACHED_CPU)
                flags |= BASE_MEM_CACHED_CPU;
#endif

#if PAN_BASE_API >= 2
        if (pan_flags & MALI_BO_UNCACHED_GPU)
                flags |= BASE_MEM_UNCACHED_GPU;
#endif

        if (!(pan_flags & PANFROST_BO_NOEXEC)) {
                /* Using SAME_VA for executable BOs would make it too likely
                 * for a blend shader to end up on the wrong side of a 4 GB
                 * boundary. */
                flags |= BASE_MEM_PROT_GPU_EX;
                flags &= ~(BASE_MEM_PROT_GPU_WR | BASE_MEM_SAME_VA);

                if (PAN_BASE_API == 0) {
                        /* Assume 4K pages */
                        a.in.va_pages = 0x1000; /* Align shader BOs to 16 MB */
                        size = 1 << 26; /* Four times the alignment */
                        exec_align = true;
                }
        }

        a.in.flags = flags;

        int ret = kbase_ioctl(k->fd, KBASE_IOCTL_MEM_ALLOC, &a);

        if (ret == -1) {
                perror("ioctl(KBASE_IOCTL_MEM_ALLOC)");
                return r;
        }

        // TODO: Is this always true, even in the face of multithreading?
        if (PAN_BASE_API == 0)
                a.out.gpu_va = 0x41000;

        if ((flags & BASE_MEM_SAME_VA) &&
            !((a.out.flags & BASE_MEM_SAME_VA) &&
              a.out.gpu_va < 0x80000)) {

                fprintf(stderr, "Flags: 0x%"PRIx64", VA: 0x%"PRIx64"\n",
                        (uint64_t) a.out.flags, (uint64_t) a.out.gpu_va);
                errno = EINVAL;
                return r;
        }

        void *ptr = kbase_mmap(NULL, size,
                               PROT_READ | PROT_WRITE, MAP_SHARED,
                               k->fd, a.out.gpu_va);

        if (ptr == MAP_FAILED) {
                perror("mmap(GPU BO)");
                kbase_free(k, a.out.gpu_va);
                return r;
        }

        uint64_t gpu_va = (a.out.flags & BASE_MEM_SAME_VA) ?
                (uintptr_t) ptr : a.out.gpu_va;

        if (exec_align) {
                gpu_va = ALIGN_POT(gpu_va, 1 << 24);

                ptr = kbase_mmap(NULL, alloc_size,
                                 PROT_READ | PROT_WRITE, MAP_SHARED,
                                 k->fd, gpu_va);

                if (ptr == MAP_FAILED) {
                        perror("mmap(GPU EXEC BO)");
                        kbase_free(k, gpu_va);
                        return r;
                }
        }

        r.cpu = ptr;
        r.gpu = gpu_va;

        return r;
}

static int
kbase_import_dmabuf(kbase k, int fd)
{
        int ret;

        pthread_mutex_lock(&k->handle_lock);

        unsigned size = util_dynarray_num_elements(&k->gem_handles, kbase_handle);

        kbase_handle *handles = util_dynarray_begin(&k->gem_handles);

        for (unsigned i = 0; i < size; ++i) {
                kbase_handle h = handles[i];

                if (h.fd < 0)
                        continue;

                ret = os_same_file_description(h.fd, fd);

                if (ret == 0) {
                        pthread_mutex_unlock(&k->handle_lock);
                        return i;
                } else if (ret < 0) {
                        printf("error in os_same_file_description(%i, %i)\n", h.fd, fd);
                }
        }

        int dup = os_dupfd_cloexec(fd);

        union kbase_ioctl_mem_import import = {
                .in = {
                        .phandle = (uintptr_t) &dup,
                        .type = BASE_MEM_IMPORT_TYPE_UMM,
                        /* Usage flags: CPU/GPU reads/writes */
                        .flags = 0xf,
                }
        };

        ret = kbase_ioctl(k->fd, KBASE_IOCTL_MEM_IMPORT, &import);

        int handle;

        if (ret == -1) {
                perror("ioctl(KBASE_IOCTL_MEM_IMPORT)");
                handle = -1;
        } else if (import.out.flags & BASE_MEM_NEED_MMAP) {
                uint64_t va = (uintptr_t) kbase_mmap(NULL, import.out.va_pages * k->page_size,
                                                     PROT_READ | PROT_WRITE,
                                                     MAP_SHARED, k->fd, import.out.gpu_va);

                if (va == (uintptr_t) MAP_FAILED) {
                        perror("mmap(IMPORTED BO)");
                        handle = -1;
                } else {
                        handle = kbase_alloc_gem_handle_locked(k, va, dup);
                }
        } else {
                handle = kbase_alloc_gem_handle_locked(k, import.out.gpu_va, dup);
        }

        pthread_mutex_unlock(&k->handle_lock);

        return handle;
}

static void *
kbase_mmap_import(kbase k, base_va va, size_t size)
{
        return kbase_mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, k->fd, va);
}

struct kbase_fence {
        struct list_head link;

        unsigned slot;
        uint64_t value;
};

struct kbase_syncobj {
        struct list_head link;

        struct list_head fences;
};

static struct kbase_syncobj *
kbase_syncobj_create(kbase k)
{
        struct kbase_syncobj *o = calloc(1, sizeof(*o));
        list_inithead(&o->fences);
        pthread_mutex_lock(&k->queue_lock);
        list_add(&o->link, &k->syncobjs);
        pthread_mutex_unlock(&k->queue_lock);
        return o;
}

static void
kbase_syncobj_destroy(kbase k, struct kbase_syncobj *o)
{
        pthread_mutex_lock(&k->queue_lock);
        list_del(&o->link);
        pthread_mutex_unlock(&k->queue_lock);

        list_for_each_entry_safe(struct kbase_fence, fence, &o->fences, link) {
                list_del(&fence->link);
                free(fence);
        }

        free(o);
}

static void
kbase_syncobj_add_fence(struct kbase_syncobj *o, unsigned slot, uint64_t value)
{
        struct kbase_fence *fence = calloc(1, sizeof(*fence));

        fence->slot = slot;
        fence->value = value;

        list_add(&fence->link, &o->fences);
}

static void
kbase_syncobj_update_fence(struct kbase_syncobj *o, unsigned slot, uint64_t value)
{
        list_for_each_entry(struct kbase_fence, fence, &o->fences, link) {
                if (fence->slot == slot) {
                        if (value > fence->value)
                                fence->value = value;

                        return;
                }
        }

        kbase_syncobj_add_fence(o, slot, value);
}

static struct kbase_syncobj *
kbase_syncobj_dup(kbase k, struct kbase_syncobj *o)
{
        struct kbase_syncobj *dup = kbase_syncobj_create(k);

        pthread_mutex_lock(&k->queue_lock);

        list_for_each_entry(struct kbase_fence, fence, &o->fences, link)
                kbase_syncobj_add_fence(dup, fence->slot, fence->value);

        pthread_mutex_unlock(&k->queue_lock);

        return dup;
}

static void
kbase_syncobj_update(kbase k, struct kbase_syncobj *o)
{
        list_for_each_entry_safe(struct kbase_fence, fence, &o->fences, link) {
                uint64_t value = k->event_slots[fence->slot].last;

                if (value > fence->value) {
                        LOG("syncobj %p slot %u value %"PRIu64" vs %"PRIu64"\n",
                            o, fence->slot, fence->value, value);

                        list_del(&fence->link);
                        free(fence);
                }
        }
}

static bool
kbase_syncobj_wait(kbase k, struct kbase_syncobj *o)
{
        if (list_is_empty(&o->fences)) {
                LOG("syncobj has no fences\n");
                return true;
        }

        struct kbase_wait_ctx wait = kbase_wait_init(k, 1 * 1000000000LL);

        while (kbase_wait_for_event(&wait)) {
                kbase_syncobj_update(k, o);

                if (list_is_empty(&o->fences)) {
                        kbase_wait_fini(wait);
                        return true;
                }
        }

        kbase_wait_fini(wait);

        fprintf(stderr, "syncobj %p wait timeout\n", o);
        return false;
}

static bool
kbase_poll_event(kbase k, int64_t timeout_ns)
{
        struct pollfd pfd = {
                .fd = k->fd,
                .events = POLLIN,
        };

        struct timespec t = {
                .tv_sec = timeout_ns / 1000000000,
                .tv_nsec = timeout_ns % 1000000000,
        };

        int ret = ppoll(&pfd, 1, &t, NULL);

        if (ret == -1 && errno != EINTR)
                perror("poll(mali fd)");

        LOG("poll returned %i\n", pfd.revents);

        return ret != 0;
}

#if PAN_BASE_API < 2
static bool
kbase_handle_events(kbase k)
{
        struct base_jd_event_v2 event;
        bool ret = true;

        for (;;) {
                int ret = read(k->fd, &event, sizeof(event));

                if (ret == -1) {
                        if (errno == EAGAIN) {
                                return true;
                        } else {
                                perror("read(mali fd)");
                                return false;
                        }
                }

                if (event.event_code != BASE_JD_EVENT_DONE) {
                        fprintf(stderr, "Atom %i reported event 0x%x!\n",
                                event.atom_number, event.event_code);
                        ret = false;
                }

                pthread_mutex_lock(&k->handle_lock);

                k->event_slots[event.atom_number].last = event.udata.blob[0];

                unsigned size = util_dynarray_num_elements(&k->gem_handles,
                                                           kbase_handle);
                kbase_handle *handle_data = util_dynarray_begin(&k->gem_handles);

                struct util_dynarray *handles = k->atom_bos + event.atom_number;

                util_dynarray_foreach(handles, int32_t, h) {
                        if (*h >= size)
                                continue;
                        assert(handle_data[*h].use_count);
                        --handle_data[*h].use_count;
                }
                util_dynarray_fini(handles);

                pthread_mutex_unlock(&k->handle_lock);
        }

        return ret;
}

#else

static bool
kbase_read_event(kbase k)
{
        struct base_csf_notification event;
        int ret = read(k->fd, &event, sizeof(event));

        if (ret == -1) {
                if (errno == EAGAIN) {
                        return true;
                } else {
                        perror("read(mali_fd)");
                        return false;
                }
        }

        if (ret != sizeof(event)) {
                fprintf(stderr, "read(mali_fd) returned %i, expected %i!\n",
                        ret, (int) sizeof(event));
                return false;
        }

        switch (event.type) {
        case BASE_CSF_NOTIFICATION_EVENT:
                LOG("Notification event!\n");
                return true;

        case BASE_CSF_NOTIFICATION_GPU_QUEUE_GROUP_ERROR:
                break;

        case BASE_CSF_NOTIFICATION_CPU_QUEUE_DUMP:
                fprintf(stderr, "No event from mali_fd!\n");
                return true;

        default:
                fprintf(stderr, "Unknown event type!\n");
                return true;
        }

        struct base_gpu_queue_group_error e = event.payload.csg_error.error;

        switch (e.error_type) {
        case BASE_GPU_QUEUE_GROUP_ERROR_FATAL: {
                // See CS_FATAL_EXCEPTION_* in mali_gpu_csf_registers.h
                fprintf(stderr, "Queue group error: status 0x%x "
                        "sideband 0x%"PRIx64"\n",
                        e.payload.fatal_group.status,
                        (uint64_t) e.payload.fatal_group.sideband);
                break;
        }
        case BASE_GPU_QUEUE_GROUP_QUEUE_ERROR_FATAL: {
                unsigned queue = e.payload.fatal_queue.csi_index;

                // See CS_FATAL_EXCEPTION_* in mali_gpu_csf_registers.h
                fprintf(stderr, "Queue %i error: status 0x%x "
                        "sideband 0x%"PRIx64"\n",
                        queue, e.payload.fatal_queue.status,
                        (uint64_t) e.payload.fatal_queue.sideband);

                /* TODO: Decode the instruct that it got stuck at */

                break;
        }

        case BASE_GPU_QUEUE_GROUP_ERROR_TIMEOUT:
                fprintf(stderr, "Command stream timeout!\n");
                break;
        case BASE_GPU_QUEUE_GROUP_ERROR_TILER_HEAP_OOM:
                fprintf(stderr, "Command stream OOM!\n");
                break;
        default:
                fprintf(stderr, "Unknown error type!\n");
        }

        return false;
}

static void
kbase_update_queue_callbacks(kbase k,
                             struct kbase_event_slot *slot,
                             uint64_t seqnum)
{
        struct kbase_sync_link **list = &slot->syncobjs;
        struct kbase_sync_link **back = slot->back;

        while (*list) {
                struct kbase_sync_link *link = *list;

                LOG("seq %"PRIu64" %"PRIu64"\n", seqnum, link->seqnum);

                /* Items in the list should be in order, there is no need to
                 * check any more if we can't process this link yet. */
                if (seqnum <= link->seqnum)
                        break;

                LOG("done, calling %p(%p)\n", link->callback, link->data);
                link->callback(link->data);
                *list = link->next;
                if (&link->next == back)
                        slot->back = list;
                free(link);
        }
}

static bool
kbase_handle_events(kbase k)
{
#ifdef PAN_BASE_NOOP
        return true;
#endif

        /* This will clear the event count, so there's no need to do it in a
         * loop. */
        bool ret = kbase_read_event(k);

        uint64_t *event_mem = k->event_mem.cpu;

        pthread_mutex_lock(&k->queue_lock);

        for (unsigned i = 0; i < k->event_slot_usage; ++i) {
                uint64_t seqnum = event_mem[i * 2];
                uint64_t cmp = k->event_slots[i].last;

                LOG("MAIN SEQ %"PRIu64" > %"PRIu64"?\n", seqnum, cmp);

                if (seqnum < cmp) {
                        if (false)
                                fprintf(stderr, "seqnum at offset %i went backward "
                                        "from %"PRIu64" to %"PRIu64"!\n",
                                        i, cmp, seqnum);
                } else /*if (seqnum > cmp)*/ {
                        kbase_update_queue_callbacks(k, &k->event_slots[i],
                                                     seqnum);
                }

                /* TODO: Atomic operations? */
                k->event_slots[i].last = seqnum;
        }

        pthread_mutex_unlock(&k->queue_lock);

        return ret;
}

#endif

#if PAN_BASE_API < 2
static uint8_t
kbase_latest_slot(uint8_t a, uint8_t b, uint8_t newest)
{
        /* If a == 4 and newest == 5, a will become 255 */
        a -= newest;
        b -= newest;
        a = MAX2(a, b);
        a += newest;
        return a;
}

static int
kbase_submit(kbase k, uint64_t va, unsigned req,
             struct kbase_syncobj *o,
             int32_t *handles, unsigned num_handles)
{
        struct util_dynarray buf;
        util_dynarray_init(&buf, NULL);

        memcpy(util_dynarray_resize(&buf, int32_t, num_handles),
               handles, num_handles * sizeof(int32_t));

        pthread_mutex_lock(&k->handle_lock);

        unsigned slot = (req & PANFROST_JD_REQ_FS) ? 0 : 1;
        unsigned dep_slots[KBASE_SLOT_COUNT];

        uint8_t nr = k->atom_number++;

        struct base_jd_atom_v2 atom = {
                .jc = va,
                .atom_number = nr,
                .udata.blob[0] = k->job_seq++,
        };

        for (unsigned i = 0; i < KBASE_SLOT_COUNT; ++i)
                dep_slots[i] = nr;

        /* Make sure that we haven't taken an atom that's already in use. */
        assert(!k->atom_bos[nr].data);
        k->atom_bos[atom.atom_number] = buf;

        unsigned handle_buf_size = util_dynarray_num_elements(&k->gem_handles, kbase_handle);
        kbase_handle *handle_buf = util_dynarray_begin(&k->gem_handles);

        struct util_dynarray extres;
        util_dynarray_init(&extres, NULL);

        /* Mark the BOs as in use */
        for (unsigned i = 0; i < num_handles; ++i) {
                int32_t h = handles[i];
                assert(h < handle_buf_size);
                assert(handle_buf[h].use_count < 255);

                /* Implicit sync */
                if (handle_buf[h].use_count)
                        for (unsigned s = 0; s < KBASE_SLOT_COUNT; ++s)
                                dep_slots[s] =
                                        kbase_latest_slot(dep_slots[s],
                                                          handle_buf[h].last_access[s],
                                                          nr);

                handle_buf[h].last_access[slot] = nr;
                ++handle_buf[h].use_count;

                if (handle_buf[h].fd != -1)
                        util_dynarray_append(&extres, base_va, handle_buf[h].va);
        }

        pthread_mutex_unlock(&k->handle_lock);

        /* TODO: Better work out the difference between handle_lock and
         * queue_lock. */
        if (o) {
                pthread_mutex_lock(&k->queue_lock);
                kbase_syncobj_update_fence(o, nr, atom.udata.blob[0]);
                pthread_mutex_unlock(&k->queue_lock);
        }

        assert(KBASE_SLOT_COUNT == 2);
        if (dep_slots[0] != nr) {
                atom.pre_dep[0].atom_id = dep_slots[0];
                /* TODO: Use data dependencies?  */
                atom.pre_dep[0].dependency_type = BASE_JD_DEP_TYPE_ORDER;
        }
        if (dep_slots[1] != nr) {
                atom.pre_dep[1].atom_id = dep_slots[1];
                atom.pre_dep[1].dependency_type = BASE_JD_DEP_TYPE_ORDER;
        }

        if (extres.size) {
                atom.core_req |= BASE_JD_REQ_EXTERNAL_RESOURCES;
                atom.nr_extres = util_dynarray_num_elements(&extres, base_va);
                atom.extres_list = (uintptr_t) util_dynarray_begin(&extres);
        }

        if (req & PANFROST_JD_REQ_FS)
                atom.core_req |= BASE_JD_REQ_FS;
        else
                atom.core_req |= BASE_JD_REQ_CS | BASE_JD_REQ_T;

        struct kbase_ioctl_job_submit submit = {
                .nr_atoms = 1,
                .stride = sizeof(atom),
                .addr = (uintptr_t) &atom,
        };

        int ret = kbase_ioctl(k->fd, KBASE_IOCTL_JOB_SUBMIT, &submit);

        util_dynarray_fini(&extres);

        if (ret == -1) {
                perror("ioctl(KBASE_IOCTL_JOB_SUBMIT)");
                return -1;
        }

        return atom.atom_number;
}

#else
static struct kbase_context *
kbase_context_create(kbase k)
{
        struct kbase_context *c = calloc(1, sizeof(*c));

        if (!cs_group_create(k, c)) {
                free(c);
                return NULL;
        }

        if (!tiler_heap_create(k, c)) {
                cs_group_term(k, c);
                free(c);
                return NULL;
        }

        return c;
}

static void
kbase_kcpu_queue_destroy(kbase k, struct kbase_context *ctx);

static void
kbase_context_destroy(kbase k, struct kbase_context *ctx)
{
        kbase_kcpu_queue_destroy(k, ctx);
        tiler_heap_term(k, ctx);
        cs_group_term(k, ctx);
        free(ctx);
}

static bool
kbase_context_recreate(kbase k, struct kbase_context *ctx)
{
        kbase_kcpu_queue_destroy(k, ctx);
        tiler_heap_term(k, ctx);
        cs_group_term(k, ctx);

        if (!cs_group_create(k, ctx)) {
                free(ctx);
                return false;
        }

        if (!tiler_heap_create(k, ctx)) {
                free(ctx);
                return false;
        }

        return true;
}

static struct kbase_cs
kbase_cs_bind_noevent(kbase k, struct kbase_context *ctx,
                      base_va va, unsigned size, unsigned csi)
{
        struct kbase_cs cs = {
                .ctx = ctx,
                .va = va,
                .size = size,
                .csi = csi,
                .latest_flush = (uint32_t *)k->csf_user_reg,
        };

        struct kbase_ioctl_cs_queue_register reg = {
                .buffer_gpu_addr = va,
                .buffer_size = size,
                .priority = 1,
        };

        int ret = kbase_ioctl(k->fd, KBASE_IOCTL_CS_QUEUE_REGISTER, &reg);

        if (ret == -1) {
                perror("ioctl(KBASE_IOCTL_CS_QUEUE_REGISTER)");
                return cs;
        }

        union kbase_ioctl_cs_queue_bind bind = {
                .in = {
                        .buffer_gpu_addr = va,
                        .group_handle = ctx->csg_handle,
                        .csi_index = csi,
                }
        };

        ret = kbase_ioctl(k->fd, KBASE_IOCTL_CS_QUEUE_BIND, &bind);

        if (ret == -1) {
                perror("ioctl(KBASE_IOCTL_CS_QUEUE_BIND)");
                // hack
                cs.user_io = (void *)1;
                return cs;
        }

        cs.user_io =
                kbase_mmap(NULL,
                           k->page_size * BASEP_QUEUE_NR_MMAP_USER_PAGES,
                           PROT_READ | PROT_WRITE, MAP_SHARED,
                           k->fd, bind.out.mmap_handle);

        if (cs.user_io == MAP_FAILED) {
                perror("mmap(CS USER IO)");
                cs.user_io = NULL;
        }

        return cs;
}

static struct kbase_cs
kbase_cs_bind(kbase k, struct kbase_context *ctx,
              base_va va, unsigned size)
{
        struct kbase_cs cs = kbase_cs_bind_noevent(k, ctx, va, size, ctx->num_csi++);

        // TODO: Fix this problem properly
        if (k->event_slot_usage >= 256) {
                fprintf(stderr, "error: Too many contexts created!\n");

                /* *very* dangerous, but might just work */
                --k->event_slot_usage;
        }

        // TODO: This is a misnomer... it isn't a byte offset
        cs.event_mem_offset = k->event_slot_usage++;
        k->event_slots[cs.event_mem_offset].back =
                &k->event_slots[cs.event_mem_offset].syncobjs;

        uint64_t *event_data = k->event_mem.cpu + cs.event_mem_offset * PAN_EVENT_SIZE;

        /* We use the "Higher" wait condition, so initialise to 1 to allow
         * waiting before writing... */
        event_data[0] = 1;
        /* And reset the error field to 0, to avoid INHERITing faults */
        event_data[1] = 0;

        /* Just a zero-init is fine... reads and writes are always paired */
        uint64_t *kcpu_data = k->kcpu_event_mem.cpu + cs.event_mem_offset * PAN_EVENT_SIZE;
        kcpu_data[0] = 0;
        kcpu_data[1] = 0;

        /* To match the event data */
        k->event_slots[cs.event_mem_offset].last = 1;
        k->event_slots[cs.event_mem_offset].last_submit = 1;

        return cs;
}

static void
kbase_cs_term(kbase k, struct kbase_cs *cs)
{
        if (cs->user_io) {
                LOG("unmapping %p user_io %p\n", cs, cs->user_io);
                munmap(cs->user_io,
                       k->page_size * BASEP_QUEUE_NR_MMAP_USER_PAGES);
        }

        struct kbase_ioctl_cs_queue_terminate term = {
                .buffer_gpu_addr = cs->va,
        };

        kbase_ioctl(k->fd, KBASE_IOCTL_CS_QUEUE_TERMINATE, &term);

        pthread_mutex_lock(&k->queue_lock);
        kbase_update_queue_callbacks(k, &k->event_slots[cs->event_mem_offset],
                                     ~0ULL);

        k->event_slots[cs->event_mem_offset].last = ~0ULL;

        /* Make sure that no syncobjs are referencing this CS */
        list_for_each_entry(struct kbase_syncobj, o, &k->syncobjs, link)
                kbase_syncobj_update(k, o);


        k->event_slots[cs->event_mem_offset].last = 0;
        pthread_mutex_unlock(&k->queue_lock);
}

static void
kbase_cs_rebind(kbase k, struct kbase_cs *cs)
{
        struct kbase_cs new;
        new = kbase_cs_bind_noevent(k, cs->ctx, cs->va, cs->size, cs->csi);

        cs->user_io = new.user_io;
        LOG("remapping %p user_io %p\n", cs, cs->user_io);

        fprintf(stderr, "bound csi %i again\n", cs->csi);
}

static bool
kbase_cs_kick(kbase k, struct kbase_cs *cs)
{
        struct kbase_ioctl_cs_queue_kick kick = {
                .buffer_gpu_addr = cs->va,
        };

        int ret = kbase_ioctl(k->fd, KBASE_IOCTL_CS_QUEUE_KICK, &kick);

        if (ret == -1) {
                perror("ioctl(KBASE_IOCTL_CS_QUEUE_KICK)");
                return false;
        }

        return true;
}

#define CS_RING_DOORBELL(cs) \
        *((uint32_t *)(cs->user_io)) = 1

#define CS_READ_REGISTER(cs, r) \
        *((uint64_t *)(cs->user_io + 4096 * 2 + r))

#define CS_WRITE_REGISTER(cs, r, v) \
        *((uint64_t *)(cs->user_io + 4096 + r)) = v

static bool
kbase_cs_submit(kbase k, struct kbase_cs *cs, uint64_t insert_offset,
                struct kbase_syncobj *o, uint64_t seqnum)
{
        LOG("submit %p, seq %"PRIu64", insert %"PRIu64" -> %"PRIu64"\n",
            cs, seqnum, cs->last_insert, insert_offset);

        if (!cs->user_io)
                return false;

        if (insert_offset == cs->last_insert)
                return true;

#ifndef PAN_BASE_NOOP
        struct kbase_event_slot *slot =
                &k->event_slots[cs->event_mem_offset];

        pthread_mutex_lock(&k->queue_lock);
        slot->last_submit = seqnum + 1;

        if (o)
                kbase_syncobj_update_fence(o, cs->event_mem_offset, seqnum);
        pthread_mutex_unlock(&k->queue_lock);
#endif

        memory_barrier();

        bool active = CS_READ_REGISTER(cs, CS_ACTIVE);
        LOG("active is %i\n", active);

        CS_WRITE_REGISTER(cs, CS_INSERT, insert_offset);
        cs->last_insert = insert_offset;

        if (false /*active*/) {
                memory_barrier();
                CS_RING_DOORBELL(cs);
                memory_barrier();

                active = CS_READ_REGISTER(cs, CS_ACTIVE);
                LOG("active is now %i\n", active);
        } else {
                kbase_cs_kick(k, cs);
        }

        return true;
}

static bool
kbase_cs_wait(kbase k, struct kbase_cs *cs, uint64_t extract_offset,
              struct kbase_syncobj *o)
{
        if (!cs->user_io)
                return false;

        if (kbase_syncobj_wait(k, o))
                return true;

        uint64_t e = CS_READ_REGISTER(cs, CS_EXTRACT);
        unsigned a = CS_READ_REGISTER(cs, CS_ACTIVE);

        fprintf(stderr, "CSI %i CS_EXTRACT (%"PRIu64") != %"PRIu64", "
                "CS_ACTIVE (%i)\n",
                cs->csi, e, extract_offset, a);

        fprintf(stderr, "fences:\n");
        list_for_each_entry(struct kbase_fence, fence, &o->fences, link) {
                fprintf(stderr, " slot %i: seqnum %"PRIu64"\n",
                        fence->slot, fence->value);
        }

        return false;
}

static bool
kbase_kcpu_queue_create(kbase k, struct kbase_context *ctx)
{
#ifdef PAN_BASE_NOOP
        return false;
#endif

        if (ctx->kcpu_init)
                return true;

        struct kbase_ioctl_kcpu_queue_new create = {0};

        int ret;
        ret = ioctl(k->fd, KBASE_IOCTL_KCPU_QUEUE_CREATE, &create);

        if (ret == -1) {
                perror("ioctl(KBASE_IOCTL_KCPU_QUEUE_CREATE)");
                return false;
        }

        ctx->kcpu_queue = create.id;
        ctx->kcpu_init = true;
        return true;
}

static void
kbase_kcpu_queue_destroy(kbase k, struct kbase_context *ctx)
{
        if (!ctx->kcpu_init)
                return;

        struct kbase_ioctl_kcpu_queue_delete destroy = {
                .id = ctx->kcpu_queue,
        };

        int ret;
        ret = ioctl(k->fd, KBASE_IOCTL_KCPU_QUEUE_DELETE, &destroy);

        if (ret == -1) {
                perror("ioctl(KBASE_IOCTL_KCPU_QUEUE_DELETE)");
        }

        ctx->kcpu_init = false;
}

static bool
kbase_kcpu_command(kbase k, struct kbase_context *ctx, struct base_kcpu_command *cmd)
{
        int err;
        bool ret = true;

        if (!kbase_kcpu_queue_create(k, ctx))
                return false;

        struct kbase_ioctl_kcpu_queue_enqueue enqueue = {
                .addr = (uintptr_t) cmd,
                .nr_commands = 1,
                .id = ctx->kcpu_queue,
        };

        err = kbase_ioctl(k->fd, KBASE_IOCTL_KCPU_QUEUE_ENQUEUE, &enqueue);
        if (err != -1)
                return ret;

        /* If the enqueue failed, probably we hit the limit of enqueued
         * commands (256), wait a bit and try again.
         */

        struct kbase_wait_ctx wait = kbase_wait_init(k, 1000000000);
        while (kbase_wait_for_event(&wait)) {
                err = kbase_ioctl(k->fd, KBASE_IOCTL_KCPU_QUEUE_ENQUEUE, &enqueue);
                if (err != -1)
                        break;

                if (errno != EBUSY) {
                        ret = false;
                        perror("ioctl(KBASE_IOCTL_KCPU_QUEUE_ENQUEUE");
                        break;
                }
        }
        kbase_wait_fini(wait);

        return ret;
}

static int
kbase_kcpu_fence_export(kbase k, struct kbase_context *ctx)
{
        struct base_fence fence = {
                .basep.fd = -1,
        };

        struct base_kcpu_command fence_cmd = {
                .type = BASE_KCPU_COMMAND_TYPE_FENCE_SIGNAL,
                .info.fence.fence = (uintptr_t) &fence,
        };

        return kbase_kcpu_command(k, ctx, &fence_cmd) ? fence.basep.fd : -1;
}

static bool
kbase_kcpu_fence_import(kbase k, struct kbase_context *ctx, int fd)
{
        struct base_kcpu_command fence_cmd = {
                .type = BASE_KCPU_COMMAND_TYPE_FENCE_WAIT,
                .info.fence.fence = (uintptr_t) &(struct base_fence) {
                        .basep.fd = fd,
                },
        };

        return kbase_kcpu_command(k, ctx, &fence_cmd);
}

static bool
kbase_kcpu_cqs_set(kbase k, struct kbase_context *ctx,
                   base_va addr, uint64_t value)
{
        struct base_kcpu_command set_cmd = {
                .type = BASE_KCPU_COMMAND_TYPE_CQS_SET_OPERATION,
                .info.cqs_set_operation = {
                        .objs = (uintptr_t) &(struct base_cqs_set_operation_info) {
                                .addr = addr,
                                .val = value,
                                .operation = BASEP_CQS_SET_OPERATION_SET,
                                .data_type = BASEP_CQS_DATA_TYPE_U64,
                        },
                        .nr_objs = 1,
                },
        };

        return kbase_kcpu_command(k, ctx, &set_cmd);
}

static bool
kbase_kcpu_cqs_wait(kbase k, struct kbase_context *ctx,
                    base_va addr, uint64_t value)
{
        struct base_kcpu_command wait_cmd = {
                .type = BASE_KCPU_COMMAND_TYPE_CQS_WAIT_OPERATION,
                .info.cqs_wait_operation = {
                        .objs = (uintptr_t) &(struct base_cqs_wait_operation_info) {
                                .addr = addr,
                                .val = value,
                                .operation = BASEP_CQS_WAIT_OPERATION_GT,
                                .data_type = BASEP_CQS_DATA_TYPE_U64,
                        },
                        .nr_objs = 1,
                        .inherit_err_flags = 0,
                },
        };

        return kbase_kcpu_command(k, ctx, &wait_cmd);
}
#endif

// TODO: Only define for CSF kbases?
static bool
kbase_callback_all_queues(kbase k, int32_t *count,
                          void (*callback)(void *), void *data)
{
        pthread_mutex_lock(&k->queue_lock);

        int32_t queue_count = 0;

        for (unsigned i = 0; i < k->event_slot_usage; ++i) {
                struct kbase_event_slot *slot = &k->event_slots[i];

                /* There is no need to do anything for idle slots */
                if (slot->last == slot->last_submit)
                        continue;

                struct kbase_sync_link *link = malloc(sizeof(*link));
                *link = (struct kbase_sync_link) {
                        .next = NULL,
                        .seqnum = slot->last_submit,
                        .callback = callback,
                        .data = data,
                };

                // TODO: Put insertion code into its own function
                struct kbase_sync_link **list = slot->back;
                slot->back = &link->next;
                assert(!*list);
                *list = link;

                ++queue_count;
        }

        p_atomic_add(count, queue_count);

        pthread_mutex_unlock(&k->queue_lock);

        return queue_count != 0;
}

static void
kbase_mem_sync(kbase k, base_va gpu, void *cpu, size_t size,
               bool invalidate)
{
#ifdef __aarch64__
        /* Valgrind replaces the operations with DC CVAU, which is not enough
         * for CPU<->GPU coherency. The ioctl can be used instead. */
        if (!RUNNING_ON_VALGRIND) {
                /* I don't that memory barriers are needed here... having the
                 * DMB SY before submit should be enough. TODO what about
                 * dma-bufs? */
                if (invalidate)
                        cache_invalidate_range(cpu, size);
                else
                        cache_clean_range(cpu, size);
                return;
        }
#endif

        struct kbase_ioctl_mem_sync sync = {
                .handle = gpu,
                .user_addr = (uintptr_t) cpu,
                .size = size,
                .type = invalidate + (PAN_BASE_API == 0 ? 0 : 1),
        };

        int ret;
        ret = kbase_ioctl(k->fd, KBASE_IOCTL_MEM_SYNC, &sync);
        if (ret == -1)
                perror("ioctl(KBASE_IOCTL_MEM_SYNC)");
}

bool
#if defined(PAN_BASE_NOOP)
kbase_open_csf_noop
#elif PAN_BASE_API == 0
kbase_open_old
#elif PAN_BASE_API == 1
kbase_open_new
#elif PAN_BASE_API == 2
kbase_open_csf
#endif
(kbase k)
{
        k->api = PAN_BASE_API;

        pthread_mutex_init(&k->handle_lock, NULL);
        pthread_mutex_init(&k->event_read_lock, NULL);
        pthread_mutex_init(&k->event_cnd_lock, NULL);
        pthread_mutex_init(&k->queue_lock, NULL);

        pthread_condattr_t attr;
        pthread_condattr_init(&attr);
        pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
        pthread_cond_init(&k->event_cnd, &attr);
        pthread_condattr_destroy(&attr);

        list_inithead(&k->syncobjs);

        /* For later APIs, we've already checked the version in pan_base.c */
#if PAN_BASE_API == 0
        struct kbase_ioctl_get_version ver = { 0 };
        kbase_ioctl(k->fd, KBASE_IOCTL_GET_VERSION, &ver);
#endif

        k->close = kbase_close;

        k->get_pan_gpuprop = kbase_get_pan_gpuprop;
        k->get_mali_gpuprop = kbase_get_mali_gpuprop;

        k->alloc = kbase_alloc;
        k->free = kbase_free;
        k->import_dmabuf = kbase_import_dmabuf;
        k->mmap_import = kbase_mmap_import;

        k->poll_event = kbase_poll_event;
        k->handle_events = kbase_handle_events;

#if PAN_BASE_API < 2
        k->submit = kbase_submit;
#else
        k->context_create = kbase_context_create;
        k->context_destroy = kbase_context_destroy;
        k->context_recreate = kbase_context_recreate;

        k->cs_bind = kbase_cs_bind;
        k->cs_term = kbase_cs_term;
        k->cs_rebind = kbase_cs_rebind;
        k->cs_submit = kbase_cs_submit;
        k->cs_wait = kbase_cs_wait;

        k->kcpu_fence_export = kbase_kcpu_fence_export;
        k->kcpu_fence_import = kbase_kcpu_fence_import;
        k->kcpu_cqs_set = kbase_kcpu_cqs_set;
        k->kcpu_cqs_wait = kbase_kcpu_cqs_wait;
#endif

        k->syncobj_create = kbase_syncobj_create;
        k->syncobj_destroy = kbase_syncobj_destroy;
        k->syncobj_dup = kbase_syncobj_dup;
        k->syncobj_wait = kbase_syncobj_wait;

        k->callback_all_queues = kbase_callback_all_queues;

        k->mem_sync = kbase_mem_sync;

        for (unsigned i = 0; i < ARRAY_SIZE(kbase_main); ++i) {
                ++k->setup_state;
                if (!kbase_main[i].part(k)) {
                        k->close(k);
                        return false;
                }
        }
        return true;
}
