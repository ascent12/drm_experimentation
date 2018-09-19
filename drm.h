#ifndef DP_DRM_H
#define DP_DRM_H

#include <stdint.h>

#include <drm_mode.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <xf86drmMode.h>

struct conn;

struct dev {
	int fd;

	struct gbm_device *gbm;
	EGLDisplay egl;
	EGLContext context;
	EGLConfig config;

	uint32_t format;

	struct conn *conns;
};

struct buf {
	struct gbm_bo *bo;
	uint32_t fb_id;

	EGLImage image;
	GLuint renderbuffer;
	GLuint framebuffer;

	int fence;
};

struct conn {
	struct dev *dev;
	struct conn *next;

	uint32_t conn_id;
	uint32_t crtc_id;
	uint32_t primary_id;

	uint32_t mode_id;
	uint32_t width;
	uint32_t height;

	drmModeCrtc *old_crtc;

	struct {
		uint32_t crtc_id;
	} conn_props;

	struct {
		uint32_t active;
		uint32_t mode_id;
		uint32_t out_fence_ptr;
	} crtc_props;

	struct {
		uint32_t fb_id;
		uint32_t in_fence_fd;
		uint32_t crtc_id;
		uint32_t crtc_x;
		uint32_t crtc_y;
		uint32_t crtc_w;
		uint32_t crtc_h;
		uint32_t src_x;
		uint32_t src_y;
		uint32_t src_w;
		uint32_t src_h;
	} plane_props;

	int fence;

	int front;
	struct buf bufs[2];

	drmModeAtomicReq *atomic;

	uint64_t start_ns;
	uint64_t curr_ns;
};

struct dev *open_drm(const char *path);
void close_drm(struct dev *dev);

void new_connector(struct dev *dev, uint32_t conn_id, uint32_t crtc_id, uint32_t primary_id);

void swap_buffers(struct conn *conn);

void draw(struct dev *dev, struct buf *buf, uint64_t n);

#endif
