/*
 * Copyright (C) 2017 Jérémie Galarneau <jeremie.galarneau@efficios.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 */

#include <assert.h>
#include <common/error.h>
#include <lttng/action/action-internal.h>
#include <lttng/action/group-internal.h>
#include <lttng/action/notify-internal.h>
#include <lttng/action/rate-policy-internal.h>
#include <lttng/action/rotate-session-internal.h>
#include <lttng/action/snapshot-session-internal.h>
#include <lttng/action/start-session-internal.h>
#include <lttng/action/stop-session-internal.h>
#include <lttng/error-query-internal.h>

LTTNG_HIDDEN
const char *lttng_action_type_string(enum lttng_action_type action_type)
{
	switch (action_type) {
	case LTTNG_ACTION_TYPE_UNKNOWN:
		return "UNKNOWN";
	case LTTNG_ACTION_TYPE_GROUP:
		return "GROUP";
	case LTTNG_ACTION_TYPE_NOTIFY:
		return "NOTIFY";
	case LTTNG_ACTION_TYPE_ROTATE_SESSION:
		return "ROTATE_SESSION";
	case LTTNG_ACTION_TYPE_SNAPSHOT_SESSION:
		return "SNAPSHOT_SESSION";
	case LTTNG_ACTION_TYPE_START_SESSION:
		return "START_SESSION";
	case LTTNG_ACTION_TYPE_STOP_SESSION:
		return "STOP_SESSION";
	default:
		return "???";
	}
}

enum lttng_action_type lttng_action_get_type(const struct lttng_action *action)
{
	return action ? action->type : LTTNG_ACTION_TYPE_UNKNOWN;
}

LTTNG_HIDDEN
void lttng_action_init(struct lttng_action *action,
		enum lttng_action_type type,
		action_validate_cb validate,
		action_serialize_cb serialize,
		action_equal_cb equal,
		action_destroy_cb destroy,
		action_get_rate_policy_cb get_rate_policy,
		action_add_error_query_results_cb add_error_query_results)
{
	urcu_ref_init(&action->ref);
	action->type = type;
	action->validate = validate;
	action->serialize = serialize;
	action->equal = equal;
	action->destroy = destroy;
	action->get_rate_policy = get_rate_policy;
	action->add_error_query_results = add_error_query_results;

	action->execution_request_counter = 0;
	action->execution_counter = 0;
	action->execution_failure_counter = 0;
}

static
void action_destroy_ref(struct urcu_ref *ref)
{
	struct lttng_action *action =
			container_of(ref, struct lttng_action, ref);

	action->destroy(action);
}

LTTNG_HIDDEN
void lttng_action_get(struct lttng_action *action)
{
	urcu_ref_get(&action->ref);
}

LTTNG_HIDDEN
void lttng_action_put(struct lttng_action *action)
{
	if (!action) {
		return;
	}

	assert(action->destroy);
	urcu_ref_put(&action->ref, action_destroy_ref);
}

void lttng_action_destroy(struct lttng_action *action)
{
	lttng_action_put(action);
}

LTTNG_HIDDEN
bool lttng_action_validate(struct lttng_action *action)
{
	bool valid;

	if (!action) {
		valid = false;
		goto end;
	}

	if (!action->validate) {
		/* Sub-class guarantees that it can never be invalid. */
		valid = true;
		goto end;
	}

	valid = action->validate(action);
end:
	return valid;
}

LTTNG_HIDDEN
int lttng_action_serialize(struct lttng_action *action,
		struct lttng_payload *payload)
{
	int ret;
	struct lttng_action_comm action_comm = {
		.action_type = (int8_t) action->type,
	};

	ret = lttng_dynamic_buffer_append(&payload->buffer, &action_comm,
			sizeof(action_comm));
	if (ret) {
		goto end;
	}

	ret = action->serialize(action, payload);
	if (ret) {
		goto end;
	}
end:
	return ret;
}

LTTNG_HIDDEN
ssize_t lttng_action_create_from_payload(struct lttng_payload_view *view,
		struct lttng_action **action)
{
	ssize_t consumed_len, specific_action_consumed_len;
	action_create_from_payload_cb create_from_payload_cb;
	const struct lttng_action_comm *action_comm;
	const struct lttng_payload_view action_comm_view =
			lttng_payload_view_from_view(
					view, 0, sizeof(*action_comm));

	if (!view || !action) {
		consumed_len = -1;
		goto end;
	}

	if (!lttng_payload_view_is_valid(&action_comm_view)) {
		/* Payload not large enough to contain the header. */
		consumed_len = -1;
		goto end;
	}

	action_comm = (const struct lttng_action_comm *) action_comm_view.buffer.data;

	DBG("Create action from payload: action-type=%s",
			lttng_action_type_string(action_comm->action_type));

	switch (action_comm->action_type) {
	case LTTNG_ACTION_TYPE_NOTIFY:
		create_from_payload_cb = lttng_action_notify_create_from_payload;
		break;
	case LTTNG_ACTION_TYPE_ROTATE_SESSION:
		create_from_payload_cb =
				lttng_action_rotate_session_create_from_payload;
		break;
	case LTTNG_ACTION_TYPE_SNAPSHOT_SESSION:
		create_from_payload_cb =
				lttng_action_snapshot_session_create_from_payload;
		break;
	case LTTNG_ACTION_TYPE_START_SESSION:
		create_from_payload_cb =
				lttng_action_start_session_create_from_payload;
		break;
	case LTTNG_ACTION_TYPE_STOP_SESSION:
		create_from_payload_cb =
				lttng_action_stop_session_create_from_payload;
		break;
	case LTTNG_ACTION_TYPE_GROUP:
		create_from_payload_cb = lttng_action_group_create_from_payload;
		break;
	default:
		ERR("Failed to create action from payload, unhandled action type: action-type=%u (%s)",
				action_comm->action_type,
				lttng_action_type_string(
						action_comm->action_type));
		consumed_len = -1;
		goto end;
	}

	{
		/* Create buffer view for the action-type-specific data. */
		struct lttng_payload_view specific_action_view =
				lttng_payload_view_from_view(view,
						sizeof(struct lttng_action_comm),
						-1);

		specific_action_consumed_len = create_from_payload_cb(
				&specific_action_view, action);
	}
	if (specific_action_consumed_len < 0) {
		ERR("Failed to create specific action from buffer.");
		consumed_len = -1;
		goto end;
	}

	assert(*action);

	consumed_len = sizeof(struct lttng_action_comm) +
		       specific_action_consumed_len;

end:
	return consumed_len;
}

LTTNG_HIDDEN
bool lttng_action_is_equal(const struct lttng_action *a,
		const struct lttng_action *b)
{
	bool is_equal = false;

	if (!a || !b) {
		goto end;
	}

	if (a->type != b->type) {
		goto end;
	}

	if (a == b) {
		is_equal = true;
		goto end;
	}

	assert(a->equal);
	is_equal = a->equal(a, b);
end:
	return is_equal;
}

LTTNG_HIDDEN
void lttng_action_increase_execution_request_count(struct lttng_action *action)
{
	action->execution_request_counter++;
}

LTTNG_HIDDEN
void lttng_action_increase_execution_count(struct lttng_action *action)
{
	action->execution_counter++;
}

LTTNG_HIDDEN
void lttng_action_increase_execution_failure_count(struct lttng_action *action)
{
	uatomic_inc(&action->execution_failure_counter);
}

LTTNG_HIDDEN
bool lttng_action_should_execute(const struct lttng_action *action)
{
	const struct lttng_rate_policy *policy = NULL;
	bool execute = false;

	if (action->get_rate_policy == NULL) {
		execute = true;
		goto end;
	}

	policy = action->get_rate_policy(action);
	if (policy == NULL) {
		execute = true;
		goto end;
	}

	execute = lttng_rate_policy_should_execute(
			policy, action->execution_request_counter);
end:
	return execute;
}

LTTNG_HIDDEN
enum lttng_action_status lttng_action_add_error_query_results(
		const struct lttng_action *action,
		struct lttng_error_query_results *results)
{
	return action->add_error_query_results(action, results);
}

LTTNG_HIDDEN
enum lttng_action_status lttng_action_generic_add_error_query_results(
		const struct lttng_action *action,
		struct lttng_error_query_results *results)
{
	enum lttng_action_status action_status;
	struct lttng_error_query_result *error_counter = NULL;
	const uint64_t execution_failure_counter =
			uatomic_read(&action->execution_failure_counter);

	error_counter = lttng_error_query_result_counter_create(
			"total execution failures",
			"Aggregated count of errors encountered when executing the action",
			execution_failure_counter);
	if (!error_counter) {
		action_status = LTTNG_ACTION_STATUS_ERROR;
		goto end;
	}

	if (lttng_error_query_results_add_result(
			    results, error_counter)) {
		action_status = LTTNG_ACTION_STATUS_ERROR;
		goto end;
	}

	/* Ownership transferred to the results. */
	error_counter = NULL;
	action_status = LTTNG_ACTION_STATUS_OK;
end:
	lttng_error_query_result_destroy(error_counter);
	return action_status;
}
