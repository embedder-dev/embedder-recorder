/*
 * Copyright (c) 2025 Embedder
 * SPDX-License-Identifier: Apache-2.0
 *
 * Rich user event helper implementations (B4).
 *
 * Higher-level typed events built on top of the CTF packing macros.
 * All functions are ISR-safe — no heap, no blocking.
 */

#include <zephyr/kernel.h>
#include <string.h>
#include <embedder/trace_internal.h>

void embedder_trace_string(uint16_t channel_id, const char *str)
{
	/* Emit the pointer value — host resolves via ELF .rodata */
	uint32_t str_ptr = (uint32_t)(uintptr_t)str;

	EMBEDDER_CTF_EMIT(EMBEDDER_CTF_EVENT_STRING, channel_id, str_ptr);
}

void embedder_trace_sample_i32(uint16_t channel_id, int32_t value)
{
	EMBEDDER_CTF_EMIT(EMBEDDER_CTF_EVENT_SAMPLE_I32, channel_id, value);
}

void embedder_trace_sample_f32(uint16_t channel_id, float value)
{
	/* Bit-cast float to uint32_t for CTF packing — no FPU in packing */
	uint32_t bits;

	memcpy(&bits, &value, sizeof(bits));
	EMBEDDER_CTF_EMIT(EMBEDDER_CTF_EVENT_SAMPLE_F32, channel_id, bits);
}

void embedder_trace_state(uint16_t channel_id, uint8_t new_state)
{
	EMBEDDER_CTF_EMIT(EMBEDDER_CTF_EVENT_STATE, channel_id, new_state);
}

void embedder_trace_interval_begin(uint16_t channel_id)
{
	EMBEDDER_CTF_EMIT(EMBEDDER_CTF_EVENT_INTERVAL_BEGIN, channel_id);
}

void embedder_trace_interval_end(uint16_t channel_id)
{
	EMBEDDER_CTF_EMIT(EMBEDDER_CTF_EVENT_INTERVAL_END, channel_id);
}

void embedder_trace_section_begin(const char *name)
{
	if (name != NULL) {
		embedder_ctf_bounded_string_t bs =
			embedder_ctf_bounded_string(name);
		EMBEDDER_CTF_EMIT(EMBEDDER_CTF_EVENT_SECTION_BEGIN, bs);
	} else {
		/* Use return address as implicit section identifier */
		uint32_t ra = (uint32_t)(uintptr_t)
			__builtin_return_address(0);
		EMBEDDER_CTF_EMIT(EMBEDDER_CTF_EVENT_SECTION_BEGIN_PC, ra);
	}
}

void embedder_trace_section_end(void)
{
	EMBEDDER_CTF_EMIT_NOARGS(EMBEDDER_CTF_EVENT_SECTION_END);
}

void embedder_trace_channel_meta(uint16_t channel_id, const char *name,
				 const char *unit)
{
	embedder_ctf_bounded_string_t bs_name =
		embedder_ctf_bounded_string(name);
	embedder_ctf_bounded_string_t bs_unit =
		embedder_ctf_bounded_string(unit);

	EMBEDDER_CTF_EMIT(EMBEDDER_CTF_EVENT_CHANNEL_META,
			  channel_id, bs_name, bs_unit);
}
