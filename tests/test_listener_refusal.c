#include <endian.h>
#include <errno.h>
#include <libhexagonrpc/error.h>
#include <libhexagonrpc/hexagonrpc.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../hexagonrpcd/apps_std.h"
#include "../hexagonrpcd/interface/adsp_listener.h"
#include "../hexagonrpcd/interface/apps_std.h"
#include "../hexagonrpcd/listener.h"
#include "../hexagonrpcd/rpcd_builder.h"

/*
 * Drive run_fastrpc_listener() through a scripted remote side. A refused
 * request must be answered with its error and no output buffers, and the
 * listener must then fetch the next request instead of returning.
 */

struct step {
	/* What the listener must reply to the previous request. */
	uint32_t expect_result;
	uint32_t expect_outbufs_len;
	/* The next request, or none to tear the tunnel down. */
	int last;
	uint32_t handle;
	uint32_t sc;
	uint32_t method;
};

static const struct step script[] = {
	/* The first call carries no reply. */
	{ 0xffffffff, 0, 0, 1, APPS_STD_FOPEN_WITH_ENV_FD_SC, 37 },
	/* Unknown method: error, no output, keep going. */
	{ AEE_EUNSUPPORTED, 0, 0, 1, APPS_STD_FOPEN_WITH_ENV_FD_SC, 36 },
	/* Method 36 on an absent file: ENOENT and the { fd, len } buffer. */
	{ ENOENT, 16, 0, 7, APPS_STD_FOPEN_WITH_ENV_FD_SC, 36 },
	/* Unknown handle. */
	{ AEE_EUNSUPPORTED, 0, 0, 1, REMOTE_SCALARS_MAKE(31, 5, 2), 36 },
	/* Wrong scalar shape. */
	{ AEE_EBADPARM, 0, 1, 0, 0, 0 },
};

static size_t steps;

static void fail(const char *message)
{
	fprintf(stderr, "test_listener_refusal: step %zu: %s\n", steps, message);
	exit(1);
}

static size_t encode_request(char *wire, size_t capacity, uint32_t method)
{
	static const char *const strings[] = {
		"ADSP_LIBRARY_PATH", ";", "absent.so", "r",
	};
	uint32_t primary[5];
	const void *bufs[5];
	uint32_t lens[5];
	size_t off = 0, i;
	uint32_t le;

	primary[0] = htole32(method);
	for (i = 0; i < 4; i++)
		primary[i + 1] = htole32(strlen(strings[i]) + 1);

	bufs[0] = primary;
	lens[0] = sizeof(primary);
	for (i = 0; i < 4; i++) {
		bufs[i + 1] = strings[i];
		lens[i + 1] = strlen(strings[i]) + 1;
	}

	for (i = 0; i < 5; i++) {
		if (off + 16 + lens[i] > capacity)
			fail("request does not fit");

		le = htole32(lens[i]);
		memcpy(&wire[off], &le, sizeof(le));
		off += sizeof(le);

		if (off & 7) {
			memset(&wire[off], 0, 8 - (off & 7));
			off += 8 - (off & 7);
		}

		memcpy(&wire[off], bufs[i], lens[i]);
		off += lens[i];
	}

	return off;
}

static void check_fd_reply(const char *outbufs)
{
	uint32_t size;
	int32_t out[2];

	memcpy(&size, outbufs, sizeof(size));
	memcpy(out, &outbufs[8], sizeof(out));
	if (le32toh(size) != 8 || out[0] != -1 || out[1] != 0)
		fail("method 36 reply is not { fd = -1, len = 0 }");
}

/* The remote side of adsp_listener: init2, then one next2 per step. */
int hexagonrpc(const struct hrpc_method_def_interp4 *def,
	       int fd, uint32_t handle, ...)
{
	const struct step *step;
	uint32_t ret_result, ret_outbufs_len, inbufs_size;
	uint32_t *rctx, *req_handle, *sc, *inbufs_len;
	const char *ret_outbufs;
	char *inbufs;
	va_list ap;

	if (def == &adsp_listener_init2_def)
		return 0;
	if (def != &adsp_listener_next2_def)
		fail("unexpected remote call");
	if (steps >= HRPC_ARRAY_SIZE(script))
		fail("listener asked for more requests than scripted");

	va_start(ap, handle);
	va_arg(ap, uint32_t);
	ret_result = va_arg(ap, uint32_t);
	ret_outbufs_len = va_arg(ap, uint32_t);
	ret_outbufs = va_arg(ap, const char *);
	rctx = va_arg(ap, uint32_t *);
	req_handle = va_arg(ap, uint32_t *);
	sc = va_arg(ap, uint32_t *);
	inbufs_len = va_arg(ap, uint32_t *);
	inbufs_size = va_arg(ap, uint32_t);
	inbufs = va_arg(ap, char *);
	va_end(ap);

	step = &script[steps];
	if (ret_result != step->expect_result)
		fail("listener replied with an unexpected result");
	if (ret_outbufs_len != step->expect_outbufs_len)
		fail("listener replied with unexpected output buffers");
	if (ret_outbufs_len == 16)
		check_fd_reply(ret_outbufs);

	steps++;
	if (step->last)
		return AEE_EBADSTATE;

	*rctx = steps;
	*req_handle = step->handle;
	*sc = step->sc;
	*inbufs_len = encode_request(inbufs, inbufs_size, step->method);

	return 0;
}

int main(int argc, char **argv)
{
	struct fastrpc_interface *ifaces[2];
	int ret;

	if (argc != 2)
		return 2;
	if (mkdir(argv[1], 0700) && access(argv[1], F_OK))
		return 3;

	ifaces[0] = NULL;
	ifaces[1] = fastrpc_apps_std_init(construct_root_dir(argv[1], "adsp"));
	if (ifaces[1] == NULL)
		fail("could not create the apps_std interface");

	ret = run_fastrpc_listener(-1, HRPC_ARRAY_SIZE(ifaces), ifaces);
	if (steps != HRPC_ARRAY_SIZE(script))
		fail("listener stopped before the tunnel was torn down");
	if (ret == 0)
		fail("a torn-down tunnel was reported as success");

	return 0;
}
