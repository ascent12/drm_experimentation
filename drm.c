#include "drm.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <drm.h>
#include <drm_mode.h>
#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "util.h"

struct dev *open_drm(const char *path)
{
	int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		fatal_errno("Failed to open \"%s\"", path);

	if (drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1))
		fatal("DRM device must support atomic modesetting");

	struct dev *dev = xalloc(sizeof *dev);

	dev->fd = fd;
	dev->gbm = gbm_create_device(fd);
	if (!dev->gbm)
		fatal_errno("Failed to create GBM device");

	return dev;
}

void close_drm(struct dev *dev)
{
	if (!dev)
		return;

	struct conn *tmp;
	for (struct conn *conn = dev->conns; conn; conn = tmp) {
		tmp = conn->next;
		drmModeDestroyPropertyBlob(dev->fd, conn->mode_id);
		drmModeAtomicFree(conn->atomic);
		drmModeFreeCrtc(conn->old_crtc);
		free(conn);
	}

	gbm_device_destroy(dev->gbm);
	close(dev->fd);
	free(dev);
}

struct prop {
	const char *name;
	uint32_t *dest;
	uint64_t *value;
};

static int prop_cmp(const void *arg1, const void *arg2)
{
	const char *key = arg1;
	const struct prop *val = arg2;

	return strcmp(key, val->name);
}

void new_connector(struct dev *dev, uint32_t conn_id, uint32_t crtc_id, uint32_t primary_id)
{
	struct conn *conn = xalloc(sizeof *conn);

	conn->dev = dev;
	conn->conn_id = conn_id;
	conn->crtc_id = crtc_id;
	conn->primary_id = primary_id;

	conn->next = dev->conns;
	dev->conns = conn;

	drmModeConnector *drm_conn = drmModeGetConnector(dev->fd, conn_id);
	if (!drm_conn)
		fatal_errno("Failed to get conn-id %"PRIu32, conn_id);

	if (drm_conn->connection != DRM_MODE_CONNECTED || drm_conn->count_modes == 0)
		fatal("conn-id %"PRIu32" not connected", conn_id);

	if (drmModeCreatePropertyBlob(dev->fd, &drm_conn->modes[0],
			sizeof drm_conn->modes[0], &conn->mode_id))
		fatal_errno("Failed to create DRM property blob");

	drmModeFreeConnector(drm_conn);

	conn->atomic = drmModeAtomicAlloc();
	if (!conn->atomic)
		fatal_errno("Failed to allocate atomic request");

	uint64_t curr_crtc_id = 0;

	struct prop conn_props[] = {
		{ "CRTC_ID", &conn->conn_props.crtc_id, &curr_crtc_id },
	};

	struct prop crtc_props[] = {
		{ "MODE_ID", &conn->crtc_props.mode_id, NULL },
		{ "OUT_FENCE_PTR", &conn->crtc_props.out_fence_ptr, NULL },
	};

	struct prop plane_props[] = {
		{ "CRTC_H", &conn->plane_props.crtc_h, NULL },
		{ "CRTC_ID", &conn->plane_props.crtc_id, NULL },
		{ "CRTC_W", &conn->plane_props.crtc_w, NULL },
		{ "CRTC_X", &conn->plane_props.crtc_x, NULL },
		{ "CRTC_Y", &conn->plane_props.crtc_y, NULL },
		{ "FB_ID", &conn->plane_props.fb_id, NULL },
		{ "IN_FENCE_FD", &conn->plane_props.in_fence_fd, NULL },
		{ "SRC_H", &conn->plane_props.src_h, NULL },
		{ "SRC_W", &conn->plane_props.src_w, NULL },
		{ "SRC_X", &conn->plane_props.src_x, NULL },
		{ "SRC_Y", &conn->plane_props.src_y, NULL },
	};

	struct {
		uint32_t id;
		uint32_t type;
		struct prop *props;
		size_t props_len;
	} props[] = {
		{ conn_id, DRM_MODE_OBJECT_CONNECTOR, conn_props, sizeof conn_props / sizeof conn_props[0] },
		{ crtc_id, DRM_MODE_OBJECT_CRTC, crtc_props, sizeof crtc_props / sizeof crtc_props[0] },
		{ primary_id, DRM_MODE_OBJECT_PLANE, plane_props, sizeof plane_props / sizeof plane_props[0] },
	};

	for (size_t i = 0; i < sizeof props / sizeof props[0]; ++i) {
		drmModeObjectProperties *obj_props =
			drmModeObjectGetProperties(dev->fd, props[i].id, props[i].type);
		if (!obj_props)
			fatal_errno("Failed to get object properties");

		size_t seen = 0;

		for (uint32_t j = 0; j < obj_props->count_props; ++j) {
			drmModePropertyRes *prop = drmModeGetProperty(dev->fd, obj_props->props[j]);
			if (!prop)
				fatal_errno("Failed to get property");

			struct prop *p = bsearch(prop->name, props[i].props, props[i].props_len,
				sizeof *props[i].props, prop_cmp);

			if (p) {
				++seen;
				*p->dest = prop->prop_id;
				if (p->value)
					*p->value = obj_props->prop_values[j];
			}

			drmModeFreeProperty(prop);
		}

		if (seen != props[i].props_len)
			fatal("Could not find all required DRM properties");

		drmModeFreeObjectProperties(obj_props);
	}

	conn->old_crtc = drmModeGetCrtc(dev->fd, curr_crtc_id);
}
