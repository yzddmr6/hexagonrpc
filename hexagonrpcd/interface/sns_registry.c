/*
 * Qualcomm sensors registry property interface - method definitions
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

#include <libhexagonrpc/hexagonrpc.h>

#include "sns_registry.h"

/*
 * Exact QAIC wire shape recovered from the liuqin ROM skel:
 *
 *   inbuf[0] = { uint32_t prop_name_len, uint32_t prop_value_len }
 *   inbuf[1] = NUL-terminated property name
 *   outbuf[0] = property value, prop_value_len bytes
 */
static const struct hrpc_arg_def_interp4 sns_registry_get_property_args[] = {
	{ HRPC_ARG_BLOB_SEQ, sizeof(char) },
	{ HRPC_ARG_OUT_BLOB_SEQ, sizeof(char) },
};

const struct hrpc_method_def_interp4 sns_registry_get_property_def = {
	.msg_id = 0,
	.n_args = HRPC_ARRAY_SIZE(sns_registry_get_property_args),
	.args = sns_registry_get_property_args,
	.n_inner_types = 0,
	.inner_types = NULL,
};
