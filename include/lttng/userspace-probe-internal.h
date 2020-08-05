/*
 * Copyright (C) 2017 Jérémie Galarneau <jeremie.galarneau@efficios.com>
 * Copyright (C) 2018 Francis Deslauriers <francis.deslauriers@efficios.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 */

#ifndef LTTNG_USERSPACE_PROBE_INTERNAL_H
#define LTTNG_USERSPACE_PROBE_INTERNAL_H

#include <lttng/userspace-probe.h>
#include <common/macros.h>
#include <common/fd-handle.h>
#include <stdbool.h>

struct lttng_payload;
struct lttng_payload_view;
struct lttng_dynamic_buffer;

typedef bool (*userspace_probe_location_equal_cb)(
		const struct lttng_userspace_probe_location *a,
		const struct lttng_userspace_probe_location *b);

/*
 * No elf-specific comm structure is defined since no elf-specific payload is
 * currently needed.
 */
struct lttng_userspace_probe_location_lookup_method_comm {
	/* enum lttng_userspace_probe_location_lookup_method_type */
	int8_t type;
	/* type-specific payload */
	char payload[];
};

/* Common ancestor of all userspace probe location lookup methods. */
struct lttng_userspace_probe_location_lookup_method {
	enum lttng_userspace_probe_location_lookup_method_type type;
};

struct lttng_userspace_probe_location_lookup_method_elf {
	struct lttng_userspace_probe_location_lookup_method parent;
};

struct lttng_userspace_probe_location_lookup_method_sdt {
	struct lttng_userspace_probe_location_lookup_method parent;
};

struct lttng_userspace_probe_location_comm {
	/* enum lttng_userspace_probe_location_type */
	int8_t type;
	/*
	 * Payload is composed of, in that order,
	 *   - type-specific payload
	 *   - struct lttng_userspace_probe_location_lookup_method_comm
	 */
	char payload[];
};

struct lttng_userspace_probe_location_function_comm {
	/* Both lengths include the trailing \0. */
	uint32_t function_name_len;
	uint32_t binary_path_len;
	/*
	 * Payload is composed of, in that order,
	 *   - function name (with trailing \0),
	 *   - absolute binary path (with trailing \0)
	 */
	char payload[];
} LTTNG_PACKED;

struct lttng_userspace_probe_location_tracepoint_comm {
	/* The three lengths include the trailing \0. */
	uint32_t probe_name_len;
	uint32_t provider_name_len;
	uint32_t binary_path_len;
	/*
	 * Payload is composed of, in that order,
	 *   - probe name (with trailing \0),
	 *   - provider name (with trailing \0),
	 *   - absolute binary path (with trailing \0)
	 */
	char payload[];
} LTTNG_PACKED;

/* Common ancestor of all userspace probe locations. */
struct lttng_userspace_probe_location {
	enum lttng_userspace_probe_location_type type;
	struct lttng_userspace_probe_location_lookup_method *lookup_method;
	userspace_probe_location_equal_cb equal;
};

struct lttng_userspace_probe_location_function {
	struct lttng_userspace_probe_location parent;
	char *function_name;
	char *binary_path;
	/*
	 * binary_fd is a file descriptor to the executable file. It's open
	 * early on to keep the backing inode valid over the course of the
	 * intrumentation and use. It prevents deletion and reuse races.
	 */
	struct fd_handle *binary_fd_handle;
	enum lttng_userspace_probe_location_function_instrumentation_type instrumentation_type;
};

struct lttng_userspace_probe_location_tracepoint {
	struct lttng_userspace_probe_location parent;
	char *probe_name;
	char *provider_name;
	char *binary_path;
	/*
	 * binary_fd is a file descriptor to the executable file. It's open
	 * early on to keep the backing inode valid over the course of the
	 * intrumentation and use. It prevents deletion and reuse races.
	 */
	struct fd_handle *binary_fd_handle;
};

LTTNG_HIDDEN
int lttng_userspace_probe_location_serialize(
		const struct lttng_userspace_probe_location *location,
		struct lttng_payload *payload);

LTTNG_HIDDEN
int lttng_userspace_probe_location_create_from_payload(
		struct lttng_payload_view *view,
		struct lttng_userspace_probe_location **probe_location);

/*
 * Returns a version of the location that is serialized to a contiguous region
 * of memory. Pass NULL to buffer to only get the storage requirement of the
 * flattened userspace probe location.
 */
LTTNG_HIDDEN
int lttng_userspace_probe_location_flatten(
		const struct lttng_userspace_probe_location *location,
		struct lttng_dynamic_buffer *buffer);

LTTNG_HIDDEN
struct lttng_userspace_probe_location *lttng_userspace_probe_location_copy(
		const struct lttng_userspace_probe_location *location);

LTTNG_HIDDEN
bool lttng_userspace_probe_location_lookup_method_is_equal(
		const struct lttng_userspace_probe_location_lookup_method *a,
		const struct lttng_userspace_probe_location_lookup_method *b);

LTTNG_HIDDEN
bool lttng_userspace_probe_location_is_equal(
		const struct lttng_userspace_probe_location *a,
		const struct lttng_userspace_probe_location *b);

#endif /* LTTNG_USERSPACE_PROBE_INTERNAL_H */
