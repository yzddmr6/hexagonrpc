#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../hexagonrpcd/hexagonfs.h"
#include "../hexagonrpcd/rpcd_builder.h"

/*
 * The firmware reads its device-specific JSON list from
 * /odm/etc/sensor/config. It must resolve to <prefix>/sensors/odm-config,
 * and an absent host directory must still read as an empty directory.
 */
int main(int argc, char **argv)
{
	struct hexagonfs_fd *fds[HEXAGONFS_MAX_FD] = { 0 };
	struct hexagonfs_dirent *root;
	char sensors[4096], odm[4096], list[4096];
	int rootfd, fd, hostfd, n;

	if (argc != 2)
		return 2;
	if (mkdir(argv[1], 0700) && access(argv[1], F_OK))
		return 3;

	root = construct_root_dir(argv[1], "adsp");
	rootfd = hexagonfs_open_root(fds, root);
	if (rootfd < 0)
		return 4;

	fd = hexagonfs_openat(fds, rootfd, rootfd, "/odm/etc/sensor/config");
	printf("odm_config_absent_open=%d\n", fd);
	if (fd < 0)
		return 5;

	n = snprintf(sensors, sizeof(sensors), "%s/sensors", argv[1]);
	if (n < 0 || (size_t)n >= sizeof(sensors))
		return 6;
	n = snprintf(odm, sizeof(odm), "%s/odm-config", sensors);
	if (n < 0 || (size_t)n >= sizeof(odm))
		return 6;
	n = snprintf(list, sizeof(list), "%s/json_list", odm);
	if (n < 0 || (size_t)n >= sizeof(list))
		return 6;
	if (mkdir(sensors, 0700) && access(sensors, F_OK))
		return 7;
	if (mkdir(odm, 0700) && access(odm, F_OK))
		return 7;
	hostfd = open(list, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (hostfd < 0 || write(hostfd, "a.json\n", 7) != 7 || close(hostfd))
		return 8;

	fd = hexagonfs_openat(fds, rootfd, rootfd,
			      "/odm/etc/sensor/config/json_list");
	printf("odm_json_list_open=%d\n", fd);
	return fd < 0;
}
