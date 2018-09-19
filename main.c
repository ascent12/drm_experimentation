#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <unistd.h>
#include <poll.h>

#include <xf86drm.h>

#include "drm.h"
#include "util.h"

const char *progname;

static noreturn void usage(int status, const char *fmt, ...)
{
	if (fmt) {
		va_list args;
		va_start(args, fmt);
		vfprintf(stderr, fmt, args);
		va_end(args);

		fprintf(stderr, "\n");
	}

	fprintf(stderr, "usage:\n");
	fprintf(stderr, "  %s [-d drm-device] (-m conn-id -c crtc-id -p primary-id)+\n", progname);
	fprintf(stderr, "  %s -h\n", progname);

	exit(status);
}

void handle_sequence(int fd, uint64_t seq, uint64_t ns, uint64_t userdata)
{
	(void)fd;
	(void)seq;

	struct conn *conn = (void *)userdata;
	struct dev *dev = conn->dev;

	conn->curr_ns = ns;
	draw(dev, &conn->bufs[(conn->front + 1) % 2], ns - conn->start_ns);
}

int main(int argc, char *argv[])
{
	progname = argv[0];

	struct c {
		struct c *next;

		uint32_t conn_id;
		uint32_t crtc_id;
		uint32_t primary_id;
	};

	const char *gpu = NULL;
	struct c *conns = NULL;

	int c;
	while ((c = getopt(argc, argv, ":c:d:hm:p:")) != -1) {
		struct c *conn;

		switch (c) {
		case 'c':
			if (!conns)
				usage(1, "Preceeding conn-id required");
			if (conns->crtc_id)
				usage(1, "crtc-id already set for conn-id %"PRIu32, conns->conn_id);

			conns->crtc_id = atoi(optarg);
			if (!conns->crtc_id)
				usage(1, "Invalid crtc-id \"%s\"", optarg);

			break;
		case 'd':
			if (gpu)
				usage(1, "drm-device already set");

			gpu = optarg;
			break;
		case 'h':
			usage(0, NULL);
		case 'm':
			if (conns && (!conns->crtc_id || !conns->primary_id))
				usage(1, "Missing crtc-id or primary-id for conn-id %"PRIu32, conns->conn_id);

			conn = xalloc(sizeof *conn);

			conn->conn_id = atoi(optarg);
			if (!conn->conn_id)
				usage(1, "Invalid conn-id \"%s\"", optarg);

			conn->next = conns;
			conns = conn;
			break;
		case 'p':
			if (!conns)
				usage(1, "Preceeding conn-id required");
			if (conns->primary_id)
				usage(1, "primary-id already set for conn-id %"PRIu32, conns->conn_id);

			conns->primary_id = atoi(optarg);
			if (!conns->primary_id)
				usage(1, "Invalid primary-id \"%s\"", optarg);

			break;
		case ':':
			usage(1, "Option -%c requires an argument\n", optopt);
		case '?':
			usage(1, "Unknown option -%c\n", optopt);
		}
	}

	if (optind != argc)
		usage(1, argv[0], "Unexpected argument \"%s\"\n", argv[optind]);

	if (!gpu)
		gpu = "/dev/dri/card0";

	struct dev *dev = open_drm(gpu);

	struct c *tmp;
	for (struct c *conn = conns; conn; conn = tmp) {
		tmp = conn->next;
		new_connector(dev, conn->conn_id, conn->crtc_id, conn->primary_id);
		free(conn);
	}

	/*
	 * TODO: Add a proper event loop which can handle more than one connector.
	 * Some of this stuff doesn't belong in this file either.
	 */

	struct pollfd fd[2] = {
		{ .fd = dev->fd, .events = POLLIN },
		{ .fd = dev->conns->fence, .events = POLLIN },
	};

	while (1) {
		int ret = poll(fd, 2, -1);
		if (ret < 0)
			fatal_errno("poll failed");
		if (ret == 0)
			continue;

		if (fd[0].revents & (POLLERR | POLLHUP)) {
			break;
		} else {
			drmEventContext context = {
				.version = DRM_EVENT_CONTEXT_VERSION,
				.sequence_handler = handle_sequence,
			};

			drmHandleEvent(dev->fd, &context);
		}

		if (fd[1].revents & (POLLERR | POLLHUP)) {
			break;
		} else {
			struct conn *conn = dev->conns;

			swap_buffers(conn);
			fd[1].fd = conn->fence;

			drmCrtcQueueSequence(dev->fd, conn->crtc_id,
				DRM_CRTC_SEQUENCE_RELATIVE | DRM_CRTC_SEQUENCE_NEXT_ON_MISS,
				1, NULL, (uint64_t)conn);
		}
	}

	close_drm(dev);
}
