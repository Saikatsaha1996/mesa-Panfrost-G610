#ifndef MALI_BUFFER_H
#define MALI_BUFFER_H

#include <wayland-server.h>

#include "wayland-drm.h"

struct wl_drm *
mali_buffer_sharing_init(struct wl_display *display, char *device_name,
		 const struct wayland_drm_callbacks *callbacks, void *user_data);

#endif
