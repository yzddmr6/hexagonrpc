#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <libhexagonrpc/error.h>
#include <libhexagonrpc/hexagonrpc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../hexagonrpcd/apps_std.h"
#include "../hexagonrpcd/hexagonfs.h"
#include "../hexagonrpcd/interface/apps_std.h"
#include "../hexagonrpcd/iobuffer.h"
#include "../hexagonrpcd/listener.h"
#include "../hexagonrpcd/rpcd_builder.h"

/*
 * apps_std method 36 (fopen_with_env_fd) as the SM8750 ADSP sensor domain
 * sends it: sc 0x1f050100, i.e. extended method 31 with five input buffers
 * and one output buffer. The primary input carries the real method ID and
 * the four string lengths; the primary output carries { fd, len }.
 */

#define APPS_STD_FOPEN_WITH_ENV_SC REMOTE_SCALARS_MAKE(19, 5, 1)

static struct fastrpc_interface *apps_std;

static void fail(const char *message)
{
	fprintf(stderr, "test_apps_std_fd: %s\n", message);
	exit(1);
}

/* Dispatch-only tests must never issue a host FastRPC ioctl. */
int hexagonrpc(const struct hrpc_method_def_interp4 *def,
	       int fd, uint32_t handle, ...)
{
	fail("dispatch-only test attempted a FastRPC ioctl");
	return -1;
}

/* Encode input buffers as the kernel hands them to adsp_listener_next2. */
static size_t encode_wire(unsigned char *wire, size_t capacity,
			  size_t n, const void *const *bufs, const uint32_t *lens)
{
	size_t off = 0, i;
	uint32_t le;

	for (i = 0; i < n; i++) {
		if (off + 16 + lens[i] > capacity)
			fail("wire buffer is too small");

		le = htole32(lens[i]);
		memcpy(&wire[off], &le, sizeof(le));
		off += sizeof(le);

		if (lens[i] && (off & 7)) {
			memset(&wire[off], 0, 8 - (off & 7));
			off += 8 - (off & 7);
		}

		memcpy(&wire[off], bufs[i], lens[i]);
		off += lens[i];
	}

	return off;
}

static uint32_t invoke(uint32_t sc, uint32_t method,
		       const char *env, const char *name, const char *mode,
		       uint32_t name_len, int *dispatch_ret,
		       struct fastrpc_io_buffer **returned_out)
{
	struct fastrpc_interface *ifaces[] = { NULL, apps_std };
	struct fastrpc_decoder_context *ctx;
	struct fastrpc_io_buffer *decoded, *returned = NULL;
	unsigned char wire[256];
	uint32_t primary[5];
	const void *bufs[5];
	uint32_t lens[5];
	uint32_t result = UINT32_MAX;
	size_t wire_len;
	int extended = REMOTE_SCALARS_METHOD(sc) == 31;
	int off = 0;

	if (extended)
		primary[off++] = htole32(method);
	primary[off++] = htole32(strlen(env) + 1);
	primary[off++] = htole32(2);
	primary[off++] = htole32(name_len);
	primary[off++] = htole32(strlen(mode) + 1);

	bufs[0] = primary;
	lens[0] = off * sizeof(uint32_t);
	bufs[1] = env;
	lens[1] = strlen(env) + 1;
	bufs[2] = ";";
	lens[2] = 2;
	bufs[3] = name;
	lens[3] = name_len;
	bufs[4] = mode;
	lens[4] = strlen(mode) + 1;

	wire_len = encode_wire(wire, sizeof(wire), 5, bufs, lens);

	ctx = inbuf_decode_start(sc);
	if (ctx == NULL || inbuf_decode(ctx, wire_len, wire)
	 || !inbuf_decode_is_complete(ctx))
		fail("could not decode the request");
	decoded = inbuf_decode_finish(ctx);

	*dispatch_ret = fastrpc_invoke_requested_procedure(
		HRPC_ARRAY_SIZE(ifaces), ifaces, 1, sc, &result,
		decoded, &returned);
	iobuf_free(REMOTE_SCALARS_INBUFS(sc), decoded);
	*returned_out = returned;

	return result;
}

static uint32_t open_fd(const char *env, const char *name, const char *mode,
			int32_t *fd, int32_t *len)
{
	struct fastrpc_io_buffer *returned;
	int32_t out[2];
	uint32_t result;
	int dispatch_ret;

	result = invoke(APPS_STD_FOPEN_WITH_ENV_FD_SC, 36, env, name, mode,
			strlen(name) + 1, &dispatch_ret, &returned);
	if (dispatch_ret)
		fail("a well-formed method 36 request was rejected");
	if (returned == NULL || returned[0].s != sizeof(out))
		fail("method 36 did not return an 8-byte { fd, len }");

	memcpy(out, returned[0].p, sizeof(out));
	*fd = out[0];
	*len = out[1];
	iobuf_free(1, returned);

	return result;
}

static void test_abi(void)
{
	if (APPS_STD_FOPEN_WITH_ENV_FD_SC != 0x1f050100)
		fail("method 36 scalars differ from the ones the DSP sends");
	if (apps_std_fopen_with_env_fd_def.msg_id != 36)
		fail("method 36 definition has the wrong message ID");
	if (apps_std_interface.n_procs != 37)
		fail("apps_std does not reach method 36");
}

static void test_absent_file(void)
{
	int32_t fd, len;
	uint32_t result;

	result = open_fd("ADSP_LIBRARY_PATH", "absent.so", "r", &fd, &len);
	if (result != ENOENT || fd != -1 || len != 0)
		fail("an absent file was not reported as ENOENT with fd -1");

	/* Absolute names resolve from the root, as for fopen_with_env. */
	result = open_fd("ADSP_LIBRARY_PATH", "/vendor/etc/absent.json", "r",
			 &fd, &len);
	if (result != ENOENT)
		fail("an absent absolute path was not reported as ENOENT");

	/* No acdb directory is mapped in this root. */
	result = open_fd("ADSP_AVS_CFG_PATH", "absent.acdb", "r", &fd, &len);
	if (result != ENOENT || fd != -1)
		fail("a missing search directory was not reported as ENOENT");
}

static void test_present_file_is_refused(void)
{
	int32_t fd, len;
	uint32_t result;
	int i;

	/* More lookups than file numbers: a leaked number would surface. */
	for (i = 0; i < HEXAGONFS_MAX_FD + 8; i++) {
		result = open_fd("ADSP_LIBRARY_PATH", "present.so", "r",
				 &fd, &len);
		if (result != AEE_EUNSUPPORTED || fd != -1 || len != 0)
			fail("a present file was not refused cleanly");
	}
}

static void test_rejections(void)
{
	struct fastrpc_io_buffer *returned;
	int32_t fd, len;
	uint32_t result;
	int dispatch_ret;

	result = open_fd("ADSP_LIBRARY_PATH", "absent.so", "w", &fd, &len);
	if (result != AEE_EUNSUPPORTED)
		fail("a write-mode open was not refused");
	result = open_fd("ADSP_LIBRARY_PATH", "present.so", "r+", &fd, &len);
	if (result != AEE_EUNSUPPORTED)
		fail("an update-mode open was not refused");

	result = open_fd("DSP_UNKNOWN_PATH", "absent.so", "r", &fd, &len);
	if (result != AEE_EBADPARM)
		fail("an unknown search variable was not rejected");

	/* A name without its NUL terminator. */
	result = invoke(APPS_STD_FOPEN_WITH_ENV_FD_SC, 36, "ADSP_LIBRARY_PATH",
			"present.so", "r", strlen("present.so"),
			&dispatch_ret, &returned);
	if (dispatch_ret || result != AEE_EBADPARM)
		fail("an unterminated name was not rejected");
	iobuf_free(1, returned);

	/* Any other scalar shape is refused before the handler runs. */
	result = invoke(REMOTE_SCALARS_MAKE(31, 5, 2), 36, "ADSP_LIBRARY_PATH",
			"present.so", "r", strlen("present.so") + 1,
			&dispatch_ret, &returned);
	if (!dispatch_ret || result != AEE_EBADPARM || returned != NULL)
		fail("a non-ROM scalar shape was not rejected");

	/* Methods 32 to 35 stay unimplemented. */
	result = invoke(APPS_STD_FOPEN_WITH_ENV_FD_SC, 34, "ADSP_LIBRARY_PATH",
			"present.so", "r", strlen("present.so") + 1,
			&dispatch_ret, &returned);
	if (!dispatch_ret || result != AEE_EUNSUPPORTED || returned != NULL)
		fail("method 34 was not rejected");
}

static void test_fopen_with_env_unchanged(void)
{
	struct fastrpc_io_buffer *returned;
	uint32_t result;
	int dispatch_ret;

	result = invoke(APPS_STD_FOPEN_WITH_ENV_SC, 19, "ADSP_LIBRARY_PATH",
			"absent.so", "r", strlen("absent.so") + 1,
			&dispatch_ret, &returned);
	if (dispatch_ret || result != AEE_EFAILED)
		fail("fopen_with_env changed its absent-file result");
	iobuf_free(1, returned);

	result = invoke(APPS_STD_FOPEN_WITH_ENV_SC, 19, "DSP_UNKNOWN_PATH",
			"absent.so", "r", strlen("absent.so") + 1,
			&dispatch_ret, &returned);
	if (dispatch_ret || result != AEE_EBADPARM)
		fail("fopen_with_env changed its unknown-variable result");
	iobuf_free(1, returned);

	result = invoke(APPS_STD_FOPEN_WITH_ENV_SC, 19, "ADSP_LIBRARY_PATH",
			"present.so", "r", strlen("present.so") + 1,
			&dispatch_ret, &returned);
	if (dispatch_ret || result != AEE_SUCCESS)
		fail("fopen_with_env no longer opens a present file");
	iobuf_free(1, returned);
}

int main(int argc, char **argv)
{
	char dsp[4096], adsp[4096], lib[4096];
	int fd;

	if (argc != 2)
		return 2;
	if (mkdir(argv[1], 0700) && access(argv[1], F_OK))
		return 3;
	if (snprintf(dsp, sizeof(dsp), "%s/dsp", argv[1]) >= (int) sizeof(dsp)
	 || snprintf(adsp, sizeof(adsp), "%s/adsp", dsp) >= (int) sizeof(adsp)
	 || snprintf(lib, sizeof(lib), "%s/present.so", adsp) >= (int) sizeof(lib))
		return 3;
	if ((mkdir(dsp, 0700) && access(dsp, F_OK))
	 || (mkdir(adsp, 0700) && access(adsp, F_OK)))
		return 3;
	fd = open(lib, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0 || write(fd, "ELF", 3) != 3 || close(fd))
		return 3;

	apps_std = fastrpc_apps_std_init(construct_root_dir(argv[1], "adsp"));
	if (apps_std == NULL)
		fail("could not create the apps_std interface");

	test_abi();
	test_absent_file();
	test_present_file_is_refused();
	test_rejections();
	test_fopen_with_env_unchanged();

	/*
	 * Not deinitialized: fastrpc_apps_std_deinit() double-frees the shared
	 * directory descriptors even right after init; the daemon only calls it
	 * on a failed start.
	 */
	return 0;
}
