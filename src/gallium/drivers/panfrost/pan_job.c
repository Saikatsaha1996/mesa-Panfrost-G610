/*
 * Copyright (C) 2019-2020 Collabora, Ltd.
 * Copyright (C) 2019 Alyssa Rosenzweig
 * Copyright (C) 2014-2017 Broadcom
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
 */

#include <assert.h>
#include <unistd.h>

#include "drm-uapi/panfrost_drm.h"

#include "pan_bo.h"
#include "pan_context.h"
#include "util/hash_table.h"
#include "util/ralloc.h"
#include "util/format/u_format.h"
#include "util/u_pack_color.h"
#include "util/rounding.h"
#include "util/u_framebuffer.h"
#include "pan_util.h"
#include "decode.h"

#define foreach_batch(ctx, idx) \
        BITSET_FOREACH_SET(idx, ctx->batches.active, PAN_MAX_BATCHES)

static unsigned
panfrost_batch_idx(struct panfrost_batch *batch)
{
        return batch - batch->ctx->batches.slots;
}

/* Adds the BO backing surface to a batch if the surface is non-null */

static void
panfrost_batch_add_surface(struct panfrost_batch *batch, struct pipe_surface *surf)
{
        if (surf) {
                struct panfrost_resource *rsrc = pan_resource(surf->texture);
                panfrost_batch_write_rsrc(batch, rsrc, PIPE_SHADER_FRAGMENT);
        }
}

static void
panfrost_batch_init(struct panfrost_context *ctx,
                    const struct pipe_framebuffer_state *key,
                    struct panfrost_batch *batch)
{
        struct pipe_screen *pscreen = ctx->base.screen;
        struct panfrost_screen *screen = pan_screen(pscreen);
        struct panfrost_device *dev = &screen->dev;

        batch->ctx = ctx;

        batch->seqnum = ++ctx->batches.seqnum;

        util_dynarray_init(&batch->bos, NULL);

        batch->minx = batch->miny = ~0;
        batch->maxx = batch->maxy = 0;

        util_copy_framebuffer_state(&batch->key, key);
        batch->resources =_mesa_set_create(NULL, _mesa_hash_pointer,
                                          _mesa_key_pointer_equal);

        for (unsigned i = 0; i < PAN_USAGE_COUNT; ++i)
                util_dynarray_init(&batch->resource_bos[i], NULL);

        util_dynarray_init(&batch->vert_deps, NULL);
        util_dynarray_init(&batch->frag_deps, NULL);

        util_dynarray_init(&batch->dmabufs, NULL);

        /* Preallocate the main pool, since every batch has at least one job
         * structure so it will be used */
        panfrost_pool_init(&batch->pool, NULL, dev, 0, 65536, "Batch pool", true, true);

        /* Don't preallocate the invisible pool, since not every batch will use
         * the pre-allocation, particularly if the varyings are larger than the
         * preallocation and a reallocation is needed after anyway. */
        panfrost_pool_init(&batch->invisible_pool, NULL, dev,
                        PAN_BO_INVISIBLE, 65536, "Varyings", false, true);

        for (unsigned i = 0; i < batch->key.nr_cbufs; ++i)
                panfrost_batch_add_surface(batch, batch->key.cbufs[i]);

        panfrost_batch_add_surface(batch, batch->key.zsbuf);

        if ((dev->debug & PAN_DBG_SYNC) || !(dev->debug & PAN_DBG_GOFASTER))
                batch->needs_sync = true;

        screen->vtbl.init_batch(batch);
}

/*
 * Safe helpers for manipulating batch->resources follow. In addition to
 * wrapping the underlying set operations, these update the required
 * bookkeeping for resource tracking and reference counting.
 */
static bool
panfrost_batch_uses_resource(struct panfrost_batch *batch,
                             struct panfrost_resource *rsrc)
{
        return _mesa_set_search(batch->resources, rsrc) != NULL;
}

static void
panfrost_batch_add_resource(struct panfrost_batch *batch,
                            struct panfrost_resource *rsrc)
{
        struct panfrost_context *ctx = batch->ctx;
        struct panfrost_device *dev = pan_device(ctx->base.screen);

        bool found = false;
        _mesa_set_search_or_add(batch->resources, rsrc, &found);

        /* Nothing to do if we already have the resource */
        if (found)
                return;

        /* Cache number of batches accessing a resource */
        rsrc->track.nr_users++;

        /* Reference the resource on the batch */
        pipe_reference(NULL, &rsrc->base.reference);

        if (rsrc->scanout) {
                if (dev->has_dmabuf_fence) {
                        int fd = rsrc->image.data.bo->dmabuf_fd;
                        util_dynarray_append(&batch->dmabufs, int, fd);
                } else {
                        perf_debug_ctx(ctx, "Forcing sync on batch");
                        batch->needs_sync = true;
                }
        }
}

static void
panfrost_batch_remove_resource_internal(struct panfrost_context *ctx,
                                        struct panfrost_resource *rsrc)
{
        struct hash_entry *writer = _mesa_hash_table_search(ctx->writers, rsrc);
        if (writer) {
                _mesa_hash_table_remove(ctx->writers, writer);
                rsrc->track.nr_writers--;
        }

        rsrc->track.nr_users--;
        pipe_resource_reference((struct pipe_resource **) &rsrc, NULL);
}

static void
panfrost_batch_remove_resource_if_present(struct panfrost_context *ctx,
                                          struct panfrost_batch *batch,
                                          struct panfrost_resource *rsrc)
{
        struct set_entry *ent = _mesa_set_search(batch->resources, rsrc);

        if (ent != NULL) {
                panfrost_batch_remove_resource_internal(ctx, rsrc);
                _mesa_set_remove(batch->resources, ent);
        }
}

static void
panfrost_batch_destroy_resources(struct panfrost_context *ctx,
                                 struct panfrost_batch *batch)
{
        set_foreach(batch->resources, entry) {
                struct panfrost_resource *rsrc = (void *) entry->key;

                panfrost_batch_remove_resource_internal(ctx, rsrc);
        }

        _mesa_set_destroy(batch->resources, NULL);
}

static void
panfrost_batch_cleanup(struct panfrost_context *ctx, struct panfrost_batch *batch)
{
        struct panfrost_device *dev = pan_device(ctx->base.screen);

        /* Make sure we keep handling events, to free old BOs */
        if (dev->kbase)
                kbase_ensure_handle_events(&dev->mali);

        assert(batch->seqnum);

        if (ctx->batch == batch)
                ctx->batch = NULL;

        unsigned batch_idx = panfrost_batch_idx(batch);

        pan_bo_access *flags = util_dynarray_begin(&batch->bos);
        unsigned end_bo = util_dynarray_num_elements(&batch->bos, pan_bo_access);

        for (int i = 0; i < end_bo; ++i) {
                if (!flags[i])
                        continue;

                struct panfrost_bo *bo = pan_lookup_bo_existing(dev, i);
                panfrost_bo_unreference(bo);
        }

        util_dynarray_fini(&batch->dmabufs);

        util_dynarray_fini(&batch->vert_deps);
        util_dynarray_fini(&batch->frag_deps);

        for (unsigned i = 0; i < PAN_USAGE_COUNT; ++i)
                util_dynarray_fini(&batch->resource_bos[i]);

        panfrost_batch_destroy_resources(ctx, batch);
        panfrost_pool_cleanup(&batch->pool);
        panfrost_pool_cleanup(&batch->invisible_pool);

        util_unreference_framebuffer_state(&batch->key);

        util_dynarray_fini(&batch->bos);

        memset(batch, 0, sizeof(*batch));
        BITSET_CLEAR(ctx->batches.active, batch_idx);
}

static void
panfrost_batch_submit(struct panfrost_context *ctx,
                      struct panfrost_batch *batch);

static struct panfrost_batch *
panfrost_get_batch(struct panfrost_context *ctx,
                   const struct pipe_framebuffer_state *key)
{
        struct panfrost_batch *batch = NULL;

        for (unsigned i = 0; i < PAN_MAX_BATCHES; i++) {
                if (ctx->batches.slots[i].seqnum &&
                    util_framebuffer_state_equal(&ctx->batches.slots[i].key, key)) {
                        /* We found a match, increase the seqnum for the LRU
                         * eviction logic.
                         */
                        ctx->batches.slots[i].seqnum = ++ctx->batches.seqnum;
                        return &ctx->batches.slots[i];
                }

                if (!batch || batch->seqnum > ctx->batches.slots[i].seqnum)
                        batch = &ctx->batches.slots[i];
        }

        assert(batch);

        /* The selected slot is used, we need to flush the batch */
        if (batch->seqnum)
                panfrost_batch_submit(ctx, batch);

        panfrost_batch_init(ctx, key, batch);

        unsigned batch_idx = panfrost_batch_idx(batch);
        BITSET_SET(ctx->batches.active, batch_idx);

        return batch;
}

/* Get the job corresponding to the FBO we're currently rendering into */

struct panfrost_batch *
panfrost_get_batch_for_fbo(struct panfrost_context *ctx)
{
        /* If we already began rendering, use that */

        if (ctx->batch) {
                assert(util_framebuffer_state_equal(&ctx->batch->key,
                                                    &ctx->pipe_framebuffer));
                return ctx->batch;
        }

        /* If not, look up the job */
        struct panfrost_batch *batch = panfrost_get_batch(ctx,
                                                          &ctx->pipe_framebuffer);

        /* Set this job as the current FBO job. Will be reset when updating the
         * FB state and when submitting or releasing a job.
         */
        ctx->batch = batch;
        panfrost_dirty_state_all(ctx);
        return batch;
}

struct panfrost_batch *
panfrost_get_fresh_batch_for_fbo(struct panfrost_context *ctx, const char *reason)
{
        struct panfrost_batch *batch;

        batch = panfrost_get_batch(ctx, &ctx->pipe_framebuffer);
        panfrost_dirty_state_all(ctx);

        /* We only need to submit and get a fresh batch if there is no
         * draw/clear queued. Otherwise we may reuse the batch. */

        if (batch->scoreboard.first_job) {
                perf_debug_ctx(ctx, "Flushing the current FBO due to: %s", reason);
                panfrost_batch_submit(ctx, batch);
                batch = panfrost_get_batch(ctx, &ctx->pipe_framebuffer);
        }

        ctx->batch = batch;
        return batch;
}

static void
panfrost_batch_update_access(struct panfrost_batch *batch,
                             struct panfrost_resource *rsrc, bool writes)
{
        struct panfrost_context *ctx = batch->ctx;
        uint32_t batch_idx = panfrost_batch_idx(batch);
        struct hash_entry *entry = _mesa_hash_table_search(ctx->writers, rsrc);
        struct panfrost_batch *writer = entry ? entry->data : NULL;

        panfrost_batch_add_resource(batch, rsrc);

        /* Flush users if required */
        if (writes || ((writer != NULL) && (writer != batch))) {
                unsigned i;
                foreach_batch(ctx, i) {
                        struct panfrost_batch *batch = &ctx->batches.slots[i];

                        /* Skip the entry if this our batch. */
                        if (i == batch_idx)
                                continue;

                        /* Submit if it's a user */
                        if (panfrost_batch_uses_resource(batch, rsrc))
                                panfrost_batch_submit(ctx, batch);
                }
        }

        if (writes && (writer != batch)) {
                _mesa_hash_table_insert(ctx->writers, rsrc, batch);
                rsrc->track.nr_writers++;
        }
}

static pan_bo_access *
panfrost_batch_get_bo_access(struct panfrost_batch *batch, unsigned handle)
{
        unsigned size = util_dynarray_num_elements(&batch->bos, pan_bo_access);

        if (handle >= size) {
                unsigned grow = handle + 1 - size;

                memset(util_dynarray_grow(&batch->bos, pan_bo_access, grow),
                       0, grow * sizeof(pan_bo_access));
        }

        return util_dynarray_element(&batch->bos, pan_bo_access, handle);
}

static void
panfrost_batch_add_bo_old(struct panfrost_batch *batch,
                struct panfrost_bo *bo, uint32_t flags)
{
        if (!bo)
                return;

        pan_bo_access *entry =
                panfrost_batch_get_bo_access(batch, bo->gem_handle);
        pan_bo_access old_flags = *entry;

        if (!old_flags) {
                batch->num_bos++;
                panfrost_bo_reference(bo);
        }

        if (old_flags == flags)
                return;

        flags |= old_flags;
        *entry = flags;
}

static uint32_t
panfrost_access_for_stage(enum pipe_shader_type stage)
{
        return (stage == PIPE_SHADER_FRAGMENT) ?
                PAN_BO_ACCESS_FRAGMENT : PAN_BO_ACCESS_VERTEX_TILER;
}

void
panfrost_batch_add_bo(struct panfrost_batch *batch,
                struct panfrost_bo *bo, enum pipe_shader_type stage)
{
        panfrost_batch_add_bo_old(batch, bo, PAN_BO_ACCESS_READ |
                        panfrost_access_for_stage(stage));
}

void
panfrost_batch_read_rsrc(struct panfrost_batch *batch,
                         struct panfrost_resource *rsrc,
                         enum pipe_shader_type stage)
{
        uint32_t access = PAN_BO_ACCESS_READ |
                panfrost_access_for_stage(stage);

        enum panfrost_usage_type type = (stage == MESA_SHADER_FRAGMENT) ?
                PAN_USAGE_READ_FRAGMENT : PAN_USAGE_READ_VERTEX;

        util_dynarray_append(&batch->resource_bos[type], struct panfrost_bo *,
                             rsrc->image.data.bo);

        panfrost_batch_add_bo_old(batch, rsrc->image.data.bo, access);

        if (rsrc->separate_stencil)
                panfrost_batch_add_bo_old(batch, rsrc->separate_stencil->image.data.bo, access);

        panfrost_batch_update_access(batch, rsrc, false);
}

void
panfrost_batch_write_rsrc(struct panfrost_batch *batch,
                         struct panfrost_resource *rsrc,
                         enum pipe_shader_type stage)
{
        uint32_t access = PAN_BO_ACCESS_WRITE |
                panfrost_access_for_stage(stage);

        enum panfrost_usage_type type = (stage == MESA_SHADER_FRAGMENT) ?
                PAN_USAGE_WRITE_FRAGMENT : PAN_USAGE_WRITE_VERTEX;

        util_dynarray_append(&batch->resource_bos[type], struct panfrost_bo *,
                             rsrc->image.data.bo);

        panfrost_batch_add_bo_old(batch, rsrc->image.data.bo, access);

        if (rsrc->separate_stencil)
                panfrost_batch_add_bo_old(batch, rsrc->separate_stencil->image.data.bo, access);

        panfrost_batch_update_access(batch, rsrc, true);
}

void
panfrost_resource_swap_bo(struct panfrost_context *ctx,
                          struct panfrost_resource *rsrc,
                          struct panfrost_bo *newbo)
{
        /* Likewise, any batch reading this resource is reading the old BO, and
         * after swapping will not be reading this resource.
         */
        unsigned i;
        foreach_batch(ctx, i) {
                struct panfrost_batch *batch = &ctx->batches.slots[i];

                panfrost_batch_remove_resource_if_present(ctx, batch, rsrc);
        }

        /* Swap the pointers, dropping a reference to the old BO which is no
         * long referenced from the resource
         */
        panfrost_bo_unreference(rsrc->image.data.bo);
        rsrc->image.data.bo = newbo;
}

struct panfrost_bo *
panfrost_batch_create_bo(struct panfrost_batch *batch, size_t size,
                         uint32_t create_flags, enum pipe_shader_type stage,
                         const char *label)
{
        struct panfrost_bo *bo;

        bo = panfrost_bo_create(pan_device(batch->ctx->base.screen), size,
                                create_flags, label);
        panfrost_batch_add_bo(batch, bo, stage);

        /* panfrost_batch_add_bo() has retained a reference and
         * panfrost_bo_create() initialize the refcnt to 1, so let's
         * unreference the BO here so it gets released when the batch is
         * destroyed (unless it's retained by someone else in the meantime).
         */
        panfrost_bo_unreference(bo);
        return bo;
}

struct panfrost_bo *
panfrost_batch_get_scratchpad(struct panfrost_batch *batch,
                unsigned size_per_thread,
                unsigned thread_tls_alloc,
                unsigned core_id_range)
{
        unsigned size = panfrost_get_total_stack_size(size_per_thread,
                        thread_tls_alloc,
                        core_id_range);

        if (batch->scratchpad) {
                assert(batch->scratchpad->size >= size);
        } else {
                batch->scratchpad = panfrost_batch_create_bo(batch, size,
                                             PAN_BO_INVISIBLE,
                                             PIPE_SHADER_VERTEX,
                                             "Thread local storage");

                panfrost_batch_add_bo(batch, batch->scratchpad,
                                PIPE_SHADER_FRAGMENT);
        }

        return batch->scratchpad;
}

struct panfrost_bo *
panfrost_batch_get_shared_memory(struct panfrost_batch *batch,
                unsigned size,
                unsigned workgroup_count)
{
        if (batch->shared_memory) {
                assert(batch->shared_memory->size >= size);
        } else {
                batch->shared_memory = panfrost_batch_create_bo(batch, size,
                                             PAN_BO_INVISIBLE,
                                             PIPE_SHADER_VERTEX,
                                             "Workgroup shared memory");
        }

        return batch->shared_memory;
}

static void
panfrost_batch_to_fb_info(struct panfrost_batch *batch,
                          struct pan_fb_info *fb,
                          struct pan_image_view *rts,
                          struct pan_image_view *zs,
                          struct pan_image_view *s,
                          bool reserve)
{
        memset(fb, 0, sizeof(*fb));
        memset(rts, 0, sizeof(*rts) * 8);
        memset(zs, 0, sizeof(*zs));
        memset(s, 0, sizeof(*s));

        fb->width = batch->key.width;
        fb->height = batch->key.height;
        fb->extent.minx = batch->minx;
        fb->extent.miny = batch->miny;
        fb->extent.maxx = batch->maxx - 1;
        fb->extent.maxy = batch->maxy - 1;
        fb->nr_samples = util_framebuffer_get_num_samples(&batch->key);
        fb->rt_count = batch->key.nr_cbufs;
        fb->sprite_coord_origin = pan_tristate_get(batch->sprite_coord_origin);
        fb->first_provoking_vertex = pan_tristate_get(batch->first_provoking_vertex);
        fb->cs_fragment = &batch->cs_fragment;

        static const unsigned char id_swz[] = {
                PIPE_SWIZZLE_X, PIPE_SWIZZLE_Y, PIPE_SWIZZLE_Z, PIPE_SWIZZLE_W,
        };

        for (unsigned i = 0; i < fb->rt_count; i++) {
                struct pipe_surface *surf = batch->key.cbufs[i];

                if (!surf)
                        continue;

                struct panfrost_resource *prsrc = pan_resource(surf->texture);
                unsigned mask = PIPE_CLEAR_COLOR0 << i;

                if (batch->clear & mask) {
                        fb->rts[i].clear = true;
                        memcpy(fb->rts[i].clear_value, batch->clear_color[i],
                               sizeof((fb->rts[i].clear_value)));
                }

                fb->rts[i].discard = !reserve && !(batch->resolve & mask);

                rts[i].format = surf->format;
                rts[i].dim = MALI_TEXTURE_DIMENSION_2D;
                rts[i].last_level = rts[i].first_level = surf->u.tex.level;
                rts[i].first_layer = surf->u.tex.first_layer;
                rts[i].last_layer = surf->u.tex.last_layer;
                rts[i].image = &prsrc->image;
                rts[i].nr_samples = surf->nr_samples ? : MAX2(surf->texture->nr_samples, 1);
                memcpy(rts[i].swizzle, id_swz, sizeof(rts[i].swizzle));
                fb->rts[i].crc_valid = &prsrc->valid.crc;
                fb->rts[i].view = &rts[i];

                /* Preload if the RT is read or updated */
                if (!(batch->clear & mask) &&
                    ((batch->read & mask) ||
                     ((batch->draws & mask) &&
                      BITSET_TEST(prsrc->valid.data, fb->rts[i].view->first_level))))
                        fb->rts[i].preload = true;

        }

        const struct pan_image_view *s_view = NULL, *z_view = NULL;
        struct panfrost_resource *z_rsrc = NULL, *s_rsrc = NULL;

        if (batch->key.zsbuf) {
                struct pipe_surface *surf = batch->key.zsbuf;
                z_rsrc = pan_resource(surf->texture);

                zs->format = surf->format == PIPE_FORMAT_Z32_FLOAT_S8X24_UINT ?
                             PIPE_FORMAT_Z32_FLOAT : surf->format;
                zs->dim = MALI_TEXTURE_DIMENSION_2D;
                zs->last_level = zs->first_level = surf->u.tex.level;
                zs->first_layer = surf->u.tex.first_layer;
                zs->last_layer = surf->u.tex.last_layer;
                zs->image = &z_rsrc->image;
                zs->nr_samples = surf->nr_samples ? : MAX2(surf->texture->nr_samples, 1);
                memcpy(zs->swizzle, id_swz, sizeof(zs->swizzle));
                fb->zs.view.zs = zs;
                z_view = zs;
                if (util_format_is_depth_and_stencil(zs->format)) {
                        s_view = zs;
                        s_rsrc = z_rsrc;
                }

                if (z_rsrc->separate_stencil) {
                        s_rsrc = z_rsrc->separate_stencil;
                        s->format = PIPE_FORMAT_S8_UINT;
                        s->dim = MALI_TEXTURE_DIMENSION_2D;
                        s->last_level = s->first_level = surf->u.tex.level;
                        s->first_layer = surf->u.tex.first_layer;
                        s->last_layer = surf->u.tex.last_layer;
                        s->image = &s_rsrc->image;
                        s->nr_samples = surf->nr_samples ? : MAX2(surf->texture->nr_samples, 1);
                        memcpy(s->swizzle, id_swz, sizeof(s->swizzle));
                        fb->zs.view.s = s;
                        s_view = s;
                }
        }

        if (batch->clear & PIPE_CLEAR_DEPTH) {
                fb->zs.clear.z = true;
                fb->zs.clear_value.depth = batch->clear_depth;
        }

        if (batch->clear & PIPE_CLEAR_STENCIL) {
                fb->zs.clear.s = true;
                fb->zs.clear_value.stencil = batch->clear_stencil;
        }

        fb->zs.discard.z = !reserve && !(batch->resolve & PIPE_CLEAR_DEPTH);
        fb->zs.discard.s = !reserve && !(batch->resolve & PIPE_CLEAR_STENCIL);

        if (!fb->zs.clear.z && z_rsrc &&
            ((batch->read & PIPE_CLEAR_DEPTH) ||
             ((batch->draws & PIPE_CLEAR_DEPTH) &&
              BITSET_TEST(z_rsrc->valid.data, z_view->first_level))))
                fb->zs.preload.z = true;

        if (!fb->zs.clear.s && s_rsrc &&
            ((batch->read & PIPE_CLEAR_STENCIL) ||
             ((batch->draws & PIPE_CLEAR_STENCIL) &&
              BITSET_TEST(s_rsrc->valid.data, s_view->first_level))))
                fb->zs.preload.s = true;

        /* Preserve both component if we have a combined ZS view and
         * one component needs to be preserved.
         */
        if (z_view && s_view == z_view && fb->zs.discard.z != fb->zs.discard.s) {
                bool valid = BITSET_TEST(z_rsrc->valid.data, z_view->first_level);

                fb->zs.discard.z = false;
                fb->zs.discard.s = false;
                fb->zs.preload.z = !fb->zs.clear.z && valid;
                fb->zs.preload.s = !fb->zs.clear.s && valid;
        }
}

static int
panfrost_batch_submit_kbase(struct panfrost_device *dev,
                            struct drm_panfrost_submit *submit,
                            struct kbase_syncobj *syncobj)
{
        dev->mali.handle_events(&dev->mali);

        int atom = dev->mali.submit(&dev->mali,
                                    submit->jc,
                                    submit->requirements,
                                    syncobj,
                                    (int32_t *)(uintptr_t) submit->bo_handles,
                                    submit->bo_handle_count);

        if (atom == -1) {
                errno = EINVAL;
                return -1;
        }

        return 0;
}

static int
panfrost_batch_submit_ioctl(struct panfrost_batch *batch,
                            mali_ptr first_job_desc,
                            uint32_t reqs,
                            uint32_t in_sync,
                            uint32_t out_sync)
{
        struct panfrost_context *ctx = batch->ctx;
        struct pipe_context *gallium = (struct pipe_context *) ctx;
        struct panfrost_device *dev = pan_device(gallium->screen);
        struct drm_panfrost_submit submit = {0,};
        uint32_t in_syncs[2];
        uint32_t *bo_handles;
        int ret;

        /* If we trace, we always need a syncobj, so make one of our own if we
         * weren't given one to use. Remember that we did so, so we can free it
         * after we're done but preventing double-frees if we were given a
         * syncobj */

        if (!out_sync && dev->debug & (PAN_DBG_TRACE | PAN_DBG_SYNC))
                out_sync = ctx->syncobj;

        submit.out_sync = out_sync;
        submit.jc = first_job_desc;
        submit.requirements = reqs;

        if (in_sync)
                in_syncs[submit.in_sync_count++] = in_sync;

        if (ctx->in_sync_fd >= 0) {
                ret = drmSyncobjImportSyncFile(dev->fd, ctx->in_sync_obj,
                                               ctx->in_sync_fd);
                assert(!ret);

                in_syncs[submit.in_sync_count++] = ctx->in_sync_obj;
                close(ctx->in_sync_fd);
                ctx->in_sync_fd = -1;
        }

        if (submit.in_sync_count)
                submit.in_syncs = (uintptr_t)in_syncs;

        bo_handles = calloc(panfrost_pool_num_bos(&batch->pool) +
                            panfrost_pool_num_bos(&batch->invisible_pool) +
                            batch->num_bos + 2,
                            sizeof(*bo_handles));
        assert(bo_handles);

        pan_bo_access *flags = util_dynarray_begin(&batch->bos);
        unsigned end_bo = util_dynarray_num_elements(&batch->bos, pan_bo_access);

        for (int i = 0; i < end_bo; ++i) {
                if (!flags[i])
                        continue;

                assert(submit.bo_handle_count < batch->num_bos);
                bo_handles[submit.bo_handle_count++] = i;

                /* Update the BO access flags so that panfrost_bo_wait() knows
                 * about all pending accesses.
                 * We only keep the READ/WRITE info since this is all the BO
                 * wait logic cares about.
                 * We also preserve existing flags as this batch might not
                 * be the first one to access the BO.
                 */
                struct panfrost_bo *bo = pan_lookup_bo_existing(dev, i);

                bo->gpu_access |= flags[i] & (PAN_BO_ACCESS_RW);
        }

        panfrost_pool_get_bo_handles(&batch->pool, bo_handles + submit.bo_handle_count);
        submit.bo_handle_count += panfrost_pool_num_bos(&batch->pool);
        panfrost_pool_get_bo_handles(&batch->invisible_pool, bo_handles + submit.bo_handle_count);
        submit.bo_handle_count += panfrost_pool_num_bos(&batch->invisible_pool);

        /* Add the tiler heap to the list of accessed BOs if the batch has at
         * least one tiler job. Tiler heap is written by tiler jobs and read
         * by fragment jobs (the polygon list is coming from this heap).
         */
        if (batch->scoreboard.first_tiler)
                bo_handles[submit.bo_handle_count++] = dev->tiler_heap->gem_handle;

        /* Always used on Bifrost, occassionally used on Midgard */
        bo_handles[submit.bo_handle_count++] = dev->sample_positions->gem_handle;

        submit.bo_handles = (u64) (uintptr_t) bo_handles;
        if (ctx->is_noop)
                ret = 0;
        else if (dev->kbase)
                ret = panfrost_batch_submit_kbase(dev, &submit, ctx->syncobj_kbase);
        else
                ret = drmIoctl(dev->fd, DRM_IOCTL_PANFROST_SUBMIT, &submit);
        free(bo_handles);

        if (ret)
                return errno;

        /* Trace the job if we're doing that */
        if (dev->debug & (PAN_DBG_TRACE | PAN_DBG_SYNC)) {
                /* Wait so we can get errors reported back */
                if (dev->kbase)
                        dev->mali.syncobj_wait(&dev->mali, ctx->syncobj_kbase);
                else
                        drmSyncobjWait(dev->fd, &out_sync, 1,
                                       INT64_MAX, 0, NULL);

                if (dev->debug & PAN_DBG_TRACE)
                        pandecode_jc(submit.jc, dev->gpu_id);

                if (dev->debug & PAN_DBG_DUMP)
                        pandecode_dump_mappings();

                /* Jobs won't be complete if blackhole rendering, that's ok */
                if (!ctx->is_noop && dev->debug & PAN_DBG_SYNC)
                        pandecode_abort_on_fault(submit.jc, dev->gpu_id);
        }

        return 0;
}

static bool
panfrost_has_fragment_job(struct panfrost_batch *batch)
{
        return batch->scoreboard.first_tiler || batch->clear;
}

/* Submit both vertex/tiler and fragment jobs for a batch, possibly with an
 * outsync corresponding to the later of the two (since there will be an
 * implicit dep between them) */

static int
panfrost_batch_submit_jobs(struct panfrost_batch *batch,
                           const struct pan_fb_info *fb,
                           uint32_t in_sync, uint32_t out_sync)
{
        struct pipe_screen *pscreen = batch->ctx->base.screen;
        struct panfrost_screen *screen = pan_screen(pscreen);
        struct panfrost_device *dev = pan_device(pscreen);
        bool has_draws = batch->scoreboard.first_job;
        bool has_tiler = batch->scoreboard.first_tiler;
        bool has_frag = panfrost_has_fragment_job(batch);
        int ret = 0;

        /* Take the submit lock to make sure no tiler jobs from other context
         * are inserted between our tiler and fragment jobs, failing to do that
         * might result in tiler heap corruption.
         */
        if (has_tiler)
                pthread_mutex_lock(&dev->submit_lock);

        if (has_draws) {
                ret = panfrost_batch_submit_ioctl(batch, batch->scoreboard.first_job,
                                                  0, in_sync, has_frag ? 0 : out_sync);

                if (ret)
                        goto done;
        }

        if (has_frag) {
                mali_ptr fragjob = screen->vtbl.emit_fragment_job(batch, fb);
                ret = panfrost_batch_submit_ioctl(batch, fragjob,
                                                  PANFROST_JD_REQ_FS, 0,
                                                  out_sync);
                if (ret)
                        goto done;
        }

done:
        if (has_tiler)
                pthread_mutex_unlock(&dev->submit_lock);

        return ret;
}

#define BASE_MEM_MMU_DUMP_HANDLE (1 << 12)

static void
mmu_dump(struct panfrost_device *dev)
{
        unsigned size = 16 * 1024 * 1024;

        fprintf(stderr, "dumping MMU tables\n");
        sleep(3);

        void *mem = mmap(NULL, size, PROT_READ, MAP_SHARED,
                         dev->mali.fd, BASE_MEM_MMU_DUMP_HANDLE);
        if (mem == MAP_FAILED) {
                perror("mmap(BASE_MEM_MMU_DUMP_HANDLE)");
                return;;
        }

        fprintf(stderr, "writing to file\n");
        sleep(1);

        char template[] = {"/tmp/mmu-dump.XXXXXX"};
        int fd = mkstemp(template);
        if (fd == -1) {
                perror("mkstemp(/tmp/mmu-dump.XXXXXX)");
                goto unmap;
        }

        write(fd, mem, size);
        close(fd);

unmap:
        munmap(mem, size);
}

static void
reset_context(struct panfrost_context *ctx)
{
        struct pipe_screen *pscreen = ctx->base.screen;
        struct panfrost_screen *screen = pan_screen(pscreen);
        struct panfrost_device *dev = pan_device(pscreen);

        /* Don't recover from the fault if PAN_MESA_DEBUG=sync is specified,
         * to somewhat mimic behaviour with JM GPUs. TODO: Just abort? */
        bool recover = !(dev->debug & PAN_DBG_SYNC);

        mesa_loge("Context reset");

        dev->mali.cs_term(&dev->mali, &ctx->kbase_cs_vertex.base);
        dev->mali.cs_term(&dev->mali, &ctx->kbase_cs_fragment.base);

        dev->mali.context_recreate(&dev->mali, ctx->kbase_ctx);

        //mmu_dump(dev);

        if (recover) {
                dev->mali.cs_rebind(&dev->mali, &ctx->kbase_cs_vertex.base);
                dev->mali.cs_rebind(&dev->mali, &ctx->kbase_cs_fragment.base);
        } else {
                ctx->kbase_cs_vertex.base.user_io = NULL;
                ctx->kbase_cs_fragment.base.user_io = NULL;
        }

        ctx->kbase_cs_vertex.base.last_insert = 0;
        ctx->kbase_cs_fragment.base.last_insert = 0;

        screen->vtbl.init_cs(ctx, &ctx->kbase_cs_vertex);
        screen->vtbl.init_cs(ctx, &ctx->kbase_cs_fragment);

        /* TODO: this leaks memory */
        ctx->tiler_heap_desc = 0;
}

static void
pandecode_cs_ring(struct panfrost_device *dev, struct panfrost_cs *cs,
                  uint64_t insert)
{
        insert %= cs->base.size;
        uint64_t start = cs->base.last_insert % cs->base.size;

        if (insert < start) {
                pandecode_cs(cs->base.va + start, cs->base.size - start, dev->gpu_id);
                start = 0;
        }

        pandecode_cs(cs->base.va + start, insert - start, dev->gpu_id);
}

static unsigned
panfrost_add_dep_after(struct util_dynarray *deps,
                       struct panfrost_usage u,
                       unsigned index)
{
        unsigned size = util_dynarray_num_elements(deps, struct panfrost_usage);

        for (unsigned i = index; i < size; ++i) {
                struct panfrost_usage *d =
                        util_dynarray_element(deps, struct panfrost_usage, i);

                /* TODO: Remove d if it is an invalid entry? */

                if ((d->queue == u.queue) && (d->write == u.write)) {
                        d->seqnum = MAX2(d->seqnum, u.seqnum);
                        return i;

                } else if (d->queue > u.queue) {
                        void *p = util_dynarray_grow(deps, struct panfrost_usage, 1);
                        assert(p);
                        memmove(util_dynarray_element(deps, struct panfrost_usage, i + 1),
                                util_dynarray_element(deps, struct panfrost_usage, i),
                                (size - i) * sizeof(struct panfrost_usage));

                        *util_dynarray_element(deps, struct panfrost_usage, i) = u;
                        return i;
                }
        }

        util_dynarray_append(deps, struct panfrost_usage, u);
        return size;
}

static void
panfrost_update_deps(struct util_dynarray *deps, struct panfrost_bo *bo, bool write)
{
        /* Both lists should be sorted, so each dependency is at a higher
         * index than the last */
        unsigned index = 0;
        util_dynarray_foreach(&bo->usage, struct panfrost_usage, u) {
                /* read->read access does not require a dependency */
                if (!write && !u->write)
                        continue;

                index = panfrost_add_dep_after(deps, *u, index);
        }
}

static inline bool
panfrost_usage_writes(enum panfrost_usage_type usage)
{
        return (usage == PAN_USAGE_WRITE_VERTEX) || (usage == PAN_USAGE_WRITE_FRAGMENT);
}

static inline bool
panfrost_usage_fragment(enum panfrost_usage_type usage)
{
        return (usage == PAN_USAGE_READ_FRAGMENT) || (usage == PAN_USAGE_WRITE_FRAGMENT);
}

/* Removes invalid dependencies from deps */
static void
panfrost_clean_deps(struct panfrost_device *dev, struct util_dynarray *deps)
{
        kbase k = &dev->mali;

        struct panfrost_usage *rebuild = util_dynarray_begin(deps);
        unsigned index = 0;

        util_dynarray_foreach(deps, struct panfrost_usage, u) {
                /* Usages are ordered, so we can break here */
                if (u->queue >= k->event_slot_usage)
                        break;

                struct kbase_event_slot *slot = &k->event_slots[u->queue];
                uint64_t seqnum = u->seqnum;

                /* There is a race condition, where we can depend on an
                 * unsubmitted batch. In that cade, decrease the seqnum.
                 * Otherwise, skip invalid dependencies. */
                if (slot->last_submit == seqnum)
                        --seqnum;
                else if (slot->last_submit < seqnum)
                        continue;

                /* This usage is valid, add it to the returned list */
                rebuild[index++] = (struct panfrost_usage) {
                        .queue = u->queue,
                        .write = u->write,
                        .seqnum = seqnum,
                };
        }

        /* No need to check the return value, it can only shrink */
        (void)! util_dynarray_resize(deps, struct panfrost_usage, index);
}

static int
panfrost_batch_submit_csf(struct panfrost_batch *batch,
                          const struct pan_fb_info *fb)
{
        struct panfrost_context *ctx = batch->ctx;
        struct pipe_screen *pscreen = ctx->base.screen;
        struct panfrost_screen *screen = pan_screen(pscreen);
        struct panfrost_device *dev = pan_device(pscreen);

        ++ctx->kbase_cs_vertex.seqnum;

        if (panfrost_has_fragment_job(batch)) {
                screen->vtbl.emit_fragment_job(batch, fb);
                ++ctx->kbase_cs_fragment.seqnum;
        }

        pthread_mutex_lock(&dev->bo_usage_lock);
        for (unsigned i = 0; i < PAN_USAGE_COUNT; ++i) {

                bool write = panfrost_usage_writes(i);
                pan_bo_access access = write ? PAN_BO_ACCESS_RW : PAN_BO_ACCESS_READ;
                struct util_dynarray *deps;
                unsigned queue;
                uint64_t seqnum;

                if (panfrost_usage_fragment(i)) {
                        deps = &batch->frag_deps;
                        queue = ctx->kbase_cs_fragment.base.event_mem_offset;
                        seqnum = ctx->kbase_cs_fragment.seqnum;
                } else {
                        deps = &batch->vert_deps;
                        queue = ctx->kbase_cs_vertex.base.event_mem_offset;
                        seqnum = ctx->kbase_cs_vertex.seqnum;
                }

                util_dynarray_foreach(&batch->resource_bos[i], struct panfrost_bo *, bo) {
                        panfrost_update_deps(deps, *bo, write);
                        struct panfrost_usage u = {
                                .queue = queue,
                                .write = write,
                                .seqnum = seqnum,
                        };

                        panfrost_add_dep_after(&(*bo)->usage, u, 0);
                        (*bo)->gpu_access |= access;
                }
        }
        pthread_mutex_unlock(&dev->bo_usage_lock);

        /* For now, only a single batch can use each tiler heap at once */
        if (ctx->tiler_heap_desc) {
                panfrost_update_deps(&batch->vert_deps, ctx->tiler_heap_desc, true);

                struct panfrost_usage u = {
                        .queue = ctx->kbase_cs_fragment.base.event_mem_offset,
                        .write = true,
                        .seqnum = ctx->kbase_cs_fragment.seqnum,
                };
                panfrost_add_dep_after(&ctx->tiler_heap_desc->usage, u, 0);
        }

        /* TODO: Use atomics in kbase code to avoid lock? */
        pthread_mutex_lock(&dev->mali.queue_lock);

        panfrost_clean_deps(dev, &batch->vert_deps);
        panfrost_clean_deps(dev, &batch->frag_deps);

        pthread_mutex_unlock(&dev->mali.queue_lock);

        screen->vtbl.emit_csf_toplevel(batch);

        uint64_t vs_offset = ctx->kbase_cs_vertex.offset +
                (void *)ctx->kbase_cs_vertex.cs.ptr - ctx->kbase_cs_vertex.bo->ptr.cpu;
        uint64_t fs_offset = ctx->kbase_cs_fragment.offset +
                (void *)ctx->kbase_cs_fragment.cs.ptr - ctx->kbase_cs_fragment.bo->ptr.cpu;

        if (dev->debug & PAN_DBG_TRACE) {
                pandecode_cs_ring(dev, &ctx->kbase_cs_vertex, vs_offset);
                pandecode_cs_ring(dev, &ctx->kbase_cs_fragment, fs_offset);
        }

        bool log = (dev->debug & PAN_DBG_LOG);

        // TODO: We need better synchronisation than a single fake syncobj!

        if (log)
                printf("About to submit\n");

        dev->mali.cs_submit(&dev->mali, &ctx->kbase_cs_vertex.base, vs_offset,
                            ctx->syncobj_kbase, ctx->kbase_cs_vertex.seqnum);

        dev->mali.cs_submit(&dev->mali, &ctx->kbase_cs_fragment.base, fs_offset,
                            ctx->syncobj_kbase, ctx->kbase_cs_fragment.seqnum);

        bool reset = false;

        // TODO: How will we know to reset a CS when waiting is not done?
        if (batch->needs_sync) {
                if (!dev->mali.cs_wait(&dev->mali, &ctx->kbase_cs_vertex.base, vs_offset, ctx->syncobj_kbase))
                        reset = true;

                if (!dev->mali.cs_wait(&dev->mali, &ctx->kbase_cs_fragment.base, fs_offset, ctx->syncobj_kbase))
                        reset = true;
        }

        if (dev->debug & PAN_DBG_TILER) {
                fflush(stdout);
                FILE *stream = popen("tiler-hex-read", "w");

                /* TODO: Dump more than just the first chunk */
                unsigned size = batch->ctx->kbase_ctx->tiler_heap_chunk_size;
                uint64_t va = batch->ctx->kbase_ctx->tiler_heap_header;

                fprintf(stream, "width %i\n" "height %i\n" "mask %i\n"
                        "vaheap 0x%"PRIx64"\n" "size %i\n",
                        batch->key.width, batch->key.height, 0xfe, va, size);

                void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE,
                                 MAP_SHARED, dev->mali.fd, va);

                pan_hexdump(stream, ptr, size, false);
                //memset(ptr, 0, size);
                munmap(ptr, size);

                pclose(stream);
        }

        if (reset)
                reset_context(ctx);

        return 0;
}

static void
panfrost_emit_tile_map(struct panfrost_batch *batch, struct pan_fb_info *fb)
{
        if (batch->key.nr_cbufs < 1 || !batch->key.cbufs[0])
                return;

        struct pipe_surface *surf = batch->key.cbufs[0];
        struct panfrost_resource *pres = surf ? pan_resource(surf->texture) : NULL;

        if (pres && pres->damage.tile_map.enable) {
                fb->tile_map.base =
                        pan_pool_upload_aligned(&batch->pool.base,
                                                pres->damage.tile_map.data,
                                                pres->damage.tile_map.size,
                                                64);
                fb->tile_map.stride = pres->damage.tile_map.stride;
        }
}

static void
panfrost_batch_submit(struct panfrost_context *ctx,
                      struct panfrost_batch *batch)
{
        struct pipe_screen *pscreen = ctx->base.screen;
        struct panfrost_screen *screen = pan_screen(pscreen);
        struct panfrost_device *dev = pan_device(pscreen);
        int ret;

        /* Nothing to do! */
        if (!batch->scoreboard.first_job && !batch->clear)
                goto out;

        if (batch->key.zsbuf && panfrost_has_fragment_job(batch)) {
                struct pipe_surface *surf = batch->key.zsbuf;
                struct panfrost_resource *z_rsrc = pan_resource(surf->texture);

                /* Shared depth/stencil resources are not supported, and would
                 * break this optimisation. */
                assert(!(z_rsrc->base.bind & PAN_BIND_SHARED_MASK));

                if (batch->clear & PIPE_CLEAR_STENCIL) {
                        z_rsrc->stencil_value = batch->clear_stencil;
                        z_rsrc->constant_stencil = true;
                } else if (z_rsrc->constant_stencil) {
                        batch->clear_stencil = z_rsrc->stencil_value;
                        batch->clear |= PIPE_CLEAR_STENCIL;
                }

                if (batch->draws & PIPE_CLEAR_STENCIL)
                        z_rsrc->constant_stencil = false;
        }

        struct pan_fb_info fb;
        struct pan_image_view rts[8], zs, s;

        panfrost_batch_to_fb_info(batch, &fb, rts, &zs, &s, false);

        screen->vtbl.preload(batch, &fb);
        screen->vtbl.init_polygon_list(batch);

        /* Now that all draws are in, we can finally prepare the
         * FBD for the batch (if there is one). */

        screen->vtbl.emit_tls(batch);
        panfrost_emit_tile_map(batch, &fb);

        if (batch->scoreboard.first_tiler || batch->clear)
                screen->vtbl.emit_fbd(batch, &fb);

        /* TODO: Don't hardcode the arch number */
        if (dev->arch < 10)
                ret = panfrost_batch_submit_jobs(batch, &fb, 0, ctx->syncobj);
        else
                ret = panfrost_batch_submit_csf(batch, &fb);

        if (ret)
                fprintf(stderr, "panfrost_batch_submit failed: %d\n", ret);

        /* We must reset the damage info of our render targets here even
         * though a damage reset normally happens when the DRI layer swaps
         * buffers. That's because there can be implicit flushes the GL
         * app is not aware of, and those might impact the damage region: if
         * part of the damaged portion is drawn during those implicit flushes,
         * you have to reload those areas before next draws are pushed, and
         * since the driver can't easily know what's been modified by the draws
         * it flushed, the easiest solution is to reload everything.
         */
        for (unsigned i = 0; i < batch->key.nr_cbufs; i++) {
                if (!batch->key.cbufs[i])
                        continue;

                panfrost_resource_set_damage_region(ctx->base.screen,
                                                    batch->key.cbufs[i]->texture,
                                                    0, NULL);
        }

out:
        panfrost_batch_cleanup(ctx, batch);
}

/* Submit all batches */

void
panfrost_flush_all_batches(struct panfrost_context *ctx, const char *reason)
{
        struct panfrost_batch *batch = panfrost_get_batch_for_fbo(ctx);
        panfrost_batch_submit(ctx, batch);

        for (unsigned i = 0; i < PAN_MAX_BATCHES; i++) {
                if (ctx->batches.slots[i].seqnum) {
                        if (reason)
                                perf_debug_ctx(ctx, "Flushing everything due to: %s", reason);

                        panfrost_batch_submit(ctx, &ctx->batches.slots[i]);
                }
        }
}

void
panfrost_flush_writer(struct panfrost_context *ctx,
                      struct panfrost_resource *rsrc,
                      const char *reason)
{
        struct hash_entry *entry = _mesa_hash_table_search(ctx->writers, rsrc);

        if (entry) {
                perf_debug_ctx(ctx, "Flushing writer due to: %s", reason);
                panfrost_batch_submit(ctx, entry->data);
        }
}

void
panfrost_flush_batches_accessing_rsrc(struct panfrost_context *ctx,
                                      struct panfrost_resource *rsrc,
                                      const char *reason)
{
        unsigned i;
        foreach_batch(ctx, i) {
                struct panfrost_batch *batch = &ctx->batches.slots[i];

                if (!panfrost_batch_uses_resource(batch, rsrc))
                        continue;

                perf_debug_ctx(ctx, "Flushing user due to: %s", reason);
                panfrost_batch_submit(ctx, batch);
        }
}

void
panfrost_batch_adjust_stack_size(struct panfrost_batch *batch)
{
        struct panfrost_context *ctx = batch->ctx;

        for (unsigned i = 0; i < PIPE_SHADER_TYPES; ++i) {
                struct panfrost_compiled_shader *ss = ctx->prog[i];

                if (!ss)
                        continue;

                batch->stack_size = MAX2(batch->stack_size, ss->info.tls_size);
        }
}

void
panfrost_batch_clear(struct panfrost_batch *batch,
                     unsigned buffers,
                     const union pipe_color_union *color,
                     double depth, unsigned stencil)
{
        struct panfrost_context *ctx = batch->ctx;

        if (buffers & PIPE_CLEAR_COLOR) {
                for (unsigned i = 0; i < ctx->pipe_framebuffer.nr_cbufs; ++i) {
                        if (!(buffers & (PIPE_CLEAR_COLOR0 << i)))
                                continue;
                        if (!ctx->pipe_framebuffer.cbufs[i])
                                continue;

                        enum pipe_format format = ctx->pipe_framebuffer.cbufs[i]->format;
                        pan_pack_color(batch->clear_color[i], color, format, false);
                }
        }

        if (buffers & PIPE_CLEAR_DEPTH) {
                batch->clear_depth = depth;
        }

        if (buffers & PIPE_CLEAR_STENCIL) {
                batch->clear_stencil = stencil;
        }

        batch->clear |= buffers;
        batch->resolve |= buffers;

        /* Clearing affects the entire framebuffer (by definition -- this is
         * the Gallium clear callback, which clears the whole framebuffer. If
         * the scissor test were enabled from the GL side, the gallium frontend
         * would emit a quad instead and we wouldn't go down this code path) */

        panfrost_batch_union_scissor(batch, 0, 0,
                                     ctx->pipe_framebuffer.width,
                                     ctx->pipe_framebuffer.height);
}

/* Given a new bounding rectangle (scissor), let the job cover the union of the
 * new and old bounding rectangles */

void
panfrost_batch_union_scissor(struct panfrost_batch *batch,
                             unsigned minx, unsigned miny,
                             unsigned maxx, unsigned maxy)
{
        batch->minx = MIN2(batch->minx, minx);
        batch->miny = MIN2(batch->miny, miny);
        batch->maxx = MAX2(batch->maxx, maxx);
        batch->maxy = MAX2(batch->maxy, maxy);
}

/**
 * Checks if rasterization should be skipped. If not, a TILER job must be
 * created for each draw, or the IDVS flow must be used.
 *
 * As a special case, if there is no vertex shader, no primitives are generated,
 * meaning the whole pipeline (including rasterization) should be skipped.
 */
bool
panfrost_batch_skip_rasterization(struct panfrost_batch *batch)
{
        struct panfrost_context *ctx = batch->ctx;
        struct pipe_rasterizer_state *rast = (void *) ctx->rasterizer;

        return (rast->rasterizer_discard ||
                batch->scissor_culls_everything ||
                !batch->rsd[PIPE_SHADER_VERTEX]);
}
