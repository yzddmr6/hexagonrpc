/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Exercise real apps_std handlers and HexagonFS; no FastRPC ioctl or DSP. */
#include <dirent.h>
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
#define REG_PARENT "/mnt/vendor/persist/sensors/registry/"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static char *join(const char *dir, const char *name)
{
	char *out = malloc(strlen(dir) + strlen(name) + 2);
	CHECK(out);
	sprintf(out, "%s%s%s", dir, *dir && dir[strlen(dir) - 1] == '/' ? "" : "/", name);
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

/* apps_std method 33 through its extended-ID primary buffer, as the DSP sends it. */
static uint32_t rpc_rename(struct fastrpc_interface *iface, const char *from, const char *to)
{
	uint32_t words[] = { 33, strlen(from) + 1, strlen(to) + 1 };
	struct fastrpc_io_buffer in[] = {
		{ sizeof(words), words }, { words[1], (void *)from }, { words[2], (void *)to },
	};
	return invoke(iface, 33, in, NULL);
}

static void rpc_store(struct fastrpc_interface *iface, const char *path, const char *data, size_t size)
{
	uint32_t written;
	int fd = rpc_open(iface, path, "w"); CHECK(fd >= 0);
	CHECK(!rpc_write(iface, fd, data, size, size, &written) && written == size);
	rpc_close(iface, fd);
}

static bool host_exists(const char *dir, const char *name)
{
	char *path = join(dir, name);
	struct stat st;
	bool found = !lstat(path, &st);
	free(path);
	return found;
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
	CHECK(rpc_open(iface, REG "../../escape", "w") < 0);
	CHECK(rpc_open(iface, REG "../../../../../../outside", "w") < 0);
	CHECK(rpc_open(iface, REG "./..", "w") < 0);
	CHECK(rpc_open(iface, "/", "w") < 0);
	CHECK(rpc_remove(iface, "/odm/etc/sensors/config/json.lst") != 0);
	CHECK(rpc_remove(iface, REG "../../escape") != 0);
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

/* frename in the writable parent SCRATCH (holding registry/); ROOT is outside it. */
static void renames(struct fastrpc_interface *iface, const char *root, const char *scratch)
{
	char *a, *b, *inner = join(scratch, "registry");
	uint32_t bad[] = { 33, 4, 4 };
	char unterminated[4] = { 'a', 'b', 'c', 'd' };
	struct fastrpc_io_buffer in[] = { { sizeof(bad), bad }, { 4, unterminated }, { 4, unterminated } };
	int fd;
	/* The DSP's sequence: write the temporary file, then rename it into place. */
	rpc_store(iface, REG_PARENT "fstempfile", "group-a", 7);
	CHECK(rpc_rename(iface, REG_PARENT "fstempfile", REG "group") == AEE_SUCCESS);
	compare_text(inner, "group", "group-a");
	CHECK(!host_exists(scratch, "fstempfile"));
	a = join(inner, "group"); compare_file(iface, REG "group", a); free(a);
	/* An existing target is replaced; an open reader keeps the old bytes. */
	fd = rpc_open(iface, REG "group", "r"); CHECK(fd >= 0);
	rpc_store(iface, REG_PARENT "fstempfile", "group-b", 7);
	CHECK(rpc_rename(iface, REG_PARENT "fstempfile", REG "group") == AEE_SUCCESS);
	compare_text(inner, "group", "group-b");
	{
		uint32_t words[] = { (uint32_t)fd, 64 }, result[2];
		char old[64];
		struct fastrpc_io_buffer rin[] = { { sizeof(words), words } };
		struct fastrpc_io_buffer rout[] = { { sizeof(result), result }, { sizeof(old), old } };
		CHECK(invoke(iface, 4, rin, rout) == 0 && result[0] == 7 && !memcmp(old, "group-a", 7));
	}
	rpc_close(iface, fd);
	/* Other spellings that resolve inside the copy, across its two levels. */
	rpc_store(iface, REG "../fstempfile", "group-c", 7);
	CHECK(rpc_rename(iface, REG "../fstempfile", REG "./../registry/group2") == AEE_SUCCESS);
	compare_text(inner, "group2", "group-c");
	CHECK(rpc_rename(iface, REG "group2", REG_PARENT "parsed_file_list.csv") == AEE_SUCCESS);
	compare_text(scratch, "parsed_file_list.csv", "group-c");
	CHECK(rpc_rename(iface, "/persist/sensors/registry/parsed_file_list.csv", REG "group2") == AEE_SUCCESS);
	compare_text(inner, "group2", "group-c");
	/* No fake success: a missing source fails and creates nothing. */
	CHECK(rpc_rename(iface, REG_PARENT "fstempfile", REG "missing") == AEE_EFAILED);
	CHECK(!host_exists(inner, "missing"));
	/* Escapes out of the writable copy, with ".." or absolute paths. */
	CHECK(rpc_rename(iface, REG "group", REG_PARENT "../escaped") == AEE_EFAILED);
	CHECK(rpc_rename(iface, REG "group", REG "../../escaped") == AEE_EFAILED);
	CHECK(rpc_rename(iface, REG "group", REG "../../../../../../../escaped") == AEE_EFAILED);
	CHECK(rpc_rename(iface, REG "group", "/odm/etc/sensors/config/json.lst") == AEE_EFAILED);
	CHECK(rpc_rename(iface, REG "group", "/vendor/etc/sensors/config/piano.json") == AEE_EFAILED);
	CHECK(rpc_rename(iface, "/odm/etc/sensors/config/json.lst", REG "stolen") == AEE_EFAILED);
	CHECK(rpc_rename(iface, REG_PARENT "../registry", REG "stolen") == AEE_EFAILED);
	CHECK(rpc_rename(iface, "group", "group3") == AEE_EFAILED); /* relative: ADSP library dir */
	CHECK(rpc_rename(iface, REG "group", REG "..") == AEE_EFAILED);
	CHECK(rpc_rename(iface, REG "group", REG ".") == AEE_EFAILED);
	CHECK(rpc_rename(iface, REG "group", REG) == AEE_EFAILED); /* empty leaf */
	CHECK(rpc_rename(iface, REG "..", REG "group3") == AEE_EFAILED);
	CHECK(rpc_rename(iface, "/", REG "group3") == AEE_EFAILED);
	compare_text(inner, "group", "group-b");
	CHECK(!host_exists(root, "escaped") && !host_exists(root, "sensors/escaped"));
	compare_text(root, "sensors/odm/config/json.lst", "piano.json\n");
	/* Links and directories are never moved or replaced. */
	host_write(root, "rename-outside", "original");
	a = join(root, "rename-outside");
	b = join(inner, "rsymlink"); CHECK(!symlink(a, b)); free(b);
	b = join(inner, "rhardlink"); CHECK(!link(a, b)); free(b);
	b = join(inner, "rdirlink"); CHECK(!symlink(root, b)); free(b);
	free(a);
	CHECK(rpc_rename(iface, REG "rsymlink", REG "moved") == AEE_EFAILED);
	CHECK(rpc_rename(iface, REG "rhardlink", REG "moved") == AEE_EFAILED);
	CHECK(rpc_rename(iface, REG "group", REG "rsymlink") == AEE_EFAILED);
	CHECK(rpc_rename(iface, REG "group", REG "rhardlink") == AEE_EFAILED);
	CHECK(rpc_rename(iface, REG "group", REG "rdirlink/rename-outside") == AEE_EFAILED);
	CHECK(rpc_rename(iface, REG "rdirlink/rename-outside", REG "moved") == AEE_EFAILED);
	CHECK(rpc_rename(iface, REG_PARENT "registry", REG_PARENT "moved") == AEE_EFAILED);
	CHECK(rpc_rename(iface, REG "group", REG_PARENT "registry") == AEE_EFAILED);
	CHECK(!host_exists(inner, "moved") && !host_exists(scratch, "moved"));
	compare_text(root, "rename-outside", "original");
	compare_text(inner, "group", "group-b");
	b = join(inner, "rsymlink"); { struct stat st; CHECK(!lstat(b, &st) && S_ISLNK(st.st_mode)); } free(b);
	/* Names must be NUL-terminated inside their buffers. */
	CHECK(invoke(iface, 33, in, NULL) == AEE_EBADPARM);
	for (const char *const *n = (const char *const[]){ "rsymlink", "rhardlink", "rdirlink", "group", "group2", NULL }; *n; n++) {
		a = join(inner, *n); CHECK(!unlink(a)); free(a);
	}
	a = join(root, "rename-outside"); CHECK(!unlink(a)); free(a);
	a = join(scratch, "parsed_file_list.csv"); unlink(a); free(a);
	free(inner);
}

static int skip_dots(const struct dirent *entry)
{
	return strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..");
}

static void slurp(const char *dir, const char *name, char **data, size_t *size)
{
	char *path = join(dir, name);
	struct stat st;
	int fd = open(path, O_RDONLY | O_NOFOLLOW);
	CHECK(fd >= 0 && !fstat(fd, &st) && S_ISREG(st.st_mode));
	*size = st.st_size;
	*data = malloc(*size + 1); CHECK(*data);
	CHECK(read(fd, *data, *size) == (ssize_t)*size);
	close(fd); free(path);
}

/*
 * Replay the stock persist registry STOCK (outer directory with registry/,
 * sns_reg_version and parsed_file_list.csv) as the DSP stores it: each registry
 * group and the parsed-file list through fstempfile + frename, the version file
 * by a direct write. The rename targets were not logged on piano; the stock tree
 * and the absence of fstempfile in it define the expected end state.
 */
static size_t replay_stock_registry(struct fastrpc_interface *iface, const char *scratch, const char *stock)
{
	struct dirent **entries;
	char *inner = join(stock, "registry"), *scratch_inner = join(scratch, "registry"), *data;
	size_t size, renames = 0;
	int n = scandir(inner, &entries, skip_dots, alphasort);
	CHECK(n > 0);
	for (int i = 0; i < n; i++) {
		char *virtual = join(REG, entries[i]->d_name);
		slurp(inner, entries[i]->d_name, &data, &size);
		rpc_store(iface, REG_PARENT "fstempfile", data, size);
		CHECK(rpc_rename(iface, REG_PARENT "fstempfile", virtual) == AEE_SUCCESS);
		renames++;
		free(data); free(virtual);
	}
	slurp(stock, "parsed_file_list.csv", &data, &size);
	rpc_store(iface, REG_PARENT "fstempfile", data, size);
	CHECK(rpc_rename(iface, REG_PARENT "fstempfile", REG_PARENT "parsed_file_list.csv") == AEE_SUCCESS);
	renames++;
	free(data);
	slurp(stock, "sns_reg_version", &data, &size);
	rpc_store(iface, REG_PARENT "sns_reg_version", data, size);
	free(data);
	/* End state: byte-identical to the stock tree, nothing more, no fstempfile. */
	CHECK(!host_exists(scratch, "fstempfile"));
	for (int i = 0; i < n; i++) {
		char *virtual = join(REG, entries[i]->d_name), *expected, *actual, *physical = join(inner, entries[i]->d_name);
		size_t expected_size, actual_size;
		slurp(inner, entries[i]->d_name, &expected, &expected_size);
		slurp(scratch_inner, entries[i]->d_name, &actual, &actual_size);
		CHECK(expected_size == actual_size && !memcmp(expected, actual, actual_size));
		compare_file(iface, virtual, physical);
		free(expected); free(actual); free(virtual); free(physical); free(entries[i]);
	}
	free(entries);
	for (const char *const *f = (const char *const[]){ "parsed_file_list.csv", "sns_reg_version", NULL }; *f; f++) {
		char *expected, *actual;
		size_t expected_size, actual_size;
		slurp(stock, *f, &expected, &expected_size);
		slurp(scratch, *f, &actual, &actual_size);
		CHECK(expected_size == actual_size && !memcmp(expected, actual, actual_size));
		free(expected); free(actual);
	}
	CHECK(scandir(scratch_inner, &entries, skip_dots, alphasort) == n);
	for (int i = 0; i < n; i++)
		free(entries[i]);
	free(entries);
	CHECK(scandir(scratch, &entries, skip_dots, alphasort) == 3); /* registry, two files */
	for (int i = 0; i < 3; i++)
		free(entries[i]);
	free(entries);
	free(inner); free(scratch_inner);
	return renames;
}

static void parent_writes(struct fastrpc_interface *iface, const char *scratch)
{
	const char *names[] = { "sns_reg_version", "fstempfile", "parsed_file_list.csv" };
	const char *modes[] = { "w", "wb", "w+" };
	uint32_t written;
	for (size_t i = 0; i < 3; i++) {
		char *virtual = join(REG_PARENT, names[i]);
		char *physical = join(scratch, names[i]);
		for (size_t j = 0; j < 3; j++) {
			int fd = rpc_open(iface, virtual, modes[j]); CHECK(fd >= 0);
			CHECK(!rpc_write(iface, fd, "parsed", 6, 6, &written) && written == 6);
			rpc_close(iface, fd);
			compare_text(scratch, names[i], "parsed");
			compare_file(iface, virtual, physical);
		}
		free(virtual); free(physical);
	}
	/* 108 rejected fstempfile opens were present in the v2 log. */
	for (unsigned int i = 0; i < 108; i++) {
		int fd = rpc_open(iface, REG_PARENT "fstempfile", "w"); CHECK(fd >= 0);
		CHECK(!rpc_write(iface, fd, "parsed", 6, 6, &written));
		rpc_close(iface, fd);
	}
	/* Both observed absolute and inner/../ spellings stay in the same copy. */
	int fd = rpc_open(iface, REG "../fstempfile", "a"); CHECK(fd >= 0);
	CHECK(!rpc_write(iface, fd, "+", 1, 1, &written));
	rpc_close(iface, fd); compare_text(scratch, "fstempfile", "parsed+");
	CHECK(rpc_open(iface, REG_PARENT "../outside", "w") < 0);
	CHECK(rpc_remove(iface, REG_PARENT "../outside") != 0);
}

int main(int argc, char **argv)
{
	char fixture[] = "/tmp/hexagonrpc-registry-XXXXXX";
	char *root, *scratch, *path;
	struct fastrpc_interface *iface;
	int fd;
	if (argc == 5 && !strcmp(argv[4], "--parents-only")) {
		iface = fastrpc_apps_std_init(construct_root_dir(argv[1], "adsp", argv[2])); CHECK(iface);
		parent_writes(iface, argv[2]);
		fastrpc_apps_std_deinit(iface);
		puts("REGISTRY_PARENT_IO_PASS");
		return 0;
	}
	if (argc == 4 || (argc == 5 && strcmp(argv[4], "--parents-only"))) {
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
		rpc_close(iface, fd); compare_text(argv[2], "registry/DIR", "DIR");
		parent_writes(iface, argv[2]);
		size_t renamed = argc == 5 ? replay_stock_registry(iface, argv[2], argv[4]) : 0;
		fastrpc_apps_std_deinit(iface);
		printf("REGISTRY_STOCK_REPLAY_PASS files=%zu actual_DIR_write=3 parents=3 fstempfile_reopens=108 "
		       "stock_registry_frename=%zu\n", count, renamed);
		return 0;
	}
	CHECK(argc == 1);
	root = mkdtemp(fixture); CHECK(root);
	const char *dirs[] = { "sensors", "sensors/odm", "sensors/odm/config", "sensors/vendor", "sensors/config", "sensors/registry", "sensors/registry/registry", "scratch", "scratch/registry" };
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
	path = join(scratch, "registry"); mutations(iface, root, path); free(path);
	renames(iface, root, scratch);
	parent_writes(iface, scratch);
	fastrpc_apps_std_deinit(iface);
	iface = fastrpc_apps_std_init(construct_root_dir(root, "adsp", NULL)); CHECK(iface);
	CHECK(rpc_open(iface, REG "DIR", "w") < 0);
	CHECK(rpc_open(iface, REG_PARENT "sns_reg_version", "w") < 0);
	/* Without -w the registry mapping is read-only for frename too. */
	host_write(root, "sensors/registry/registry/group", "read-only");
	CHECK(rpc_rename(iface, REG "group", REG "moved") == AEE_EFAILED);
	compare_text(root, "sensors/registry/registry/group", "read-only");
	CHECK(!host_exists(root, "sensors/registry/registry/moved"));
	fastrpc_apps_std_deinit(iface);
	/* Keep the synthetic fixture path in the log for sanitizer/failure inspection. */
	printf("REGISTRY_IO_PASS fixture=%s\n", root);
	free(scratch);
	return 0;
}
