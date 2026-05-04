/*
 * Copyright (c) 2025 Embedder
 * SPDX-License-Identifier: Apache-2.0
 *
 * User event helper implementations (B3).
 *
 * These emit CTF events with Embedder-reserved event IDs (0x100+).
 * All functions are ISR-safe — no heap, no blocking.
 */

#include <zephyr/kernel.h>
#include <embedder/trace_internal.h>

void embedder_trace_capture_start(const char *name)
{
	embedder_ctf_bounded_string_t bs = embedder_ctf_bounded_string(name);

	EMBEDDER_CTF_EMIT(EMBEDDER_CTF_EVENT_CAPTURE_START, bs);
}

void embedder_trace_capture_stop(void)
{
	EMBEDDER_CTF_EMIT_NOARGS(EMBEDDER_CTF_EVENT_CAPTURE_STOP);
}

void embedder_trace_event(uint16_t id, uint32_t arg0, uint32_t arg1)
{
	EMBEDDER_CTF_EMIT(EMBEDDER_CTF_EVENT_USER, id, arg0, arg1);
}

void embedder_trace_counter(uint16_t channel_id, int32_t value)
{
	EMBEDDER_CTF_EMIT(EMBEDDER_CTF_EVENT_COUNTER, channel_id, value);
}

void embedder_trace_app_metadata(const char *key, const char *value)
{
	embedder_ctf_bounded_string_t bs_key =
		embedder_ctf_bounded_string(key);
	embedder_ctf_bounded_string_t bs_val =
		embedder_ctf_bounded_string(value);

	EMBEDDER_CTF_EMIT(EMBEDDER_CTF_EVENT_APP_METADATA, bs_key, bs_val);
}
