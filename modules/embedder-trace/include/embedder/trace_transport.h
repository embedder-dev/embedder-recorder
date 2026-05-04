/*
 * Copyright (c) 2025 Embedder
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal transport abstraction for the Embedder tracing module.
 * Not part of the public API — do not include from application code.
 *
 * Each transport sink (RTT or ITM) implements these functions.
 * Only one transport is compiled in based on the Kconfig choice.
 */

#ifndef EMBEDDER_TRACE_TRANSPORT_H
#define EMBEDDER_TRACE_TRANSPORT_H

#include <stdint.h>

/**
 * Initialize the transport (RTT channels or ITM stimulus ports).
 * Called once from trace_init.c during SYS_INIT.
 */
void embedder_transport_init(void);

/**
 * Write a compact trace packet to the data transport.
 * ISR-safe, non-blocking.
 *
 * @param data   Pointer to the complete packet bytes.
 * @param length Length of the packet in bytes.
 * @return Number of bytes actually written, or 0 on failure.
 */
unsigned int embedder_trace_emit(const uint8_t *data, uint32_t length);

/**
 * Emit format descriptor metadata as chunked events on the data channel.
 * Each chunk is a compact event (0x303) with index, count, and payload.
 *
 * Called at init and periodically after every Nth sync packet to
 * support late-attaching hosts. Implemented in trace_metadata.c.
 */
void embedder_trace_emit_metadata_inline(void);

/**
 * Emit a sync packet to the data transport.
 *
 * Sync packets contain the magic (0xE3BD0001), an absolute timestamp,
 * and the CPU frequency. They are emitted:
 *   - Once at transport init (first event in stream)
 *   - Periodically inline every (1 << SYNC_PERIOD_SHIFT) events
 *   - After overflow recovery for host resync
 *
 * The host scans for the sync magic to find valid stream entry points.
 * On success, _embd_last_ts is updated to the absolute timestamp so
 * subsequent deltas are anchored. On failure, _embd_last_ts is left
 * unchanged and the caller must enter overflow state.
 *
 * Must be called with IRQs locked (caller holds the lock).
 *
 * Implemented in the active sink (sink_rtt.c or sink_itm.c).
 *
 * @return 1 if the sync was fully written, 0 on transport failure.
 */
int _embedder_trace_emit_sync(void);

#endif /* EMBEDDER_TRACE_TRANSPORT_H */
