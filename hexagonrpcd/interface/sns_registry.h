/*
 * Qualcomm sensors registry property interface - API
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

#ifndef INTERFACE_SNS_REGISTRY_H
#define INTERFACE_SNS_REGISTRY_H

#include <libhexagonrpc/hexagonrpc.h>

#define SNS_REGISTRY_GET_PROPERTY_SC REMOTE_SCALARS_MAKE(0, 2, 1)

extern const struct hrpc_method_def_interp4 sns_registry_get_property_def;

#endif /* INTERFACE_SNS_REGISTRY_H */
