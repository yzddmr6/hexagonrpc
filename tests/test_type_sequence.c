// SPDX-License-Identifier: GPL-3.0-or-later
/* Exercise the actual marshaller against explicit wire buffers. No device I/O.
 * The ioctl substitution follows tests/test_hexagonrpc.c (HexagonRPC Contributors).
 */
#include <libhexagonrpc/hexagonrpc.h>
#include <misc/fastrpc.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #x); exit(1); } } while (0)
struct buffer { size_t size; const void *data; };
struct bytes { uint32_t size; char *data; } __attribute__((packed));
struct record { uint32_t word, size; char *data; } __attribute__((packed));
static const struct buffer *inputs, *outputs;
static unsigned nin, nout, invokes, allocations, fail_at;
static int invoke_error;

void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__wrap_malloc(size_t size)
{
	if (++allocations == fail_at) return NULL;
	return __real_malloc(size);
}
void *__wrap_calloc(size_t count, size_t size)
{
	if (++allocations == fail_at) return NULL;
	return __real_calloc(count, size);
}

#ifdef __GLIBC__
typedef unsigned long ioctl_operation;
#else
typedef int ioctl_operation;
#endif
int ioctl(int fd, ioctl_operation op, ...)
{
	va_list ap;
	va_start(ap, op);
	struct fastrpc_invoke *invoke = (void *)(uintptr_t)va_arg(ap, __u64);
	va_end(ap);
	CHECK(fd == -1 && op == (ioctl_operation)FASTRPC_IOCTL_INVOKE);
	CHECK(invoke->handle == 0);
	CHECK(invoke->sc == REMOTE_SCALARS_MAKE(4, nin, nout));
	struct fastrpc_invoke_args *args = (void *)(uintptr_t)invoke->args;
	for (unsigned i = 0; i < nin; ++i) {
		CHECK(args[i].fd == -1 && args[i].attr == 0);
		CHECK(args[i].length == inputs[i].size);
		if (inputs[i].size) CHECK(!memcmp((void *)(uintptr_t)args[i].ptr, inputs[i].data, inputs[i].size));
	}
	for (unsigned i = 0; i < nout; ++i) {
		CHECK(args[nin + i].fd == -1 && args[nin + i].attr == 0);
		CHECK(args[nin + i].length == outputs[i].size);
		if (!invoke_error && outputs[i].size)
			memcpy((void *)(uintptr_t)args[nin + i].ptr, outputs[i].data, outputs[i].size);
	}
	invokes++;
	return invoke_error;
}

static const struct hrpc_arg_def_interp4 bytes_fields[] = {{HRPC_ARG_BLOB_SEQ, 1}};
static const struct hrpc_arg_def_interp4 word_fields[] = {{HRPC_ARG_BLOB, 4}};
static const struct hrpc_arg_def_interp4 wide_fields[] = {{HRPC_ARG_BLOB, 8}};
static const struct hrpc_arg_def_interp4 record_fields[] = {{HRPC_ARG_BLOB, 4}, {HRPC_ARG_BLOB_SEQ, 1}};
static const struct hrpc_inner_type_def_interp4 types[] = {
	{1, bytes_fields}, {1, word_fields}, {1, wide_fields}, {2, record_fields},
};
#define METHOD(name, ...) \
	const struct hrpc_arg_def_interp4 name##_args[] = {__VA_ARGS__}; \
	const struct hrpc_method_def_interp4 name = {4, HRPC_ARRAY_SIZE(name##_args), name##_args, HRPC_ARRAY_SIZE(types), types}
#define EXPECT(in, out) do { inputs = in; nin = HRPC_ARRAY_SIZE(in); outputs = out; nout = HRPC_ARRAY_SIZE(out); invokes = 0; } while (0)
#define EXPECT_IN(in) do { inputs = in; nin = HRPC_ARRAY_SIZE(in); outputs = NULL; nout = 0; invokes = 0; } while (0)
static const uint32_t words[] = {0x11223344, 0xaabbccdd};

static void input_first(void)
{
	METHOD(def, {HRPC_ARG_TYPE_SEQ, 1}, {HRPC_ARG_BLOB_SEQ, 1});
	const uint32_t prim[] = {2, 1};
	const struct buffer in[] = {{sizeof(prim), prim}, {sizeof(words), words}, {1, "!"}};
	EXPECT_IN(in);
	CHECK(!hexagonrpc(&def, -1, 0, (uint32_t)2, (const void *)words, (uint32_t)1, (const void *)"!"));
	CHECK(invokes == 1);
}
static void input_after_other(void)
{
	METHOD(def, {HRPC_ARG_TYPE, 2}, {HRPC_ARG_TYPE_SEQ, 1});
	const uint64_t wide = 0x0123456789abcdef;
	const uint32_t prim[] = {0x89abcdef, 0x01234567, 2};
	const struct buffer in[] = {{sizeof(prim), prim}, {sizeof(words), words}};
	EXPECT_IN(in);
	CHECK(!hexagonrpc(&def, -1, 0, (const void *)&wide, (uint32_t)2, (const void *)words));
	CHECK(invokes == 1);
}
static void input_two(void)
{
	METHOD(def, {HRPC_ARG_TYPE_SEQ, 0}, {HRPC_ARG_TYPE_SEQ, 1});
	const struct bytes nested[] = {{2, "hi"}, {3, "bye"}};
	const uint32_t prim[] = {2, 2}, lengths[] = {2, 3};
	const struct buffer in[] = {{sizeof(prim), prim}, {sizeof(lengths), lengths}, {2, "hi"}, {3, "bye"}, {sizeof(words), words}};
	EXPECT_IN(in);
	CHECK(!hexagonrpc(&def, -1, 0, (uint32_t)2, (const void *)nested, (uint32_t)2, (const void *)words));
	CHECK(invokes == 1);
}
static void input_empty(void)
{
	METHOD(def, {HRPC_ARG_TYPE_SEQ, 1}, {HRPC_ARG_BLOB_SEQ, 1});
	const uint32_t prim[] = {0, 1};
	const struct buffer in[] = {{sizeof(prim), prim}, {0, NULL}, {1, "!"}};
	EXPECT_IN(in);
	CHECK(!hexagonrpc(&def, -1, 0, (uint32_t)0, NULL, (uint32_t)1, (const void *)"!"));
	CHECK(invokes == 1);
}
static void output_plain(void)
{
	METHOD(def, {HRPC_ARG_OUT_TYPE_SEQ, 1}, {HRPC_ARG_OUT_BLOB_SEQ, 1});
	const uint32_t prim[] = {2, 1};
	const struct buffer in[] = {{sizeof(prim), prim}, {0, NULL}};
	const struct buffer out[] = {{sizeof(words), words}, {1, "!"}};
	uint32_t got[2] = {0}; char tail = 0;
	EXPECT(in, out);
	CHECK(!hexagonrpc(&def, -1, 0, (uint32_t)2, (void *)got, (uint32_t)1, (void *)&tail));
	CHECK(invokes == 1 && !memcmp(got, words, sizeof(words)) && tail == '!');
}
static void output_nested(void)
{
	METHOD(def, {HRPC_ARG_OUT_TYPE_SEQ, 3});
	const uint32_t prim[] = {2}, lengths[] = {2, 3};
	const struct buffer in[] = {{sizeof(prim), prim}, {sizeof(lengths), lengths}};
	const struct buffer out[] = {{sizeof(words), words}, {2, "hi"}, {3, "bye"}};
	char a[2] = {0}, b[3] = {0};
	struct record got[] = {{0, 2, a}, {0, 3, b}};
	EXPECT(in, out);
	allocations = 0;
	int ret = hexagonrpc(&def, -1, 0, (uint32_t)2, (void *)got);
	if (fail_at || invoke_error) {
		CHECK(ret < 0);
		CHECK(invokes == (fail_at ? 0u : 1u));
		CHECK(got[0].word == 0 && got[1].word == 0);
	} else {
		CHECK(ret == 0 && invokes == 1);
		CHECK(got[0].word == words[0] && got[1].word == words[1]);
		CHECK(!memcmp(a, "hi", 2) && !memcmp(b, "bye", 3));
	}
}
static void output_two(void)
{
	METHOD(def, {HRPC_ARG_OUT_TYPE_SEQ, 1}, {HRPC_ARG_OUT_TYPE_SEQ, 3});
	const uint32_t prim[] = {2, 1}, length = 2, word = 42;
	const struct buffer in[] = {{sizeof(prim), prim}, {0, NULL}, {sizeof(length), &length}};
	const struct buffer out[] = {{sizeof(words), words}, {sizeof(word), &word}, {2, "hi"}};
	uint32_t got[2] = {0}; char data[2] = {0}; struct record nested = {0, 2, data};
	EXPECT(in, out);
	CHECK(!hexagonrpc(&def, -1, 0, (uint32_t)2, (void *)got, (uint32_t)1, (void *)&nested));
	CHECK(invokes == 1 && !memcmp(got, words, sizeof(words)) && nested.word == word && !memcmp(data, "hi", 2));
}
static void output_empty(void)
{
	METHOD(def, {HRPC_ARG_OUT_TYPE_SEQ, 1}, {HRPC_ARG_OUT_BLOB_SEQ, 1});
	const uint32_t prim[] = {0, 1};
	const struct buffer in[] = {{sizeof(prim), prim}, {0, NULL}};
	const struct buffer out[] = {{0, NULL}, {1, "!"}};
	char got = 0;
	EXPECT(in, out);
	CHECK(!hexagonrpc(&def, -1, 0, (uint32_t)0, NULL, (uint32_t)1, (void *)&got));
	CHECK(invokes == 1 && got == '!');
}
static void in_out(void)
{
	METHOD(def, {HRPC_ARG_TYPE_SEQ, 1}, {HRPC_ARG_OUT_TYPE_SEQ, 1});
	const uint32_t prim[] = {2, 2};
	const struct buffer in[] = {{sizeof(prim), prim}, {sizeof(words), words}, {0, NULL}};
	const struct buffer out[] = {{sizeof(words), words}};
	uint32_t got[2] = {0};
	EXPECT(in, out);
	CHECK(!hexagonrpc(&def, -1, 0, (uint32_t)2, (const void *)words, (uint32_t)2, (void *)got));
	CHECK(invokes == 1 && !memcmp(got, words, sizeof(words)));
}
static void ioctl_failure(void) { invoke_error = -EIO; output_nested(); }
static void allocation_failure(void)
{
	output_nested();
	unsigned total = allocations;
	for (fail_at = 1; fail_at <= total; ++fail_at) output_nested();
	printf("allocation failure points checked: %u\n", total);
	fail_at = 0;
}
int main(int argc, char **argv)
{
	const struct { const char *name; void (*run)(void); } cases[] = {
		{"input-first", input_first}, {"input-after-other", input_after_other},
		{"input-two", input_two}, {"input-empty", input_empty},
		{"output-plain", output_plain}, {"output-nested", output_nested},
		{"output-two", output_two}, {"output-empty", output_empty},
		{"in-out", in_out}, {"ioctl-failure", ioctl_failure}, {"allocation-failure", allocation_failure},
	};
	CHECK(argc == 2);
	for (unsigned i = 0; i < HRPC_ARRAY_SIZE(cases); ++i) {
		if (!strcmp(argv[1], cases[i].name)) {
			cases[i].run(); puts("PASS"); return 0;
		}
	}
	return 2;
}
