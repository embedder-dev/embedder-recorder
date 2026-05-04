/*
 * Copyright (c) 2025 Embedder
 * SPDX-License-Identifier: Apache-2.0
 *
 * ITM/SWO transport sink for the Embedder tracing module.
 *
 * Routes compact trace packets over ARM ITM stimulus ports via SWO.
 *
 * Format descriptor metadata is emitted as chunked events (0x303) on
 * the same data port, periodically after every Nth sync packet.
 *
 * ISR-safe: writes are direct register stores with FIFO-ready polling.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/logging/log.h>
#include <embedder/trace_transport.h>
#include <embedder/trace_internal.h>

#include <cmsis_core.h>

/*
 * Build-time check: ITM is only available on Cortex-M3/M4/M7/M33/M55.
 * Cortex-M0, M0+, and M23 do not have ITM hardware.
 */
#if defined(CONFIG_CPU_CORTEX_M0) || defined(CONFIG_CPU_CORTEX_M0PLUS) || \
    defined(CONFIG_CPU_CORTEX_M23)
#error "ITM is not available on Cortex-M0/M0+/M23 cores. " \
       "Select EMBEDDER_TRACE_TRANSPORT_RTT instead."
#endif

LOG_MODULE_DECLARE(embedder_trace, LOG_LEVEL_INF);

/**
 * Running total of events dropped due to ITM FIFO saturation.
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

/**
 * Write a buffer byte-by-byte to an ITM stimulus port.
 * Returns the number of bytes successfully written.
 */
static unsigned int itm_write_port(uint32_t port, const void *data,
				   uint32_t length)
{
	const uint8_t *src = (const uint8_t *)data;
	unsigned int written = 0;

	for (uint32_t i = 0; i < length; i++) {
		int timeout = CONFIG_EMBEDDER_TRACE_ITM_FIFO_TIMEOUT;

		while ((ITM->PORT[port].u32 == 0) && (--timeout > 0)) {
			/* spin */
		}

		if (timeout == 0) {
			break;
		}

		ITM->PORT[port].u8 = src[i];
		written++;
	}

	return written;
}

static unsigned int itm_write(const void *data, uint32_t length)
{
	return itm_write_port(CONFIG_EMBEDDER_TRACE_ITM_PORT, data, length);
}

/*
 * ── Sync Packet Emission ────────────────────────────────────────
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
	const uint32_t data_port = CONFIG_EMBEDDER_TRACE_ITM_PORT;

	/* Unlock ITM registers */
	ITM->LAR = 0xC5ACCE55;

	/* Enable ITM (bit 0 of TCR) */
	ITM->TCR |= ITM_TCR_ITMENA_Msk;

	/* Enable data stimulus port */
	ITM->TER |= (1U << data_port);

#if defined(CONFIG_SOC_NRF9160)
	LOG_WRN("nRF9160: ITM output requires 4-bit parallel trace port "
		"(TRACECLK + TRACEDATA[0:3]). SWO single-wire is not "
		"available.");
#endif

	LOG_INF("ITM sink ready on port %u, FIFO timeout %d",
		data_port, CONFIG_EMBEDDER_TRACE_ITM_FIFO_TIMEOUT);
	_embd_last_ts = 0;
	_embd_sync_counter = 0;
	meta_sync_counter = 0;

	/* Emit first sync packet */
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
	return itm_write(data, length);
}
