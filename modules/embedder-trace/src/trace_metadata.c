/*
 * Copyright (c) 2025 Embedder
 * SPDX-License-Identifier: Apache-2.0
 *
 * Compact stream format descriptor for the Embedder tracing module.
 *
 * Contains a machine-readable event table describing all compact event
 * layouts. Emitted as chunked events (0x303) on the data channel at
 * init and periodically after sync packets to support late-attaching
 * hosts.
 *
 * Each chunk is a compact event:
 *   [u8 length][u16 event_id=0x303][u8 chunk_idx][u8 chunk_count]
 *   [u16 chunk_len][u8[] data]
 */

#include <zephyr/kernel.h>
#include <embedder/trace_transport.h>
#include <embedder/trace_internal.h>
#include <string.h>

/*
 * ── Compact Format Descriptor ────────────────────────────────────
 *
 * Machine-readable event table for the host decoder. Each line
 * describes one event ID, its name, and field layout.
 *
 * Compact event wire format:
 *   [u8 length][u16 event_id][varint ts_delta_ns][fields...]
 *
 * Sync packet wire format:
 *   [u8 length=15][u16 event_id=0x302][u32 magic=0xE3BD0001]
 *   [u32 abs_ts_ns][u32 cpu_freq_hz]
 *
 * Field types: u8, i8, u16, u32, i32, str20 (20-byte bounded string)
 */
static const char format_descriptor[] =
"embedder-trace-compact-v1\n"
"varint=leb128\n"
"byte_order=le\n"
"\n"
"# Kernel Events: Thread\n"
"0x10 thread_switched_out u32:thread_id str20:name\n"
"0x11 thread_switched_in u32:thread_id str20:name\n"
"0x12 thread_priority_set u32:thread_id str20:name i8:prio\n"
"0x13 thread_create u32:thread_id str20:name\n"
"0x14 thread_abort u32:thread_id str20:name\n"
"0x15 thread_suspend u32:thread_id str20:name\n"
"0x16 thread_resume u32:thread_id str20:name\n"
"0x17 thread_ready u32:thread_id str20:name\n"
"0x18 thread_pending u32:thread_id str20:name\n"
"0x19 thread_info u32:thread_id str20:name u32:stack_base u32:stack_size\n"
"0x1A thread_name_set u32:thread_id str20:name\n"
"\n"
"# Kernel Events: ISR / Idle\n"
"0x1B isr_enter\n"
"0x1C isr_exit\n"
"0x1E idle\n"
"\n"
"# Kernel Events: Semaphore\n"
"0x21 semaphore_init u32:id i32:ret\n"
"0x22 semaphore_give_enter u32:id\n"
"0x23 semaphore_give_exit u32:id\n"
"0x24 semaphore_take_enter u32:id u32:timeout\n"
"0x25 semaphore_take_blocking u32:id u32:timeout\n"
"0x26 semaphore_take_exit u32:id u32:timeout u32:ret\n"
"0x27 semaphore_reset u32:id\n"
"\n"
"# Kernel Events: Mutex\n"
"0x28 mutex_init u32:id i32:ret\n"
"0x29 mutex_lock_enter u32:id u32:timeout\n"
"0x2A mutex_lock_blocking u32:id u32:timeout\n"
"0x2B mutex_lock_exit u32:id u32:timeout i32:ret\n"
"0x2C mutex_unlock_enter u32:id\n"
"0x2D mutex_unlock_exit u32:id\n"
"\n"
"# Kernel Events: Timer\n"
"0x2E timer_init u32:id\n"
"0x2F timer_start u32:id u32:duration u32:period\n"
"0x30 timer_stop u32:id\n"
"0x31 timer_status_sync_enter u32:id\n"
"0x32 timer_status_sync_blocking u32:id u32:timeout\n"
"0x33 timer_status_sync_exit u32:id u32:result\n"
"\n"
"# Embedder User Events\n"
"0x100 capture_start str20:name\n"
"0x101 capture_stop\n"
"0x102 user_event u16:event_id u32:arg0 u32:arg1\n"
"0x103 counter u16:channel_id i32:value\n"
"0x104 app_metadata str20:key str20:value\n"
"\n"
"# Embedder Rich Events\n"
"0x110 trace_string u16:channel_id u32:str_ptr\n"
"0x111 sample_i32 u16:channel_id i32:value\n"
"0x112 sample_f32 u16:channel_id u32:bits\n"
"0x113 state u16:channel_id u8:new_state\n"
"0x114 interval_begin u16:channel_id\n"
"0x115 interval_end u16:channel_id\n"
"0x116 section_begin str20:name\n"
"0x117 section_end\n"
"0x118 channel_meta u16:channel_id str20:name str20:unit\n"
"0x119 section_begin_pc u32:pc\n"
"\n"
"# Embedder Memory Events\n"
"0x200 heap_alloc u32:addr u32:size\n"
"0x201 heap_free u32:addr\n"
"0x202 mem_slab_alloc u32:slab_addr u32:ptr_addr\n"
"0x203 mem_slab_free u32:slab_addr u32:ptr_addr\n"
"0x204 stack_sample u32:thread_id u32:unused\n"
"0x205 stack_meta u32:thread_id u32:stack_size\n"
"\n"
"# Trace Health / Sync Events\n"
"0x300 trace_health u32:total_dropped u32:new_dropped\n"
"0x301 overflow u32:drop_count\n"
"0x302 sync u32:magic u32:abs_ts_ns u32:cpu_freq_hz\n"
"0x303 metadata u8:chunk_index u8:chunk_count u16:chunk_len bytes:data\n"
;

/*
 * ── Chunked Metadata Emission ───────────────────────────────────
 *
 * The format descriptor is split into chunks that fit within the
 * compact packet length limit (uint8_t, max 255 bytes).
 *
 * Chunk packet layout:
 *   [u8  total_length]          -- packet length including this byte
 *   [u16 event_id = 0x303]     -- metadata event
 *   [u8  chunk_index]          -- 0-based chunk number
 *   [u8  chunk_count]          -- total chunks
 *   [u16 chunk_len]            -- payload bytes in this chunk
 *   [u8[] data]                -- raw descriptor bytes
 *
 * Fixed header: 1 + 2 + 1 + 1 + 2 = 7 bytes
 * Max payload per chunk: 255 - 7 = 248 bytes
 */

#define METADATA_CHUNK_HDR_SIZE  7
#define METADATA_CHUNK_MAX_DATA  (255 - METADATA_CHUNK_HDR_SIZE)

void embedder_trace_emit_metadata_inline(void)
{
	const uint16_t desc_len = (uint16_t)(sizeof(format_descriptor) - 1);
	const uint8_t chunk_count =
		(uint8_t)((desc_len + METADATA_CHUNK_MAX_DATA - 1) /
			  METADATA_CHUNK_MAX_DATA);
	const uint16_t eid = (uint16_t)EMBEDDER_CTF_EVENT_METADATA;

	uint16_t offset = 0;

	for (uint8_t i = 0; i < chunk_count; i++) {
		uint16_t remaining = desc_len - offset;
		uint16_t chunk_len = (remaining > METADATA_CHUNK_MAX_DATA)
				     ? METADATA_CHUNK_MAX_DATA
				     : remaining;

		uint8_t pkt[255];
		uint8_t *cursor = pkt + 1; /* skip length byte */

		memcpy(cursor, &eid, sizeof(eid));
		cursor += sizeof(eid);

		*cursor++ = i;            /* chunk_index */
		*cursor++ = chunk_count;  /* chunk_count */

		memcpy(cursor, &chunk_len, sizeof(chunk_len));
		cursor += sizeof(chunk_len);

		memcpy(cursor, &format_descriptor[offset], chunk_len);
		cursor += chunk_len;

		pkt[0] = (uint8_t)(cursor - pkt);

		embedder_trace_emit(pkt, pkt[0]);

		offset += chunk_len;
	}
}
