/*
 * Copyright © 2022 Icecream95
 * Copyright © 2011 Kristian Høgsberg
 * Copyright © 2011 Benjamin Franzke
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
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Kristian Høgsberg <krh@bitplanet.net>
 *    Benjamin Franzke <benjaminfranzke@googlemail.com>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <unistd.h>

#include <wayland-server.h>
#include "mali-buffer-sharing.h"
#include "mali-buffer-sharing-server-protocol.h"
#include "wayland-drm-client-protocol.h"

#define MIN(x,y) (((x)<(y))?(x):(y))

static void
destroy_buffer(struct wl_resource *resource)
{
        struct wl_drm_buffer *buffer = wl_resource_get_user_data(resource);
        struct wl_drm *drm = buffer->drm;

        drm->callbacks.release_buffer(drm->user_data, buffer);
        free(buffer);
}

static void
buffer_destroy(struct wl_client *client, struct wl_resource *resource)
{
        wl_resource_destroy(resource);
}

static void
create_buffer(struct wl_client *client, struct wl_resource *resource,
              uint32_t id, uint32_t name, int fd,
              int32_t width, int32_t height,
              uint32_t format,
              int32_t offset, int32_t stride)
{
        struct wl_drm *drm = wl_resource_get_user_data(resource);
        struct wl_drm_buffer *buffer;

        buffer = calloc(1, sizeof *buffer);
        if (buffer == NULL) {
                wl_resource_post_no_memory(resource);
                return;
        }

        buffer->drm = drm;
        buffer->width = width;
        buffer->height = height;
        buffer->format = format;
        buffer->offset[0] = offset;
        buffer->stride[0] = stride;

        drm->callbacks.reference_buffer(drm->user_data, name, fd, buffer);
        if (buffer->driver_buffer == NULL) {
                // TODO: We should return an error
                return;
        }

        buffer->resource =
                wl_resource_create(client, &wl_buffer_interface, 1, id);
        if (!buffer->resource) {
                wl_resource_post_no_memory(resource);
                free(buffer);
                return;
        }

        wl_resource_set_implementation(buffer->resource,
                                       (void (**)(void)) &drm->buffer_interface,
                                       buffer, destroy_buffer);
}

static void
mali_create_buffer(struct wl_client *client,
                   struct wl_resource *resource,
                   uint32_t id,
                   int32_t width, int32_t height, uint32_t stride,
                   enum wl_drm_format format, uint32_t unk1, uint32_t unk2,
                   int fd)
{
        create_buffer(client, resource, id, 0, fd, width, height, format,
                      0, stride);
        close(fd);
}

static void
mali_auth(struct wl_client *client,
          struct wl_resource *resource, uint32_t id)
{
        struct wl_drm *drm = wl_resource_get_user_data(resource);

        drm->callbacks.authenticate(drm->user_data, id);
}

static const struct mali_buffer_sharing_interface mali_interface = {
        mali_create_buffer,
        mali_auth,
};

static void
bind_mali(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
        struct wl_drm *drm = data;
        struct wl_resource *resource;

        resource = wl_resource_create(client, &mali_buffer_sharing_interface,
                                      MIN(version, 4), id);
        if (!resource) {
                wl_client_post_no_memory(client);
                return;
        }

        wl_resource_set_implementation(resource, &mali_interface, data, NULL);

        mali_buffer_sharing_send_alloc_device(resource, drm->device_name);
}

struct wl_drm *
mali_buffer_sharing_init(struct wl_display *display, char *device_name,
                 const struct wayland_drm_callbacks *callbacks, void *user_data)
{
        struct wl_drm *drm;

        drm = malloc(sizeof *drm);
        if (!drm)
                return NULL;

        drm->display = display;
        drm->device_name = strdup(device_name ?: "");
        drm->callbacks = *callbacks;
        drm->user_data = user_data;
        drm->flags = 1;

        drm->buffer_interface.destroy = buffer_destroy;

        drm->wl_drm_global =
                wl_global_create(display, &mali_buffer_sharing_interface, 5,
                                 drm, bind_mali);

        return drm;
}
