/*
 * Qualcomm sensors registry property shim
 *
 * Copyright (C) 2026 HexagonRPC Contributors
 *
 * This file is part of HexagonRPC.
 *
 * HexagonRPC is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <endian.h>
#include <libhexagonrpc/error.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "interface/sns_registry.h"
#include "listener.h"
#include "sns_registry.h"

#define SNS_PROPERTY_VALUE_MAX 92

static const char property0[] = "persist.vendor.sensors.enable.property";
static const char property1[] = "persist.vendor.sensors.enable.property1";

static int property_is_allowed(const void *name, size_t len)
{
	return (len == sizeof(property0) && !memcmp(name, property0, len))
	    || (len == sizeof(property1) && !memcmp(name, property1, len));
}

static uint32_t sns_registry_get_property(void *data,
					  const struct fastrpc_io_buffer *inbufs,
					  struct fastrpc_io_buffer *outbufs)
{
	const uint32_t *first_in = inbufs[0].p;
	size_t prop_name_len;
	size_t prop_value_len;

	/* Never reflect uninitialized heap contents, including on rejection. */
	memset(outbufs[0].p, 0, outbufs[0].s);

	if (inbufs[0].s != 2 * sizeof(uint32_t))
		return AEE_EBADPARM;

	prop_name_len = le32toh(first_in[0]);
	prop_value_len = le32toh(first_in[1]);

	if (prop_name_len != inbufs[1].s
	 || prop_value_len != outbufs[0].s
	 || prop_name_len == 0
	 || ((const char *) inbufs[1].p)[prop_name_len - 1] != '\0')
		return AEE_EBADPARM;

	if (prop_value_len < SNS_PROPERTY_VALUE_MAX) {
		fprintf(stderr, "sns_registry output is too small: %zu\n",
			prop_value_len);
		return AEE_EBUFFERTOOSMALL;
	}

	if (!property_is_allowed(inbufs[1].p, prop_name_len)) {
		fprintf(stderr, "sns_registry rejected a non-whitelisted property\n");
		return AEE_EUNSUPPORTED;
	}

	/*
	 * Both exact-ROM gates are intentionally unset on GNU/Linux.  Returning
	 * an empty C string preserves that state without exposing a generic
	 * Android property bridge to the sensor DSP.
	 */
	return AEE_SUCCESS;
}

static const struct fastrpc_function_impl sns_registry_procs[] = {
	{
		.def = &sns_registry_get_property_def,
		.expected_sc = SNS_REGISTRY_GET_PROPERTY_SC,
		.strict_sc = true,
		.impl = sns_registry_get_property,
	},
};

const struct fastrpc_interface sns_registry_interface = {
	.name = "sns_registry",
	.n_procs = HRPC_ARRAY_SIZE(sns_registry_procs),
	.procs = sns_registry_procs,
};
