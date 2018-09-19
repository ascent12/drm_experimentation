#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <unistd.h>

static noreturn void usage(const char *name, int status)
{
	fprintf(stderr, "usage:\n");
	fprintf(stderr, "  %s [-d drm-device]\n", name);
	fprintf(stderr, "  %s -h\n", name);

	exit(status);
}

int main(int argc, char *argv[])
{
	const char *gpu = "/dev/dri/card0";

	int c;
	while ((c = getopt(argc, argv, ":d:h")) != -1) {
		switch (c) {
		case 'd':
			gpu = optarg;
			break;
		case 'h':
			usage(argv[0], EXIT_SUCCESS);
		case ':':
			fprintf(stderr, "Option -%c requires an argument\n", optopt);
			usage(argv[0], EXIT_FAILURE);
		case '?':
			fprintf(stderr, "Unknown option -%c\n", optopt);
			usage(argv[0], EXIT_FAILURE);
		}
	}

	if (optind != argc) {
		fprintf(stderr, "Unexpected argument \"%s\"\n", argv[optind]);
		usage(argv[0], EXIT_FAILURE);
	}

	printf("%s\n", gpu);
}
