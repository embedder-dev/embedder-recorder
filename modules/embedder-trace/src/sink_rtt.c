/*
 * Copyright (c) 2025 Embedder
 * SPDX-License-Identifier: Apache-2.0
 *
 * RTT transport sink for the Embedder tracing module.
 *
 * Routes compact trace packets over a dedicated SEGGER RTT channel
 * using SEGGER_RTT_WriteNoLock() for ISR-safe, non-blocking output.
 *
 * Format descriptor metadata is emitted as chunked events (0x303) on
 * the same data channel, periodically after every Nth sync packet.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/logging/log.h>
#include <SEGGER_RTT.h>
#include <embedder/trace_transport.h>
#include <embedder/trace_internal.h>

LOG_MODULE_DECLARE(embedder_trace, LOG_LEVEL_INF);

/*
 * Map Kconfig RTT buffer-full policy to SEGGER RTT mode constants.
 */
#if defined(CONFIG_EMBEDDER_TRACE_RTT_MODE_BLOCK)
#define RTT_UP_MODE SEGGER_RTT_MODE_BLOCK_IF_FIFO_FULL
#elif defined(CONFIG_EMBEDDER_TRACE_RTT_MODE_TRIM)
#define RTT_UP_MODE SEGGER_RTT_MODE_NO_BLOCK_TRIM
#else
#define RTT_UP_MODE SEGGER_RTT_MODE_NO_BLOCK_SKIP
#endif

/*
 * Static up-buffer for the dedicated RTT data channel.
 */
static uint8_t rtt_up_buffer[CONFIG_EMBEDDER_TRACE_RTT_BUFFER_SIZE];

/**
 * Running total of events dropped due to RTT buffer overflow.
 * Referenced by the emit macros via extern in trace_internal.h.
 */
atomic_t embedder_trace_dropped;

/** Overflow state machine: 0 = normal, 1 = dropping. */
atomic_t embedder_trace_overflow_state;

/** Number of events dropped during the current overflow episode. */
atomic_t embedder_trace_overflow_drops;

/** Delta timestamp state — last emitted absolute timestamp. */
uint32_t _embd_last_ts;

/** Sync packet counter — sync emitted when low bits wrap to 0. */
uint32_t _embd_sync_counter;

/** Metadata sync divisor counter — metadata emitted every Nth sync. */
static uint32_t meta_sync_counter;

/** Guard flag: tracing hooks fire before transport_init runs. */
static atomic_t rtt_ready;

static unsigned int rtt_write(const void *data, uint32_t length)
{
	if (!atomic_get(&rtt_ready)) {
		return 0;
	}
	return SEGGER_RTT_WriteNoLock(CONFIG_EMBEDDER_TRACE_RTT_CHANNEL,
				     data, length);
}

/*
 * ── Sync Packet Emission ────────────────────────────────────────
 *
 * Sync packet format:
 *   [u8  length = 15]
 *   [u16 event_id = 0x302]
 *   [u32 magic = 0xE3BD0001]
 *   [u32 absolute_timestamp_ns]
 *   [u32 cpu_freq_hz]
 *
 * After every Nth sync (configured by METADATA_SYNC_DIVISOR), the
 * format descriptor metadata is re-emitted as chunked events.
 */
int _embedder_trace_emit_sync(void)
{
	const uint16_t eid = (uint16_t)EMBEDDER_CTF_EVENT_SYNC;
	const uint32_t magic = EMBEDDER_SYNC_MAGIC;
	const uint32_t ts = (uint32_t)k_cyc_to_ns_floor64(k_cycle_get_32());
	const uint32_t freq = sys_clock_hw_cycles_per_sec();

	uint8_t pkt[1 + 2 + 4 + 4 + 4]; /* 15 bytes */
	uint8_t *cursor = pkt + 1;

	memcpy(cursor, &eid, sizeof(eid));
	cursor += sizeof(eid);
	memcpy(cursor, &magic, sizeof(magic));
	cursor += sizeof(magic);
	memcpy(cursor, &ts, sizeof(ts));
	cursor += sizeof(ts);
	memcpy(cursor, &freq, sizeof(freq));
	cursor += sizeof(freq);

	pkt[0] = (uint8_t)(cursor - pkt);

	unsigned int w = embedder_trace_emit(pkt, pkt[0]);

	if (w != pkt[0]) {
		return 0;
	}

	/* Only update delta anchor if the sync was fully written. */
	_embd_last_ts = ts;

#if CONFIG_EMBEDDER_TRACE_METADATA_SYNC_DIVISOR > 0
	if ((meta_sync_counter++ %
	     CONFIG_EMBEDDER_TRACE_METADATA_SYNC_DIVISOR) == 0) {
		embedder_trace_emit_metadata_inline();
	}
#endif
	return 1;
}

/*
 * ── Transport API ────────────────────────────────────────────────
 */

void embedder_transport_init(void)
{
	int ret;

	/* Data channel */
	ret = SEGGER_RTT_ConfigUpBuffer(
		CONFIG_EMBEDDER_TRACE_RTT_CHANNEL,
		"embedder_trace",
		rtt_up_buffer,
		sizeof(rtt_up_buffer),
		RTT_UP_MODE);

	if (ret >= 0) {
		LOG_INF("RTT data sink ready on channel %d (%u bytes, %s mode)",
			CONFIG_EMBEDDER_TRACE_RTT_CHANNEL,
			(unsigned int)sizeof(rtt_up_buffer),
			IS_ENABLED(CONFIG_EMBEDDER_TRACE_RTT_MODE_BLOCK) ?
				"block" :
			IS_ENABLED(CONFIG_EMBEDDER_TRACE_RTT_MODE_TRIM) ?
				"trim" : "drop");
	} else {
		LOG_ERR("RTT data channel %d configuration failed: %d",
			CONFIG_EMBEDDER_TRACE_RTT_CHANNEL, ret);
	}

	_embd_last_ts = 0;
	_embd_sync_counter = 0;
	meta_sync_counter = 0;
	atomic_set(&rtt_ready, 1);

	/* Emit first sync packet — establishes absolute time anchor. */
	_embedder_trace_emit_sync();

	/*
	 * Reset drop/overflow state *after* the first sync so any
	 * pre-init tracing hook failures don't produce a bogus
	 * overflow event on the first real EMBEDDER_CTF_EMIT call.
	 * Set sync counter to 1 so the first emit doesn't immediately
	 * fire a duplicate sync (counter 0 would trigger the mask check).
	 */
	atomic_set(&embedder_trace_dropped, 0);
	atomic_set(&embedder_trace_overflow_state, 0);
	atomic_set(&embedder_trace_overflow_drops, 0);
	_embd_sync_counter = 1;
}

unsigned int embedder_trace_emit(const uint8_t *data, uint32_t length)
{
	return rtt_write(data, length);
}
