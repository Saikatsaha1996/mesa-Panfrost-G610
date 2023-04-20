/*
 * Copyright (C) 2019 Collabora, Ltd.
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
 * Authors:
 *   Alyssa Rosenzweig <alyssa.rosenzweig@collabora.com>
 */

#include <fcntl.h>
#include <xf86drm.h>

#include "util/u_math.h"
#include "util/macros.h"
#include "util/hash_table.h"
#include "util/u_thread.h"
#include "drm-uapi/panfrost_drm.h"
#include "dma-uapi/dma-buf.h"
#include "pan_encoder.h"
#include "pan_device.h"
#include "pan_bo.h"
#include "pan_texture.h"
#include "wrap.h"
#include "pan_util.h"
#include "pan_base.h"

/* Fixed "minimum revisions" */
#define NO_ANISO (~0)
#define HAS_ANISO (0)

#define MODEL(gpu_id_, shortname, counters_, min_rev_anisotropic_, tib_size_, quirks_) \
        { \
                .gpu_id = gpu_id_, \
                .name = "Mali-" shortname " (Panfrost)", \
                .performance_counters = counters_, \
                .min_rev_anisotropic = min_rev_anisotropic_, \
                .tilebuffer_size = tib_size_, \
                .quirks = quirks_, \
        }

/* Table of supported Mali GPUs */
const struct panfrost_model panfrost_model_list[] = {
        MODEL(0x620, "T620", "T62x", NO_ANISO, 8192, {}),
        MODEL(0x720, "T720", "T72x", NO_ANISO, 8192, { .no_hierarchical_tiling = true }),
        MODEL(0x750, "T760", "T76x", NO_ANISO, 8192, {}),
        MODEL(0x820, "T820", "T82x", NO_ANISO, 8192, { .no_hierarchical_tiling = true }),
        MODEL(0x830, "T830", "T83x", NO_ANISO, 8192, { .no_hierarchical_tiling = true }),
        MODEL(0x860, "T860", "T86x", NO_ANISO, 8192, {}),
        MODEL(0x880, "T880", "T88x", NO_ANISO, 8192, {}),

        MODEL(0x6000, "G71", "TMIx", NO_ANISO, 8192, {}),
        MODEL(0x6221, "G72", "THEx", 0x0030 /* r0p3 */, 16384, {}),
        MODEL(0x7090, "G51", "TSIx", 0x1010 /* r1p1 */, 16384, {}),
        MODEL(0x7093, "G31", "TDVx", HAS_ANISO, 16384, {}),
        MODEL(0x7211, "G76", "TNOx", HAS_ANISO, 16384, {}),
        MODEL(0x7212, "G52", "TGOx", HAS_ANISO, 16384, {}),
        MODEL(0x7402, "G52 r1", "TGOx", HAS_ANISO, 16384, {}),
        MODEL(0x9093, "G57", "TNAx", HAS_ANISO, 16384, {}),
        MODEL(0xa867, "G610", "LODx", HAS_ANISO, 65536, {}),
        /* Matching the kbase dummy model, probably not real GPUs */
        MODEL(0xa802, "G710", "TODx", HAS_ANISO, 65536, {}),
};

#undef NO_ANISO
#undef HAS_ANISO
#undef MODEL

/*
 * Look up a supported model by its GPU ID, or return NULL if the model is not
 * supported at this time.
 */
const struct panfrost_model *
panfrost_get_model(uint32_t gpu_id)
{
        for (unsigned i = 0; i < ARRAY_SIZE(panfrost_model_list); ++i) {
                if (panfrost_model_list[i].gpu_id == gpu_id)
                        return &panfrost_model_list[i];
        }

        return NULL;
}

/* Abstraction over the raw drm_panfrost_get_param ioctl for fetching
 * information about devices */

static __u64
panfrost_query_raw(
                struct panfrost_device *dev,
                enum drm_panfrost_param param,
                bool required,
                unsigned default_value)
{
        if (dev->kbase) {
                uint64_t value;
                bool ret = dev->mali.get_pan_gpuprop(&dev->mali, param, &value);
                if (ret) {
                        return value;
                } else {
                        assert(!required);
                        return default_value;
                }
        }

        struct drm_panfrost_get_param get_param = {0,};
        ASSERTED int ret;

        get_param.param = param;
        ret = drmIoctl(dev->fd, DRM_IOCTL_PANFROST_GET_PARAM, &get_param);

        if (ret) {
                assert(!required);
                return default_value;
        }

        return get_param.value;
}

static unsigned
panfrost_query_gpu_version(struct panfrost_device *dev)
{
        return panfrost_query_raw(dev, DRM_PANFROST_PARAM_GPU_PROD_ID, true, 0);
}

static unsigned
panfrost_query_gpu_revision(struct panfrost_device *dev)
{
        return panfrost_query_raw(dev, DRM_PANFROST_PARAM_GPU_REVISION, true, 0);
}

unsigned
panfrost_query_l2_slices(struct panfrost_device *dev)
{
        /* Query MEM_FEATURES register */
        uint32_t mem_features =
                panfrost_query_raw(dev, DRM_PANFROST_PARAM_MEM_FEATURES,
                                   true, 0);

        /* L2_SLICES is MEM_FEATURES[11:8] minus(1) */
        return ((mem_features >> 8) & 0xF) + 1;
}

static struct panfrost_tiler_features
panfrost_query_tiler_features(struct panfrost_device *dev)
{
        /* Default value (2^9 bytes and 8 levels) to match old behaviour */
        uint32_t raw = panfrost_query_raw(dev, DRM_PANFROST_PARAM_TILER_FEATURES,
                        false, 0x809);

        /* Bin size is log2 in the first byte, max levels in the second byte */
        return (struct panfrost_tiler_features) {
                .bin_size = (1 << (raw & BITFIELD_MASK(5))),
                .max_levels = (raw >> 8) & BITFIELD_MASK(4)
        };
}

static unsigned
panfrost_query_core_count(struct panfrost_device *dev, unsigned *core_id_range)
{
        /* On older kernels, worst-case to 16 cores */

        unsigned mask = panfrost_query_raw(dev,
                        DRM_PANFROST_PARAM_SHADER_PRESENT, false, 0xffff);

        /* Some cores might be absent. In some cases, we care
         * about the range of core IDs (that is, the greatest core ID + 1). If
         * the core mask is contiguous, this equals the core count.
         */
        *core_id_range = util_last_bit(mask);

        /* The actual core count skips overs the gaps */
        return util_bitcount(mask);
}

/* Architectural maximums, since this register may be not implemented
 * by a given chip. G31 is actually 512 instead of 768 but it doesn't
 * really matter. */

static unsigned
panfrost_max_thread_count(unsigned arch)
{
        switch (arch) {
        /* Midgard */
        case 4:
        case 5:
                return 256;

        /* Bifrost, first generation */
        case 6:
                return 384;

        /* Bifrost, second generation (G31 is 512 but it doesn't matter) */
        case 7:
                return 768;

        /* Valhall (for completeness) */
        default:
                return 1024;
        }
}

static unsigned
panfrost_query_thread_tls_alloc(struct panfrost_device *dev, unsigned major)
{
        unsigned tls = panfrost_query_raw(dev,
                        DRM_PANFROST_PARAM_THREAD_TLS_ALLOC, false, 0);

        return (tls > 0) ? tls : panfrost_max_thread_count(major);
}

static uint32_t
panfrost_query_compressed_formats(struct panfrost_device *dev)
{
        /* If unspecified, assume ASTC/ETC only. Factory default for Juno, and
         * should exist on any Mali configuration. All hardware should report
         * these texture formats but the kernel might not be new enough. */

        uint32_t default_set =
                (1 << MALI_ETC2_RGB8) |
                (1 << MALI_ETC2_R11_UNORM) |
                (1 << MALI_ETC2_RGBA8) |
                (1 << MALI_ETC2_RG11_UNORM) |
                (1 << MALI_ETC2_R11_SNORM) |
                (1 << MALI_ETC2_RG11_SNORM) |
                (1 << MALI_ETC2_RGB8A1) |
                (1 << MALI_ASTC_3D_LDR) |
                (1 << MALI_ASTC_3D_HDR) |
                (1 << MALI_ASTC_2D_LDR) |
                (1 << MALI_ASTC_2D_HDR);

        return panfrost_query_raw(dev, DRM_PANFROST_PARAM_TEXTURE_FEATURES0,
                        false, default_set);
}

/* DRM_PANFROST_PARAM_TEXTURE_FEATURES0 will return a bitmask of supported
 * compressed formats, so we offer a helper to test if a format is supported */

bool
panfrost_supports_compressed_format(struct panfrost_device *dev, unsigned fmt)
{
        if (MALI_EXTRACT_TYPE(fmt) != MALI_FORMAT_COMPRESSED)
                return true;

        unsigned idx = fmt & ~MALI_FORMAT_COMPRESSED;
        assert(idx < 32);

        return dev->compressed_formats & (1 << idx);
}

/* Check for AFBC hardware support. AFBC is introduced in v5. Implementations
 * may omit it, signaled as a nonzero value in the AFBC_FEATURES property. */

static bool
panfrost_query_afbc(struct panfrost_device *dev, unsigned arch)
{
        unsigned reg = panfrost_query_raw(dev,
                                          DRM_PANFROST_PARAM_AFBC_FEATURES,
                                          false, 0);

        return (arch >= 5) && (reg == 0);
}

/*
 * To pipeline multiple tiles, a given tile may use at most half of the tile
 * buffer. This function returns the optimal size (assuming pipelining).
 *
 * For Mali-G510 and Mali-G310, we will need extra logic to query the tilebuffer
 * size for the particular variant. The CORE_FEATURES register might help.
 */
static unsigned
panfrost_query_optimal_tib_size(const struct panfrost_device *dev)
{
        /* Preconditions ensure the returned value is a multiple of 1 KiB, the
         * granularity of the colour buffer allocation field.
         */
        assert(dev->model->tilebuffer_size >= 2048);
        assert(util_is_power_of_two_nonzero(dev->model->tilebuffer_size));

        return dev->model->tilebuffer_size / 2;
}

void
panfrost_open_device(void *memctx, int fd, struct panfrost_device *dev)
{
        if (kbase_open(&dev->mali, fd, 4, (dev->debug & PAN_DBG_LOG))) {
                dev->kbase = true;
                fd = -1;
        }

        dev->fd = fd;
        dev->memctx = memctx;
        dev->gpu_id = panfrost_query_gpu_version(dev);
        dev->arch = pan_arch(dev->gpu_id);
        if (dev->kbase) {
                dev->kernel_version = calloc(1, sizeof(drmVersion));
                *dev->kernel_version = (drmVersion) {
                        .version_major = 1,
                        .version_minor = 999,
                };
        } else {
                dev->kernel_version = drmGetVersion(fd);
        }
        dev->revision = panfrost_query_gpu_revision(dev);
        dev->model = panfrost_get_model(dev->gpu_id);

        /* If we don't recognize the model, bail early */
        if (!dev->model)
                return;

        if (dev->debug & PAN_DBG_BO_LOG)
                dev->bo_log = fopen("/tmp/bo_log", "w");

        dev->core_count = panfrost_query_core_count(dev, &dev->core_id_range);
        dev->thread_tls_alloc = panfrost_query_thread_tls_alloc(dev, dev->arch);
        dev->optimal_tib_size = panfrost_query_optimal_tib_size(dev);
        dev->compressed_formats = panfrost_query_compressed_formats(dev);
        dev->tiler_features = panfrost_query_tiler_features(dev);
        dev->has_afbc = panfrost_query_afbc(dev, dev->arch);

        if (dev->arch <= 6)
                dev->formats = panfrost_pipe_format_v6;
        else if (dev->arch <= 7)
                dev->formats = panfrost_pipe_format_v7;
        else
                dev->formats = panfrost_pipe_format_v9;

        stable_array_init(&dev->bo_map, struct panfrost_bo);

        pthread_mutex_init(&dev->bo_usage_lock, NULL);
        pthread_mutex_init(&dev->bo_map_lock, NULL);
        pthread_mutex_init(&dev->bo_cache.lock, NULL);
        list_inithead(&dev->bo_cache.lru);

        for (unsigned i = 0; i < ARRAY_SIZE(dev->bo_cache.buckets); ++i)
                list_inithead(&dev->bo_cache.buckets[i]);

        /* Initialize pandecode before we start allocating */
        if (dev->debug & (PAN_DBG_TRACE | PAN_DBG_SYNC))
                pandecode_initialize(!(dev->debug & PAN_DBG_TRACE));

        /* Tiler heap is internally required by the tiler, which can only be
         * active for a single job chain at once, so a single heap can be
         * shared across batches/contextes */

        if (dev->arch < 10)
                dev->tiler_heap = panfrost_bo_create(dev, 128 * 1024 * 1024,
                                             PAN_BO_INVISIBLE | PAN_BO_GROWABLE, "Tiler heap");

        pthread_mutex_init(&dev->submit_lock, NULL);

        /* Done once on init */
        panfrost_upload_sample_positions(dev);
}

void
panfrost_close_device(struct panfrost_device *dev)
{
        /* If we don't recognize the model, the rest of the device won't exist,
         * we will have early-exited the device open.
         */
        if (dev->model) {
                pthread_mutex_destroy(&dev->submit_lock);
                panfrost_bo_unreference(dev->tiler_heap);
                panfrost_bo_unreference(dev->sample_positions);
                panfrost_bo_cache_evict_all(dev);
                pthread_mutex_destroy(&dev->bo_cache.lock);
                pthread_mutex_destroy(&dev->bo_map_lock);
                pthread_mutex_destroy(&dev->bo_usage_lock);
                stable_array_fini(&dev->bo_map);
        }

        if (dev->kbase)
                free(dev->kernel_version);
        else
                drmFreeVersion(dev->kernel_version);
        if (dev->kbase)
                dev->mali.close(&dev->mali);
        else
                close(dev->fd);
}

bool
panfrost_check_dmabuf_fence(struct panfrost_device *dev)
{
        bool ret = false;
        int err;

        /* This function is only useful for kbase, where we can't create
         * dma-bufs from the kbase FD. */
        if (!dev->ro)
                goto out;

        struct drm_mode_create_dumb create_dumb = {
                .width = 16,
                .height = 16,
                .bpp = 32,
        };

        err = drmIoctl(dev->ro->kms_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_dumb);
        if (err < 0) {
                fprintf(stderr, "DRM_IOCTL_MODE_CREATE_DUMB failed "
                        "for fence check: %s\n",
                        strerror(errno));
                goto out;
        }

        int fd;
        err = drmPrimeHandleToFD(dev->ro->kms_fd, create_dumb.handle, O_CLOEXEC,
                                 &fd);
        if (err < 0) {
                fprintf(stderr, "failed to export buffer for fence check: %s\n",
                        strerror(errno));
                goto free_dumb;
        }

        struct dma_buf_export_sync_file export = {
                .flags = DMA_BUF_SYNC_RW,
        };

        /* ENOTTY is returned if the ioctl is unsupported */

        err = drmIoctl(fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &export);
        if (err < 0) {
                if (errno != ENOTTY)
                        fprintf(stderr, "failed to export fence: %s\n",
                                strerror(errno));
                goto free_fd;
        }

        struct dma_buf_import_sync_file import = {
                .flags = DMA_BUF_SYNC_RW,
                .fd = export.fd,
        };

        err = drmIoctl(fd, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &import);
        if (err < 0) {
                if (errno != ENOTTY)
                        fprintf(stderr, "failed to import fence: %s\n",
                                strerror(errno));
                goto free_sync;
        }

        /* We made it this far, the kernel must support the ioctls */
        ret = true;

free_sync:
        close(export.fd);

free_fd:
        close(fd);

        /* Some compilers don't like goto to a declaration */
        struct drm_mode_destroy_dumb destroy_dumb;
free_dumb:
        destroy_dumb = (struct drm_mode_destroy_dumb) {
                .handle = create_dumb.handle,
        };
        drmIoctl(dev->ro->kms_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_dumb);

out:
        return ret;
}
