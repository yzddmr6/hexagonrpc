/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Exercise real apps_std handlers and HexagonFS; no FastRPC ioctl or DSP. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libhexagonrpc/error.h>
#include "../hexagonrpcd/apps_std.h"
#include "../hexagonrpcd/listener.h"
#include "../hexagonrpcd/rpcd_builder.h"

#define REG "/mnt/vendor/persist/sensors/registry/registry/"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static char *join(const char *dir, const char *name)
{
	char *out = malloc(strlen(dir) + strlen(name) + 2);
	CHECK(out);
	sprintf(out, "%s/%s", dir, name);
	return out;
}

static void host_write(const char *root, const char *name, const char *data)
{
	char *path = join(root, name);
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	CHECK(fd >= 0);
	CHECK(write(fd, data, strlen(data)) == (ssize_t)strlen(data));
	CHECK(!close(fd));
	free(path);
}

static uint32_t invoke(struct fastrpc_interface *iface, unsigned int id,
		       struct fastrpc_io_buffer *in, struct fastrpc_io_buffer *out)
{
	CHECK(id < iface->n_procs && iface->procs[id].impl);
	CHECK(iface->procs[id].def->msg_id == id);
	return iface->procs[id].impl(iface->data, in, out);
}

static int rpc_open(struct fastrpc_interface *iface, const char *path, const char *mode)
{
	char env[] = "ADSP_LIBRARY_PATH", delim[] = ";";
	uint32_t lens[] = { sizeof(env), sizeof(delim), strlen(path) + 1, strlen(mode) + 1 };
	uint32_t fd = UINT32_MAX;
	struct fastrpc_io_buffer in[] = {
		{ sizeof(lens), lens }, { sizeof(env), env }, { sizeof(delim), delim },
		{ strlen(path) + 1, (void *)path }, { strlen(mode) + 1, (void *)mode },
	}, out[] = { { sizeof(fd), &fd } };
	return invoke(iface, 19, in, out) ? -1 : (int)fd;
}

static void rpc_close(struct fastrpc_interface *iface, uint32_t fd)
{
	struct fastrpc_io_buffer in[] = { { sizeof(fd), &fd } };
	CHECK(invoke(iface, 3, in, NULL) == 0);
}

static uint32_t rpc_write(struct fastrpc_interface *iface, uint32_t fd, const char *data,
			  size_t size, uint32_t declared, uint32_t *written)
{
	uint32_t words[] = { fd, declared }, result[] = { 0xdeadbeef, 0xdeadbeef };
	struct fastrpc_io_buffer in[] = { { sizeof(words), words }, { size, (void *)data } };
	struct fastrpc_io_buffer out[] = { { sizeof(result), result } };
	uint32_t ret = invoke(iface, 5, in, out);
	*written = result[0];
	CHECK(result[1] == 0);
	return ret;
}

static uint32_t rpc_remove(struct fastrpc_interface *iface, const char *name)
{
	uint32_t len = strlen(name) + 1;
	struct fastrpc_io_buffer in[] = { { sizeof(len), &len }, { len, (void *)name } };
	return invoke(iface, 24, in, NULL);
}

/* Independent POSIX bytes vs bytes served over the actual apps_std handlers. */
static void compare_file(struct fastrpc_interface *iface, const char *virtual, const char *physical)
{
	uint32_t fd = rpc_open(iface, virtual, "rb");
	uint32_t words[] = { fd, 4096 }, result[2];
	char expected[4096], actual[4096];
	struct fastrpc_io_buffer in[] = { { sizeof(words), words } };
	struct fastrpc_io_buffer out[] = { { sizeof(result), result }, { sizeof(actual), actual } };
	int source = open(physical, O_RDONLY);
	ssize_t n;
	CHECK((int)fd >= 0 && source >= 0);
	do {
		n = read(source, expected, sizeof(expected));
		CHECK(n >= 0);
		CHECK(invoke(iface, 4, in, out) == 0);
		CHECK(result[0] == n && result[1] == (n < (ssize_t)sizeof(expected)));
		CHECK(!memcmp(expected, actual, n));
	} while (n);
	close(source);
	rpc_close(iface, fd);
}

static void compare_text(const char *dir, const char *name, const char *expected)
{
	char *path = join(dir, name), buf[64] = { 0 };
	int fd = open(path, O_RDONLY);
	CHECK(fd >= 0);
	CHECK(read(fd, buf, sizeof(buf)) == (ssize_t)strlen(expected));
	CHECK(!memcmp(buf, expected, strlen(expected)));
	close(fd); free(path);
}

static void mutations(struct fastrpc_interface *iface, const char *root, const char *scratch)
{
	int fd;
	uint32_t written;
	char *a, *b;
	/* This is the exact write-open path seen on piano, not a fabricated ACK. */
	fd = rpc_open(iface, REG "DIR", "w"); CHECK(fd >= 0);
	CHECK(!rpc_write(iface, fd, "DIR", 3, 3, &written) && written == 3);
	rpc_close(iface, fd); compare_text(scratch, "DIR", "DIR");
	fd = rpc_open(iface, REG "DIR", "ab"); CHECK(fd >= 0);
	CHECK(!rpc_write(iface, fd, "+", 1, 1, &written) && written == 1);
	rpc_close(iface, fd); compare_text(scratch, "DIR", "DIR+");
	fd = rpc_open(iface, REG "DIR", "r+"); CHECK(fd >= 0);
	CHECK(!rpc_write(iface, fd, "X", 1, 1, &written));
	CHECK(rpc_write(iface, fd, "X", 1, 2, &written) == AEE_EBADPARM && !written);
	rpc_close(iface, fd); compare_text(scratch, "DIR", "XIR+");
	fd = rpc_open(iface, REG "DIR", "w+b"); CHECK(fd >= 0);
	CHECK(!rpc_write(iface, fd, "DIR", 3, 3, &written));
	rpc_close(iface, fd); compare_text(scratch, "DIR", "DIR");
	fd = rpc_open(iface, REG "DIR", "r"); CHECK(fd >= 0);
	CHECK(rpc_write(iface, fd, "X", 1, 1, &written) != 0 && !written);
	rpc_close(iface, fd);
	CHECK(rpc_write(iface, UINT32_MAX, "X", 1, 1, &written) != 0 && !written);
	CHECK(rpc_open(iface, REG "DIR", "wx-invalid") < 0);
	CHECK(rpc_open(iface, "/odm/etc/sensors/config/json.lst", "w") < 0);
	CHECK(rpc_open(iface, "/vendor/etc/sensors/sns_reg_config", "r+") < 0);
	CHECK(rpc_open(iface, REG "../escape", "w") < 0);
	CHECK(rpc_open(iface, REG "../../../../../../outside", "w") < 0);
	CHECK(rpc_open(iface, REG "./..", "w") < 0);
	CHECK(rpc_open(iface, "/", "w") < 0);
	CHECK(rpc_remove(iface, "/odm/etc/sensors/config/json.lst") != 0);
	CHECK(rpc_remove(iface, REG "../escape") != 0);
	CHECK(rpc_remove(iface, REG "..") != 0);
	/* Verify link escapes and hard-link truncation do not touch original bytes. */
	host_write(root, "outside", "original");
	a = join(root, "outside"); b = join(scratch, "symlink"); CHECK(!symlink(a, b)); free(b);
	b = join(scratch, "hardlink"); CHECK(!link(a, b)); free(b);
	b = join(scratch, "dirlink"); CHECK(!symlink(root, b)); free(b);
	CHECK(rpc_open(iface, REG "symlink", "w") < 0);
	CHECK(rpc_open(iface, REG "hardlink", "w") < 0);
	CHECK(rpc_open(iface, REG "dirlink/outside", "w") < 0);
	CHECK(rpc_remove(iface, REG "symlink") != 0);
	CHECK(rpc_remove(iface, REG "hardlink") != 0);
	compare_text(root, "outside", "original");
	free(a);
	CHECK(!rpc_remove(iface, REG "DIR"));
	a = join(scratch, "DIR"); CHECK(access(a, F_OK) < 0 && errno == ENOENT); free(a);
	CHECK(rpc_remove(iface, REG "DIR") != 0); /* No fake success for a missing file. */
}

int main(int argc, char **argv)
{
	char fixture[] = "/tmp/hexagonrpc-registry-XXXXXX";
	char *root, *scratch, *path;
	struct fastrpc_interface *iface;
	int fd;
	if (argc == 4) {
		/* Real stock byte-mapping replay: scratch must be a disposable copy. */
		char *line = NULL;
		size_t len = 0, count = 0;
		FILE *manifest = fopen(argv[3], "r");
		CHECK(manifest);
		iface = fastrpc_apps_std_init(construct_root_dir(argv[1], "adsp", argv[2])); CHECK(iface);
		while (getline(&line, &len, manifest) >= 0) {
			char *tab = strchr(line, '\t'); CHECK(tab); *tab++ = '\0';
			tab[strcspn(tab, "\n")] = '\0';
			compare_file(iface, line, tab); count++;
		}
		fclose(manifest); free(line);
		fd = rpc_open(iface, REG "DIR", "w"); CHECK(fd >= 0);
		uint32_t written;
		CHECK(!rpc_write(iface, fd, "DIR", 3, 3, &written) && written == 3);
		rpc_close(iface, fd); compare_text(argv[2], "DIR", "DIR");
		fastrpc_apps_std_deinit(iface);
		printf("REGISTRY_STOCK_REPLAY_PASS files=%zu actual_DIR_write=3\n", count);
		return 0;
	}
	CHECK(argc == 1);
	root = mkdtemp(fixture); CHECK(root);
	const char *dirs[] = { "sensors", "sensors/odm", "sensors/odm/config", "sensors/vendor", "sensors/config", "sensors/registry", "scratch" };
	for (size_t i = 0; i < sizeof(dirs)/sizeof(dirs[0]); i++) {
		path = join(root, dirs[i]); CHECK(!mkdir(path, 0700)); free(path);
	}
	scratch = join(root, "scratch");
	host_write(root, "sensors/odm/config/json.lst", "piano.json\n");
	host_write(root, "sensors/odm/config/piano.json", "odm bytes\n");
	host_write(root, "sensors/config/piano.json", "vendor bytes\n");
	host_write(root, "sensors/sns_reg.conf", "version=1\n");
	iface = fastrpc_apps_std_init(construct_root_dir(root, "adsp", scratch)); CHECK(iface);
	path = join(root, "sensors/odm/config/json.lst"); compare_file(iface, "/odm/etc/sensors/config/json.lst", path); free(path);
	path = join(root, "sensors/odm/config/piano.json"); compare_file(iface, "/odm/etc/sensors/config/piano.json", path); free(path);
	path = join(root, "sensors/config/piano.json"); compare_file(iface, "/vendor/etc/sensors/config/piano.json", path); free(path);
	mutations(iface, root, scratch);
	fastrpc_apps_std_deinit(iface);
	iface = fastrpc_apps_std_init(construct_root_dir(root, "adsp", NULL)); CHECK(iface);
	CHECK(rpc_open(iface, REG "DIR", "w") < 0);
	fastrpc_apps_std_deinit(iface);
	/* Keep the synthetic fixture path in the log for sanitizer/failure inspection. */
	printf("REGISTRY_IO_PASS fixture=%s\n", root);
	free(scratch);
	return 0;
}
