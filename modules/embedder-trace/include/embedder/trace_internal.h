/*
 * Copyright (c) 2025 Embedder
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal definitions for the Embedder tracing module.
 * Not part of the public API — do not include from application code.
 *
 * Wire format: compact stream with delta timestamps and periodic
 * sync packets for late-attach / mid-stream join support.
 */

#ifndef EMBEDDER_TRACE_INTERNAL_H
#define EMBEDDER_TRACE_INTERNAL_H

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/irq.h>
#include <embedder/trace_transport.h>
#include <string.h>

/*
 * ── Event ID Ranges ──────────────────────────────────────────────
 *
 * All event IDs are uint16_t in the compact event header.
 *
 * Zephyr kernel events:       0x10 – 0xFF
 * Embedder user events:       0x100 – 0x10F
 * Embedder rich events:       0x110 – 0x1FF
 * Embedder memory events:     0x200 – 0x2FF
 * Embedder health/sync:       0x300 – 0x3FF
 */

/* User events (B3) */
#define EMBEDDER_CTF_EVENT_CAPTURE_START      0x100
#define EMBEDDER_CTF_EVENT_CAPTURE_STOP       0x101
#define EMBEDDER_CTF_EVENT_USER               0x102
#define EMBEDDER_CTF_EVENT_COUNTER            0x103
#define EMBEDDER_CTF_EVENT_APP_METADATA       0x104

/* Rich events (B4) */
#define EMBEDDER_CTF_EVENT_STRING             0x110
#define EMBEDDER_CTF_EVENT_SAMPLE_I32         0x111
#define EMBEDDER_CTF_EVENT_SAMPLE_F32         0x112
#define EMBEDDER_CTF_EVENT_STATE              0x113
#define EMBEDDER_CTF_EVENT_INTERVAL_BEGIN     0x114
#define EMBEDDER_CTF_EVENT_INTERVAL_END       0x115
#define EMBEDDER_CTF_EVENT_SECTION_BEGIN      0x116
#define EMBEDDER_CTF_EVENT_SECTION_END        0x117
#define EMBEDDER_CTF_EVENT_CHANNEL_META       0x118
#define EMBEDDER_CTF_EVENT_SECTION_BEGIN_PC   0x119

/* Trace health events */
#define EMBEDDER_CTF_EVENT_TRACE_HEALTH       0x300
#define EMBEDDER_CTF_EVENT_OVERFLOW           0x301
#define EMBEDDER_CTF_EVENT_SYNC               0x302
#define EMBEDDER_CTF_EVENT_METADATA           0x303

/* Memory/stack events (B5) */
#define EMBEDDER_CTF_EVENT_HEAP_ALLOC         0x200
#define EMBEDDER_CTF_EVENT_HEAP_FREE          0x201
#define EMBEDDER_CTF_EVENT_MEM_SLAB_ALLOC     0x202
#define EMBEDDER_CTF_EVENT_MEM_SLAB_FREE      0x203
#define EMBEDDER_CTF_EVENT_STACK_SAMPLE       0x204
#define EMBEDDER_CTF_EVENT_STACK_META         0x205

/* Maximum bounded string length for metadata fields */
#define EMBEDDER_CTF_MAX_STRING_LEN           20

typedef struct {
	char buf[EMBEDDER_CTF_MAX_STRING_LEN];
} embedder_ctf_bounded_string_t;

/**
 * Pack a bounded string from a C string. Truncates and zero-pads
 * to exactly EMBEDDER_CTF_MAX_STRING_LEN bytes.
 */
static inline embedder_ctf_bounded_string_t
embedder_ctf_bounded_string(const char *str)
{
	embedder_ctf_bounded_string_t bs;

	if (str != NULL) {
		strncpy(bs.buf, str, sizeof(bs.buf));
		bs.buf[sizeof(bs.buf) - 1] = '\0';
	} else {
		memset(bs.buf, 0, sizeof(bs.buf));
	}

	return bs;
}

/*
 * ── Drop Counter ─────────────────────────────────────────────────
 *
 * Running total of events dropped due to transport write failure.
 * Defined in the active sink (sink_rtt.c or sink_itm.c).
 */
extern atomic_t embedder_trace_dropped;

/*
 * ── Overflow State Machine ──────────────────────────────────────
 *
 * Adopted from SystemView's approach: when a transport write fails,
 * we transition to "dropping" state. All subsequent events are
 * skipped until the transport has space. When space becomes
 * available, an overflow event (0x301) is emitted carrying the exact
 * drop count, followed by a sync packet for resync, then normal
 * recording resumes.
 *
 * States:
 *   0 = normal (emit events)
 *   1 = dropping (skip events, count drops)
 */
extern atomic_t embedder_trace_overflow_state;
extern atomic_t embedder_trace_overflow_drops;

/*
 * ── Compact Stream Format Constants ─────────────────────────────
 *
 * Each event: [u8 length][u16 event_id][varint ts_delta][payload]
 * Sync packet: [u8 length][u16 event_id=0x302][u32 magic][u32 abs_ts][u32 freq]
 *
 * Compact event header overhead: length (1) + event_id (2) + varint (1-5) = 4-8 bytes
 * vs old CTF 1.8 overhead of 27 bytes.
 */

/** Sync packet magic — host scans for this to find sync entry points. */
#define EMBEDDER_SYNC_MAGIC       0xE3BD0001U

/** Compact event header: length (1) + event_id (2) = 3 fixed bytes.
 *  Varint timestamp delta is variable-length and encoded separately. */
#define EMBEDDER_COMPACT_HDR_SIZE 3

/** Maximum varint encoding size for a uint32_t (5 bytes). */
#define EMBEDDER_VARINT_MAX       5

/*
 * ── Delta Timestamp State ───────────────────────────────────────
 *
 * Tracks the last emitted timestamp for delta encoding.
 * All accesses (read and write) must be under the same IRQ lock
 * that covers sync emission, overflow recovery, and event emission.
 * Defined in the active sink (sink_rtt.c or sink_itm.c).
 */
extern uint32_t _embd_last_ts;

/** Sync packet counter — accessed under the EMBEDDER_CTF_EMIT IRQ lock. */
extern uint32_t _embd_sync_counter;

/* Compute size of a single field */
#define _EMBD_FIELD_SIZE(x)   + sizeof(x)

/* Append a single field to the packet cursor */
#define _EMBD_FIELD_APPEND(x)                     \
	do {                                      \
		memcpy(_embd_cursor, &(x), sizeof(x)); \
		_embd_cursor += sizeof(x);        \
	} while (0);

/*
 * Variadic helpers — handle 0 to 6 extra fields.
 */

/* Count helpers */
#define _EMBD_VA_FIELD_SIZES(...)                                             \
	_EMBD_VA_FIELD_SIZES_(__VA_ARGS__, _EMBD_S6, _EMBD_S5, _EMBD_S4,    \
			      _EMBD_S3, _EMBD_S2, _EMBD_S1, _EMBD_S0)       \
			     (__VA_ARGS__)

#define _EMBD_VA_FIELD_SIZES_(_1, _2, _3, _4, _5, _6, N, ...) N

#define _EMBD_S0(...)
#define _EMBD_S1(a)             _EMBD_FIELD_SIZE(a)
#define _EMBD_S2(a, b)          _EMBD_FIELD_SIZE(a) _EMBD_FIELD_SIZE(b)
#define _EMBD_S3(a, b, c)       _EMBD_S2(a, b) _EMBD_FIELD_SIZE(c)
#define _EMBD_S4(a, b, c, d)    _EMBD_S3(a, b, c) _EMBD_FIELD_SIZE(d)
#define _EMBD_S5(a, b, c, d, e) _EMBD_S4(a, b, c, d) _EMBD_FIELD_SIZE(e)
#define _EMBD_S6(a, b, c, d, e, f) _EMBD_S5(a, b, c, d, e) _EMBD_FIELD_SIZE(f)

#define _EMBD_VA_FIELD_APPENDS(...)                                           \
	_EMBD_VA_FIELD_APPENDS_(__VA_ARGS__, _EMBD_A6, _EMBD_A5, _EMBD_A4,  \
				_EMBD_A3, _EMBD_A2, _EMBD_A1, _EMBD_A0)     \
			       (__VA_ARGS__)

#define _EMBD_VA_FIELD_APPENDS_(_1, _2, _3, _4, _5, _6, N, ...) N

#define _EMBD_A0(...)
#define _EMBD_A1(a)             _EMBD_FIELD_APPEND(a)
#define _EMBD_A2(a, b)          _EMBD_FIELD_APPEND(a) _EMBD_FIELD_APPEND(b)
#define _EMBD_A3(a, b, c)       _EMBD_A2(a, b) _EMBD_FIELD_APPEND(c)
#define _EMBD_A4(a, b, c, d)    _EMBD_A3(a, b, c) _EMBD_FIELD_APPEND(d)
#define _EMBD_A5(a, b, c, d, e) _EMBD_A4(a, b, c, d) _EMBD_FIELD_APPEND(e)
#define _EMBD_A6(a, b, c, d, e, f) _EMBD_A5(a, b, c, d, e) _EMBD_FIELD_APPEND(f)

/*
 * ── Varint Encoder ──────────────────────────────────────────────
 *
 * LEB128-style encoding: 7 data bits per byte, MSB is continuation.
 * Values 0-127 in 1 byte, 128-16383 in 2 bytes, etc.
 * Returns pointer past last written byte.
 */
static inline uint8_t *_embd_encode_varint(uint8_t *p, uint32_t val)
{
	while (val > 0x7F) {
		*p++ = (uint8_t)(val | 0x80);
		val >>= 7;
	}
	*p++ = (uint8_t)val;
	return p;
}

/*
 * ── Overflow Recovery Helper ────────────────────────────────────
 *
 * When in dropping state, attempt to send an overflow compact packet
 * containing the drop count, followed by a sync packet for host
 * resync. Only transitions back to normal state if *both* the
 * overflow event and the sync packet are fully written.
 *
 * Must be called with IRQs locked (caller holds the lock).
 *
 * Returns 1 if recovery succeeded (caller should proceed with
 * normal emit), 0 if still dropping (caller skips).
 */
static inline int _embedder_trace_try_overflow_recovery(void)
{
	const uint32_t _embd_ts =
		(uint32_t)k_cyc_to_ns_floor64(k_cycle_get_32());
	const uint16_t _embd_eid = (uint16_t)EMBEDDER_CTF_EVENT_OVERFLOW;
	const uint32_t _drop_count =
		(uint32_t)atomic_get(&embedder_trace_overflow_drops);

	/* Overflow event: [len][eid][varint delta][drop_count] */
	uint8_t _embd_pkt[EMBEDDER_COMPACT_HDR_SIZE + EMBEDDER_VARINT_MAX +
			  sizeof(uint32_t)];
	uint8_t *_embd_cursor = _embd_pkt + 1; /* skip length byte */

	_EMBD_FIELD_APPEND(_embd_eid);

	uint32_t _delta = _embd_ts - _embd_last_ts;

	_embd_cursor = _embd_encode_varint(_embd_cursor, _delta);
	_EMBD_FIELD_APPEND(_drop_count);

	uint8_t _total_len = (uint8_t)(_embd_cursor - _embd_pkt);

	_embd_pkt[0] = _total_len;

	unsigned int _w = embedder_trace_emit(_embd_pkt, _total_len);

	if (_w != _total_len) {
		atomic_inc(&embedder_trace_overflow_drops);
		atomic_inc(&embedder_trace_dropped);
		return 0;
	}

	_embd_last_ts = _embd_ts;

	/* Emit sync so host can resync after the gap.
	 * If the sync fails, enter overflow again — the host cannot
	 * reconstruct timestamps without a valid sync anchor. */
	if (!_embedder_trace_emit_sync()) {
		atomic_set(&embedder_trace_overflow_state, 1);
		atomic_inc(&embedder_trace_dropped);
		return 0;
	}

	atomic_set(&embedder_trace_overflow_state, 0);
	atomic_set(&embedder_trace_overflow_drops, 0);
	return 1;
}

/*
 * ── Compact Event Emission with Overflow State Machine ──────────
 *
 * Each EMBEDDER_CTF_EMIT() call produces a compact event on the
 * stack: [u8 length][u16 event_id][varint ts_delta][payload...]
 * and writes it directly to the transport.
 *
 * Periodic sync packets are emitted inline every
 * (1 << CONFIG_EMBEDDER_TRACE_SYNC_PERIOD_SHIFT) events.
 *
 * A single IRQ lock covers: overflow recovery, sync counter check,
 * sync emission, delta computation, and the transport write, to
 * prevent ISR/thread interleaving of any shared state.
 */

#define EMBEDDER_CTF_EMIT(evt_id, ...)                                        \
	do {                                                                  \
		const uint16_t _embd_eid = (uint16_t)(evt_id);                \
		/* Max packet: hdr(3) + varint(5) + payload */                \
		uint8_t _embd_pkt[EMBEDDER_COMPACT_HDR_SIZE +                 \
				  EMBEDDER_VARINT_MAX                         \
				  _EMBD_VA_FIELD_SIZES(__VA_ARGS__)];         \
		uint8_t *_embd_cursor = _embd_pkt + 1; /* skip length */      \
		_EMBD_FIELD_APPEND(_embd_eid);                                \
		unsigned int _embd_key = irq_lock();                          \
		/* Overflow recovery (under lock) */                          \
		if (atomic_get(&embedder_trace_overflow_state) != 0) {        \
			if (!_embedder_trace_try_overflow_recovery()) {       \
				irq_unlock(_embd_key);                        \
				break;                                        \
			}                                                     \
		} else if ((_embd_sync_counter++ &                            \
		     ((1U << CONFIG_EMBEDDER_TRACE_SYNC_PERIOD_SHIFT) - 1))   \
		    == 0) {                                                   \
			/* Periodic sync (skip if recovery just emitted one)*/\
			if (!_embedder_trace_emit_sync()) {                   \
				atomic_set(&embedder_trace_overflow_state, 1);\
				atomic_inc(&embedder_trace_dropped);          \
				irq_unlock(_embd_key);                        \
				break;                                        \
			}                                                     \
		}                                                             \
		const uint32_t _embd_ts =                                     \
			(uint32_t)k_cyc_to_ns_floor64(k_cycle_get_32());     \
		uint32_t _embd_delta = _embd_ts - _embd_last_ts;             \
		_embd_last_ts = _embd_ts;                                     \
		_embd_cursor = _embd_encode_varint(_embd_cursor, _embd_delta);\
		_EMBD_VA_FIELD_APPENDS(__VA_ARGS__)                           \
		uint8_t _embd_total = (uint8_t)(_embd_cursor - _embd_pkt);   \
		_embd_pkt[0] = _embd_total;                                   \
		unsigned int _embd_w =                                        \
			embedder_trace_emit(_embd_pkt, _embd_total);          \
		if (_embd_w != _embd_total) {                                 \
			atomic_set(&embedder_trace_overflow_state, 1);        \
			atomic_inc(&embedder_trace_overflow_drops);            \
			atomic_inc(&embedder_trace_dropped);                  \
		}                                                             \
		irq_unlock(_embd_key);                                        \
	} while (0)

#define EMBEDDER_CTF_EMIT_NOARGS(evt_id)                                      \
	do {                                                                  \
		const uint16_t _embd_eid = (uint16_t)(evt_id);                \
		uint8_t _embd_pkt[EMBEDDER_COMPACT_HDR_SIZE +                 \
				  EMBEDDER_VARINT_MAX];                       \
		uint8_t *_embd_cursor = _embd_pkt + 1;                        \
		_EMBD_FIELD_APPEND(_embd_eid);                                \
		unsigned int _embd_key = irq_lock();                          \
		if (atomic_get(&embedder_trace_overflow_state) != 0) {        \
			if (!_embedder_trace_try_overflow_recovery()) {       \
				irq_unlock(_embd_key);                        \
				break;                                        \
			}                                                     \
		} else if ((_embd_sync_counter++ &                            \
		     ((1U << CONFIG_EMBEDDER_TRACE_SYNC_PERIOD_SHIFT) - 1))   \
		    == 0) {                                                   \
			if (!_embedder_trace_emit_sync()) {                   \
				atomic_set(&embedder_trace_overflow_state, 1);\
				atomic_inc(&embedder_trace_dropped);          \
				irq_unlock(_embd_key);                        \
				break;                                        \
			}                                                     \
		}                                                             \
		const uint32_t _embd_ts =                                     \
			(uint32_t)k_cyc_to_ns_floor64(k_cycle_get_32());     \
		uint32_t _embd_delta = _embd_ts - _embd_last_ts;             \
		_embd_last_ts = _embd_ts;                                     \
		_embd_cursor = _embd_encode_varint(_embd_cursor, _embd_delta);\
		uint8_t _embd_total = (uint8_t)(_embd_cursor - _embd_pkt);   \
		_embd_pkt[0] = _embd_total;                                   \
		unsigned int _embd_w =                                        \
			embedder_trace_emit(_embd_pkt, _embd_total);          \
		if (_embd_w != _embd_total) {                                 \
			atomic_set(&embedder_trace_overflow_state, 1);        \
			atomic_inc(&embedder_trace_overflow_drops);            \
			atomic_inc(&embedder_trace_dropped);                  \
		}                                                             \
		irq_unlock(_embd_key);                                        \
	} while (0)

#endif /* EMBEDDER_TRACE_INTERNAL_H */
