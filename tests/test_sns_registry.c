#include <endian.h>
#include <libhexagonrpc/error.h>
#include <libhexagonrpc/hexagonrpc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hexagonrpcd/interface/sns_registry.h"
#include "../hexagonrpcd/iobuffer.h"
#include "../hexagonrpcd/listener.h"
#include "../hexagonrpcd/sns_registry.h"

#define PROPERTY_VALUE_MAX 92

static const char property0[] = "persist.vendor.sensors.enable.property";
static const char property1[] = "persist.vendor.sensors.enable.property1";

static void fail(const char *message)
{
	fprintf(stderr, "test_sns_registry: %s\n", message);
	exit(1);
}

/* listener.c references these inline client calls, but dispatch-only tests
 * must never issue a host FastRPC ioctl. */
int hexagonrpc(const struct hrpc_method_def_interp4 *def,
	       int fd, uint32_t handle, ...)
{
	fail("dispatch-only test attempted a FastRPC ioctl");
	return -1;
}

static size_t encode_input_wire(void *dest, size_t capacity,
				const char *name, uint32_t value_len)
{
	uint32_t name_len = strlen(name) + 1;
	uint32_t primary[2] = { htole32(name_len), htole32(value_len) };
	uint32_t wire_len;
	char *wire = dest;
	size_t off = 0;

	if (capacity < 24 + name_len)
		fail("input wire buffer is too small");

	wire_len = htole32(sizeof(primary));
	memcpy(&wire[off], &wire_len, sizeof(wire_len));
	off += sizeof(wire_len);
	memset(&wire[off], 0, 4);
	off += 4;
	memcpy(&wire[off], primary, sizeof(primary));
	off += sizeof(primary);

	wire_len = htole32(name_len);
	memcpy(&wire[off], &wire_len, sizeof(wire_len));
	off += sizeof(wire_len);
	memset(&wire[off], 0, 4);
	off += 4;
	memcpy(&wire[off], name, name_len);
	off += name_len;

	return off;
}

static struct fastrpc_io_buffer *decode_input_wire(uint32_t sc,
						    const void *wire,
						    size_t wire_len)
{
	struct fastrpc_decoder_context *ctx;
	struct fastrpc_io_buffer *decoded;

	ctx = inbuf_decode_start(sc);
	if (ctx == NULL)
		fail("could not allocate decoder");
	if (inbuf_decode(ctx, wire_len, wire))
		fail("could not decode ROM-format request");
	if (!inbuf_decode_is_complete(ctx))
		fail("ROM-format request is incomplete");
	decoded = inbuf_decode_finish(ctx);
	if (decoded == NULL)
		fail("decoder returned no buffers");

	return decoded;
}

static uint32_t invoke_property(const char *name, uint32_t value_len,
				uint32_t sc, int *dispatch_ret,
				struct fastrpc_io_buffer **returned_out)
{
	struct fastrpc_interface *ifaces[] = {
		(struct fastrpc_interface *) &sns_registry_interface,
	};
	struct fastrpc_io_buffer *decoded;
	struct fastrpc_io_buffer *returned = NULL;
	unsigned char wire[256] = { 0 };
	uint32_t result = UINT32_MAX;
	size_t wire_len;

	wire_len = encode_input_wire(wire, sizeof(wire), name, value_len);
	decoded = decode_input_wire(sc, wire, wire_len);
	*dispatch_ret = fastrpc_invoke_requested_procedure(
		HRPC_ARRAY_SIZE(ifaces), ifaces, 0, sc, &result,
		decoded, &returned);
	iobuf_free(REMOTE_SCALARS_INBUFS(sc), decoded);
	*returned_out = returned;

	return result;
}

static void assert_zero_output(const struct fastrpc_io_buffer *returned,
				       uint32_t expected_len)
{
	const unsigned char *value;
	size_t i;

	if (returned == NULL || returned[0].s != expected_len)
		fail("unexpected decoded output size");
	value = returned[0].p;
	for (i = 0; i < returned[0].s; i++) {
		if (value[i] != 0)
			fail("property shim leaked a non-empty output byte");
	}
}

static void test_allowed_property(const char *name, uint32_t value_len)
{
	struct fastrpc_io_buffer *returned;
	unsigned char encoded[256] = { 0 };
	uint32_t encoded_len;
	uint32_t result;
	int dispatch_ret;
	size_t wire_size;
	size_t i;

	result = invoke_property(name, value_len,
				 SNS_REGISTRY_GET_PROPERTY_SC,
				 &dispatch_ret, &returned);
	if (dispatch_ret || result != AEE_SUCCESS)
		fail("allowed property did not succeed");
	assert_zero_output(returned, value_len);

	wire_size = outbufs_calculate_size(1, returned);
	if (wire_size != 8 + value_len || wire_size > sizeof(encoded))
		fail("unexpected encoded output wire size");
	outbufs_encode(1, returned, encoded);
	memcpy(&encoded_len, encoded, sizeof(encoded_len));
	if (le32toh(encoded_len) != value_len)
		fail("encoded output length does not match ROM ABI");
	for (i = 4; i < wire_size; i++) {
		if (encoded[i] != 0)
			fail("encoded empty string contains non-zero bytes");
	}

	iobuf_free(1, returned);
}

static void test_rejected_property(void)
{
	struct fastrpc_io_buffer *returned;
	uint32_t result;
	int dispatch_ret;

	result = invoke_property("persist.vendor.sensors.enable.property.extra",
				 PROPERTY_VALUE_MAX,
				 SNS_REGISTRY_GET_PROPERTY_SC,
				 &dispatch_ret, &returned);
	if (dispatch_ret || result != AEE_EUNSUPPORTED)
		fail("non-whitelisted property was not rejected");
	assert_zero_output(returned, PROPERTY_VALUE_MAX);
	iobuf_free(1, returned);
}

static void test_short_output(void)
{
	struct fastrpc_io_buffer *returned;
	uint32_t result;
	int dispatch_ret;

	result = invoke_property(property0, PROPERTY_VALUE_MAX - 1,
				 SNS_REGISTRY_GET_PROPERTY_SC,
				 &dispatch_ret, &returned);
	if (dispatch_ret || result != AEE_EBUFFERTOOSMALL)
		fail("sub-92-byte output was not rejected");
	assert_zero_output(returned, PROPERTY_VALUE_MAX - 1);
	iobuf_free(1, returned);
}

static void test_rejected_method_and_shape(void)
{
	struct fastrpc_io_buffer *returned;
	uint32_t result;
	uint32_t sc;
	int dispatch_ret;

	sc = REMOTE_SCALARS_MAKE(1, 2, 1);
	result = invoke_property(property0, PROPERTY_VALUE_MAX, sc,
				 &dispatch_ret, &returned);
	if (!dispatch_ret || result != AEE_EUNSUPPORTED || returned != NULL)
		fail("method 1 was not rejected by the one-method interface");

	sc = REMOTE_SCALARS_MAKE(0, 2, 2);
	result = invoke_property(property0, PROPERTY_VALUE_MAX, sc,
				 &dispatch_ret, &returned);
	if (!dispatch_ret || result != AEE_EBADPARM || returned != NULL)
		fail("non-ROM scalar shape was not rejected");
}

int main(void)
{
	if (sns_registry_interface.n_procs != 1
	 || sns_registry_get_property_def.msg_id != 0
	 || SNS_REGISTRY_GET_PROPERTY_SC != REMOTE_SCALARS_MAKE(0, 2, 1))
		fail("interface is not the exact one-method ROM ABI");

	test_allowed_property(property0, PROPERTY_VALUE_MAX);
	test_allowed_property(property1, 128);
	test_rejected_property();
	test_short_output();
	test_rejected_method_and_shape();

	return 0;
}
