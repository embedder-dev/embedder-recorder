/*
 * Copyright (c) 2025 Embedder
 * SPDX-License-Identifier: Apache-2.0
 *
 * RTT transport sink for the Embedder tracing module.
 *
 * Routes compact trace packets over a dedicated SEGGER RTT channel
 * using SEGGER_RTT_WriteNoLock() for ISR-safe, non-blocking output.
 *
 * Format descriptor metadata is emitted once as chunked events (0x303)
 * on the same data channel during transport initialization.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/logging/log.h>
#include <SEGGER_RTT.h>
#include <embedder/trace.h>
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
static uint8_t rtt_control_buffer[CONFIG_EMBEDDER_TRACE_RTT_CONTROL_BUFFER_SIZE];

#define TRACE_CONTROL_CMD_BYTES 8
#define TRACE_CONTROL_VERSION 1
#define TRACE_CONTROL_OP_START_SESSION 1
#define TRACE_CONTROL_OP_STOP_SESSION 2
#define TRACE_CONTROL_MAGIC_0 'E'
#define TRACE_CONTROL_MAGIC_1 'T'
#define TRACE_CONTROL_MAGIC_2 'R'
#define TRACE_CONTROL_MAGIC_3 'C'

static struct k_work_delayable trace_control_work;
static uint8_t trace_control_rx[TRACE_CONTROL_CMD_BYTES];
static size_t trace_control_rx_len;

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

static uint8_t trace_control_checksum(const uint8_t *cmd)
{
	uint8_t sum = 0;

	for (size_t i = 0; i < TRACE_CONTROL_CMD_BYTES - 1; i++) {
		sum = (uint8_t)(sum + cmd[i]);
	}

	return sum;
}

static int trace_control_magic_matches_prefix(const uint8_t *cmd, size_t len)
{
	const uint8_t magic[] = {
		TRACE_CONTROL_MAGIC_0,
		TRACE_CONTROL_MAGIC_1,
		TRACE_CONTROL_MAGIC_2,
		TRACE_CONTROL_MAGIC_3,
	};

	if (len > sizeof(magic)) {
		return 1;
	}

	for (size_t i = 0; i < len; i++) {
		if (cmd[i] != magic[i]) {
			return 0;
		}
	}

	return 1;
}

static void trace_control_handle_command(const uint8_t *cmd)
{
	const uint8_t version = cmd[4];
	const uint8_t op = cmd[5];

	if (version != TRACE_CONTROL_VERSION ||
	    trace_control_checksum(cmd) != cmd[7]) {
		return;
	}

	switch (op) {
	case TRACE_CONTROL_OP_START_SESSION:
		if (embedder_trace_emit_preamble()) {
			embedder_trace_capture_start("host");
			LOG_DBG("RTT trace start command accepted");
		}
		break;
	case TRACE_CONTROL_OP_STOP_SESSION:
		embedder_trace_capture_stop();
		LOG_DBG("RTT trace stop command accepted");
		break;
	default:
		break;
	}
}

static void trace_control_feed_byte(uint8_t byte)
{
	trace_control_rx[trace_control_rx_len++] = byte;

	if (!trace_control_magic_matches_prefix(trace_control_rx,
					      trace_control_rx_len)) {
		trace_control_rx_len = byte == TRACE_CONTROL_MAGIC_0 ? 1 : 0;
		if (trace_control_rx_len == 1) {
			trace_control_rx[0] = byte;
		}
		return;
	}

	if (trace_control_rx_len < TRACE_CONTROL_CMD_BYTES) {
		return;
	}

	trace_control_handle_command(trace_control_rx);
	trace_control_rx_len = 0;
}

static void trace_control_work_handler(struct k_work *work)
{
	uint8_t buf[CONFIG_EMBEDDER_TRACE_RTT_CONTROL_BUFFER_SIZE];
	unsigned int read;

	ARG_UNUSED(work);

	do {
		read = SEGGER_RTT_Read(CONFIG_EMBEDDER_TRACE_RTT_CONTROL_CHANNEL,
				       buf, sizeof(buf));
		for (unsigned int i = 0; i < read; i++) {
			trace_control_feed_byte(buf[i]);
		}
	} while (read == sizeof(buf));

	k_work_schedule(&trace_control_work,
			K_MSEC(CONFIG_EMBEDDER_TRACE_RTT_CONTROL_POLL_MS));
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

	ret = SEGGER_RTT_ConfigDownBuffer(
		CONFIG_EMBEDDER_TRACE_RTT_CONTROL_CHANNEL,
		"embedder_trace_ctrl",
		rtt_control_buffer,
		sizeof(rtt_control_buffer),
		SEGGER_RTT_MODE_NO_BLOCK_SKIP);

	if (ret >= 0) {
		LOG_INF("RTT trace control ready on channel %d (%u bytes)",
			CONFIG_EMBEDDER_TRACE_RTT_CONTROL_CHANNEL,
			(unsigned int)sizeof(rtt_control_buffer));
		k_work_init_delayable(&trace_control_work,
				      trace_control_work_handler);
		k_work_schedule(&trace_control_work,
				K_MSEC(CONFIG_EMBEDDER_TRACE_RTT_CONTROL_POLL_MS));
	} else {
		LOG_ERR("RTT trace control channel %d configuration failed: %d",
			CONFIG_EMBEDDER_TRACE_RTT_CONTROL_CHANNEL, ret);
	}

	_embd_last_ts = 0;
	_embd_sync_counter = 0;
	atomic_set(&rtt_ready, 1);

	/*
	 * Reset pre-init tracing hook failures before the initial stream
	 * preamble. Short writes during sync/metadata emission should be
	 * accounted as real drops.
	 */
	atomic_set(&embedder_trace_dropped, 0);
	atomic_set(&embedder_trace_overflow_state, 0);
	atomic_set(&embedder_trace_overflow_drops, 0);

	embedder_trace_emit_preamble();
}

unsigned int embedder_trace_emit(const uint8_t *data, uint32_t length)
{
	return rtt_write(data, length);
}

int embedder_trace_emit_preamble(void)
{
	if (!_embedder_trace_emit_sync()) {
		return 0;
	}
	embedder_trace_emit_metadata_inline();
	return 1;
}
