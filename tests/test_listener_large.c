/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Reuse the existing real apps_std/HexagonFS fixture, including its helpers. */
#define main registry_fixture_main
#include "test_registry_io.c"
#undef main

#include <endian.h>
#include <misc/fastrpc.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include "../hexagonrpcd/interface/adsp_listener.h"

#ifdef __GLIBC__
typedef unsigned long ioctl_operation;
#else
typedef int ioctl_operation;
#endif

static unsigned char *packet, *payload;
static size_t packet_len, payload_len;
static const char *scenario;
static unsigned int next_calls, fetch_calls;
static bool reply_seen;
static const uint32_t request_context = 0x12345678;

static void word(void *where, uint32_t value)
{
	value = htole32(value);
	memcpy(where, &value, sizeof(value));
}

static uint32_t get_word(const void *where)
{
	uint32_t value;
	memcpy(&value, where, sizeof(value));
	return le32toh(value);
}

/* Only the DSP/kernel boundary is substituted. Production hexagonrpc builds
 * the invoke scalars and args, and the real listener decodes and dispatches. */
int ioctl(int fd, ioctl_operation op, ...)
{
	struct fastrpc_invoke *invoke;
	struct fastrpc_invoke_args *args;
	uint32_t *in;
	va_list ap;
	CHECK(fd == 123 && op == FASTRPC_IOCTL_INVOKE);
	va_start(ap, op);
	invoke = (void *)(uintptr_t)va_arg(ap, __u64);
	va_end(ap);
	CHECK(invoke->handle == ADSP_LISTENER);
	args = (void *)(uintptr_t)invoke->args;
	unsigned int count = REMOTE_SCALARS_INBUFS(invoke->sc) + REMOTE_SCALARS_OUTBUFS(invoke->sc);
	for (unsigned int i = 0; i < count; i++) {
		/* Kernel fastrpc_get_args handles these as copied DMA-backed args. */
		CHECK(args[i].fd == -1 && args[i].attr == 0);
	}
	if (invoke->sc == REMOTE_SCALARS_MAKE(3, 0, 0))
		return 0;
	in = (void *)(uintptr_t)args[0].ptr;
	if (invoke->sc == REMOTE_SCALARS_MAKE(4, 2, 2)) {
		CHECK(args[0].length == 16 && args[2].length == 16 && args[3].length == 256);
		CHECK(in[3] == 256 && in[2] == args[1].length);
		if (next_calls++) {
			const unsigned char *encoded = (void *)(uintptr_t)args[1].ptr;
			CHECK(in[0] == request_context);
			if (!strcmp(scenario, "unsupported")) {
				CHECK(in[1] == AEE_EUNSUPPORTED);
				CHECK(args[1].length == 4 && get_word(encoded) == 0);
			} else {
				CHECK(in[1] == AEE_SUCCESS);
				CHECK(args[1].length == 16 && get_word(encoded) == 8);
				CHECK(get_word(encoded + 8) == payload_len && get_word(encoded + 12) == 0);
			}
			reply_seen = true;
			errno = ECANCELED; /* End this one-request simulated session. */
			return -1;
		}
		CHECK(in[0] == 0 && in[1] == UINT32_MAX && args[1].length == 0);
		uint32_t *out = (void *)(uintptr_t)args[2].ptr;
		out[0] = request_context;
		out[1] = 0; /* The real apps_std interface in our one-entry array. */
		out[2] = REMOTE_SCALARS_MAKE(!strcmp(scenario, "unsupported") ? 18 : 5, 2, 1);
		out[3] = !strcmp(scenario, "oversize") ? (16 * 1024 * 1024 + 1) : packet_len;
		memcpy((void *)(uintptr_t)args[3].ptr, packet, packet_len < 256 ? packet_len : 256);
		return 0;
	}
	CHECK(invoke->sc == REMOTE_SCALARS_MAKE(5, 1, 2));
	fetch_calls++;
	CHECK(args[0].length == 12 && args[1].length == 4);
	CHECK(in[0] == request_context && in[1] == 256);
	CHECK(in[2] == packet_len - 256 && args[2].length == packet_len - 256);
	if (!strcmp(scenario, "fetch-error")) {
		errno = EIO;
		return -1;
	}
	*(uint32_t *)(uintptr_t)args[1].ptr = packet_len + (!strcmp(scenario, "changed-length") ? 1 : 0);
	memcpy((void *)(uintptr_t)args[2].ptr, packet + 256, packet_len - 256);
	return 0;
}

int main(int argc, char **argv)
{
	char template[] = "/tmp/hexagonrpc-large-XXXXXX";
	char *root, *scratch, *path;
	struct fastrpc_interface *iface;
	uint32_t file;
	CHECK(argc == 3);
	payload_len = strtoul(argv[1], NULL, 10);
	CHECK(payload_len > 0 && payload_len <= 65536);
	scenario = argv[2];
	root = mkdtemp(template); CHECK(root);
	scratch = join(root, "scratch"); CHECK(!mkdir(scratch, 0700));
	host_write(root, "pristine", "unchanged");
	iface = fastrpc_apps_std_init(construct_root_dir(root, "adsp", scratch)); CHECK(iface);
	file = rpc_open(iface, REG_PARENT "fstempfile", "w"); CHECK((int)file >= 0);
	payload = malloc(payload_len); CHECK(payload);
	for (size_t i = 0; i < payload_len; i++)
		payload[i] = (unsigned char)(i * 37 + i / 256);
	/* Two encoded input buffers: 8-byte primitive (fd,length) and the data.
	 * Each length is LE32; nonempty data starts at the next 8-byte boundary. */
	packet_len = 24 + payload_len;
	packet = calloc(1, packet_len); CHECK(packet);
	word(packet, 8); word(packet + 8, file);
	word(packet + 12, payload_len);
	word(packet + 16, payload_len + (!strcmp(scenario, "truncated") ? 1 : 0));
	memcpy(packet + 24, payload, payload_len);
	CHECK(run_fastrpc_listener(123, 1, &iface) != 0);
	bool success = !strcmp(scenario, "success");
	CHECK(reply_seen == (success || !strcmp(scenario, "unsupported")));
	CHECK(fetch_calls == (packet_len > 256 && strcmp(scenario, "oversize") ? 1 : 0));
	rpc_close(iface, file);
	path = join(scratch, "fstempfile");
	int fd = open(path, O_RDONLY); CHECK(fd >= 0);
	struct stat st; CHECK(!fstat(fd, &st));
	CHECK(st.st_size == (success ? (off_t)payload_len : 0));
	if (success) {
		unsigned char *readback = malloc(payload_len); CHECK(readback);
		CHECK(read(fd, readback, payload_len) == (ssize_t)payload_len);
		CHECK(!memcmp(payload, readback, payload_len));
		free(readback);
	}
	close(fd);
	compare_text(root, "pristine", "unchanged");
	CHECK(rpc_open(iface, "/vendor/etc/sensors/sns_reg_config", "w") < 0);
	fastrpc_apps_std_deinit(iface);
	printf("LISTENER_LARGE_PASS bytes=%zu case=%s fetch_calls=%u actual_file_bytes=%jd\n",
	       payload_len, scenario, fetch_calls, (intmax_t)st.st_size);
	free(path); free(scratch); free(packet); free(payload);
	return 0;
}
