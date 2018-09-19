#include "drm.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <drm.h>
#include <drm_mode.h>
#include <drm_fourcc.h>
#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "util.h"

PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC glEGLImageTargetRenderbufferStorageOES;
PFNEGLDUPNATIVEFENCEFDANDROIDPROC eglDupNativeFenceFDANDROID;

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

	dev->egl = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, dev->gbm, NULL);
	if (dev->egl == EGL_NO_DISPLAY)
		fatal("Failed to create EGL display");

	if (!eglInitialize(dev->egl, NULL, NULL))
		fatal("Failed to initialize EGL display");

	glEGLImageTargetRenderbufferStorageOES = (PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC)
		eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES");
	eglDupNativeFenceFDANDROID = (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)
		eglGetProcAddress("eglDupNativeFenceFDANDROID");

	EGLint config_attribs[] = {
		EGL_RED_SIZE, 8,
		EGL_BLUE_SIZE, 8,
		EGL_GREEN_SIZE, 8,
		EGL_ALPHA_SIZE, 0,
		EGL_CONFORMANT, EGL_OPENGL_ES2_BIT,
		EGL_NONE,
	};

	EGLint n = 1;
	if (!eglChooseConfig(dev->egl, config_attribs, &dev->config, n, &n))
		fatal("Failed to choose EGL config");

	EGLint format;
	if (!eglGetConfigAttrib(dev->egl, dev->config, EGL_NATIVE_VISUAL_ID, &format))
		fatal("Could not query EGL config");

	dev->format = format;
	printf("Format: %.4s\n", (char *)&dev->format);

	EGLint context_attribs[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE,
	};

	dev->context = eglCreateContext(dev->egl, dev->config, EGL_NO_CONTEXT, context_attribs);
	if (dev->context == EGL_NO_CONTEXT)
		fatal("Failed to create EGL context");

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

		for (int i = 0; i < 2; ++i) {
			struct buf *buf = &conn->bufs[i];

			drmModeRmFB(dev->fd, buf->fb_id);
			gbm_bo_destroy(buf->bo);
		}

		drmModeCrtc *c = conn->old_crtc;

		drmModeSetCrtc(dev->fd, c->crtc_id, c->buffer_id, c->x, c->y,
			&conn->conn_id, 1, &c->mode);
		drmModeFreeCrtc(conn->old_crtc);

		free(conn);
	}

	gbm_device_destroy(dev->gbm);
	close(dev->fd);
	free(dev);
}

void draw(struct dev *dev, struct buf *buf, uint64_t n)
{
	eglMakeCurrent(dev->egl, EGL_NO_SURFACE, EGL_NO_SURFACE, dev->context);

	glBindRenderbuffer(GL_RENDERBUFFER, buf->renderbuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, buf->framebuffer);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, buf->renderbuffer);

	float f = fmodf(n / 10000000.0f, 360.0f);
	float x = 1.0f - fabsf(fmodf(f / 60.0f, 2.0f) - 1.0f);

	switch ((int)floorf(f / 60.0f)) {
	case 0:
		glClearColor(1.0, x, 0.0, 1.0);
		break;
	case 1:
		glClearColor(x, 1.0, 0.0, 1.0);
		break;
	case 2:
		glClearColor(0.0, 1.0, x, 1.0);
		break;
	case 3:
		glClearColor(0.0, x, 1.0, 1.0);
		break;
	case 4:
		glClearColor(x, 0.0, 1.0, 1.0);
		break;
	case 5:
		glClearColor(1.0, 0.0, x, 1.0);
		break;
	}

	glClear(GL_COLOR_BUFFER_BIT);

	EGLSync sync = eglCreateSync(dev->egl, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
	if (sync == EGL_NO_SYNC)
		fatal("Failed to create EGL sync");

	buf->fence = eglDupNativeFenceFDANDROID(dev->egl, sync);
	if (buf->fence < 0)
		fatal("Failed to get EGL sync FD");

	glFlush();

	eglDestroySync(dev->egl, sync);
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

	conn->width = drm_conn->modes[0].hdisplay;
	conn->height = drm_conn->modes[0].vdisplay;

	drmModeFreeConnector(drm_conn);

	uint64_t curr_crtc_id = 0;

	struct prop conn_props[] = {
		{ "CRTC_ID", &conn->conn_props.crtc_id, &curr_crtc_id },
	};

	struct prop crtc_props[] = {
		{ "ACTIVE", &conn->crtc_props.active, NULL },
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

	eglMakeCurrent(dev->egl, EGL_NO_SURFACE, EGL_NO_SURFACE, dev->context);

	for (int i = 0; i < 2; ++i) {
		struct buf *buf = &conn->bufs[i];

		buf->bo = gbm_bo_create_with_modifiers(dev->gbm, conn->width, conn->height,
			dev->format, NULL, 0);
		if (!buf->bo)
			fatal("Failed to create GBM bo");

		struct gbm_bo *bo = buf->bo;

		uint32_t width = gbm_bo_get_width(bo);
		uint32_t height = gbm_bo_get_height(bo);
		uint32_t pixel_format = gbm_bo_get_format(bo);
		uint32_t bo_handles[4] = { 0 };
		uint32_t pitches[4] = { 0 };
		uint32_t offsets[4] = { 0 };

		uint64_t mod = gbm_bo_get_modifier(bo);
		uint64_t modifier_buf[4] = { mod, mod, mod, mod };

		uint64_t *modifier = NULL;
		uint64_t flags = 0;

		if (mod != DRM_FORMAT_MOD_INVALID) {
			modifier = modifier_buf;
			flags = DRM_MODE_FB_MODIFIERS;
		}

		int planes = gbm_bo_get_plane_count(bo);
		for (int i = 0; i < planes; ++i) {
			bo_handles[i] = gbm_bo_get_handle_for_plane(bo, i).u32;
			pitches[i] = gbm_bo_get_stride_for_plane(bo, i);
			offsets[i] = gbm_bo_get_offset(bo, i);
		}

		if (drmModeAddFB2WithModifiers(dev->fd, width, height, pixel_format,
				bo_handles, pitches, offsets, modifier, &buf->fb_id, flags))
			fatal_errno("Failed to create DRM buffer");

		buf->image = eglCreateImage(dev->egl, dev->context,
			EGL_NATIVE_PIXMAP_KHR, bo, NULL);
		if (buf->image == EGL_NO_IMAGE)
			fatal("Failed to create EGL image");

		glGenRenderbuffers(1, &buf->renderbuffer);
		glBindRenderbuffer(GL_RENDERBUFFER, buf->renderbuffer);
		glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER, buf->image);

		glGenFramebuffers(1, &buf->framebuffer);
	}

	drmCrtcGetSequence(dev->fd, conn->crtc_id, NULL, &conn->start_ns);
	conn->curr_ns = conn->start_ns;

	struct buf *buf = &conn->bufs[conn->front];
	draw(dev, buf, 0);

	conn->atomic = drmModeAtomicAlloc();
	if (!conn->atomic)
		fatal_errno("Failed to allocate atomic request");

	drmModeAtomicAddProperty(conn->atomic, conn->conn_id, conn->conn_props.crtc_id, conn->crtc_id);
	drmModeAtomicAddProperty(conn->atomic, conn->crtc_id, conn->crtc_props.active, 1);
	drmModeAtomicAddProperty(conn->atomic, conn->crtc_id, conn->crtc_props.mode_id, conn->mode_id);
	drmModeAtomicAddProperty(conn->atomic, conn->crtc_id, conn->crtc_props.out_fence_ptr, (uint64_t)&conn->fence);
	drmModeAtomicAddProperty(conn->atomic, conn->primary_id, conn->plane_props.crtc_id, conn->crtc_id);
	drmModeAtomicAddProperty(conn->atomic, conn->primary_id, conn->plane_props.crtc_x, 0);
	drmModeAtomicAddProperty(conn->atomic, conn->primary_id, conn->plane_props.crtc_y, 0);
	drmModeAtomicAddProperty(conn->atomic, conn->primary_id, conn->plane_props.crtc_w, conn->width);
	drmModeAtomicAddProperty(conn->atomic, conn->primary_id, conn->plane_props.crtc_h, conn->height);
	drmModeAtomicAddProperty(conn->atomic, conn->primary_id, conn->plane_props.src_x, 0);
	drmModeAtomicAddProperty(conn->atomic, conn->primary_id, conn->plane_props.src_y, 0);
	drmModeAtomicAddProperty(conn->atomic, conn->primary_id, conn->plane_props.src_w, conn->width << 16);
	drmModeAtomicAddProperty(conn->atomic, conn->primary_id, conn->plane_props.src_h, conn->height << 16);

	int cursor = drmModeAtomicGetCursor(conn->atomic);
	drmModeAtomicAddProperty(conn->atomic, conn->primary_id, conn->plane_props.fb_id, buf->fb_id);
	drmModeAtomicAddProperty(conn->atomic, conn->primary_id, conn->plane_props.in_fence_fd, buf->fence);

	if (drmModeAtomicCommit(dev->fd, conn->atomic, DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_ATOMIC_NONBLOCK, NULL))
		fatal_errno("Atomic commit failed");

	drmModeAtomicSetCursor(conn->atomic, cursor);

	close(buf->fence);
	buf->fence = -1;

	drmCrtcQueueSequence(dev->fd, conn->crtc_id,
		DRM_CRTC_SEQUENCE_RELATIVE | DRM_CRTC_SEQUENCE_NEXT_ON_MISS,
		1, NULL, (uint64_t)conn);
}

void swap_buffers(struct conn *conn)
{
	struct dev *dev = conn->dev;

	close(conn->fence);
	conn->front = (conn->front + 1) % 2;
	struct buf *buf = &conn->bufs[conn->front];

	int cursor = drmModeAtomicGetCursor(conn->atomic);
	drmModeAtomicAddProperty(conn->atomic, conn->primary_id, conn->plane_props.fb_id, buf->fb_id);
	drmModeAtomicAddProperty(conn->atomic, conn->primary_id, conn->plane_props.in_fence_fd, buf->fence);

	if (drmModeAtomicCommit(dev->fd, conn->atomic, DRM_MODE_ATOMIC_NONBLOCK, NULL))
		fatal_errno("Atomic commit failed");

	drmModeAtomicSetCursor(conn->atomic, cursor);

	close(buf->fence);
	buf->fence = -1;
}
