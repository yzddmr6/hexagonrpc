#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../hexagonrpcd/hexagonfs.h"
#include "../hexagonrpcd/rpcd_builder.h"

int main(int argc, char **argv)
{
	struct hexagonfs_fd *fds[HEXAGONFS_MAX_FD] = { 0 };
	struct hexagonfs_dirent *root;
	char sensors[4096], registry[4096], version[4096];
	int rootfd, fd, hostfd, n;

	if (argc != 2)
		return 2;
	if (mkdir(argv[1], 0700) && access(argv[1], F_OK))
		return 3;
	n = snprintf(sensors, sizeof(sensors), "%s/sensors", argv[1]);
	if (n < 0 || (size_t)n >= sizeof(sensors))
		return 4;
	n = snprintf(version, sizeof(version), "%s/sns_reg_version", sensors);
	if (n < 0 || (size_t)n >= sizeof(version))
		return 4;
	if (mkdir(sensors, 0700) && access(sensors, F_OK))
		return 5;
	n = snprintf(registry, sizeof(registry), "%s/registry", sensors);
	if (n < 0 || (size_t)n >= sizeof(registry))
		return 5;
	if (mkdir(registry, 0700) && access(registry, F_OK))
		return 6;
	hostfd = open(version, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (hostfd < 0 || write(hostfd, "version=12\0", 11) != 11 || close(hostfd))
		return 7;

	root = construct_root_dir(argv[1], "sdsp");
	rootfd = hexagonfs_open_root(fds, root);
	if (rootfd < 0)
		return 8;

	fd = hexagonfs_openat(fds, rootfd, rootfd,
			      "/mnt/vendor/persist/sensors/registry/registry/../sns_reg_version");
	printf("sns_reg_version_open=%d\n", fd);
	return fd < 0;
}
