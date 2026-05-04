/*
 * Copyright (c) 2025 Embedder
 * SPDX-License-Identifier: Apache-2.0
 *
 * Embedder Tracing Module — Public API
 *
 * Provides user-event instrumentation helpers that emit CTF events
 * alongside Zephyr's built-in kernel trace hooks. All functions are
 * ISR-safe and heap-free.
 */

#ifndef EMBEDDER_TRACE_H
#define EMBEDDER_TRACE_H

#include <zephyr/kernel.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ── Capture Markers ────────────────────────────────────────────────
 * Delimit a named capture session. The host uses these to know when
 * the firmware considers a trace "started" and "stopped".
 */
#if defined(CONFIG_EMBEDDER_TRACE_USER_EVENTS) || defined(__DOXYGEN__)

void embedder_trace_capture_start(const char *name);
void embedder_trace_capture_stop(void);

/*
 * ── Simple User Events ────────────────────────────────────────────
 */

/** Emit a generic user event with two 32-bit arguments. */
void embedder_trace_event(uint16_t id, uint32_t arg0, uint32_t arg1);

/** Emit a counter/gauge sample on a channel. */
void embedder_trace_counter(uint16_t channel_id, int32_t value);

/** Emit session-level key/value metadata (call once at startup). */
void embedder_trace_app_metadata(const char *key, const char *value);

#else /* !CONFIG_EMBEDDER_TRACE_USER_EVENTS */

static inline void embedder_trace_capture_start(const char *name) { (void)name; }
static inline void embedder_trace_capture_stop(void) {}
static inline void embedder_trace_event(uint16_t id, uint32_t arg0,
					uint32_t arg1)
{
	(void)id; (void)arg0; (void)arg1;
}
static inline void embedder_trace_counter(uint16_t channel_id, int32_t value)
{
	(void)channel_id; (void)value;
}
static inline void embedder_trace_app_metadata(const char *key,
					       const char *value)
{
	(void)key; (void)value;
}

#endif /* CONFIG_EMBEDDER_TRACE_USER_EVENTS */

/*
 * ── Rich Event Helpers ────────────────────────────────────────────
 * Higher-level typed events (B4). Only available when
 * CONFIG_EMBEDDER_TRACE_RICH_EVENTS=y.
 */
#if defined(CONFIG_EMBEDDER_TRACE_RICH_EVENTS) || defined(__DOXYGEN__)

/** Emit a string event (pointer stored; host resolves via ELF). */
void embedder_trace_string(uint16_t channel_id, const char *str);

/** Emit a signed 32-bit sample on a channel. */
void embedder_trace_sample_i32(uint16_t channel_id, int32_t value);

/** Emit a 32-bit float sample on a channel. */
void embedder_trace_sample_f32(uint16_t channel_id, float value);

/** Emit a state-machine transition. */
void embedder_trace_state(uint16_t channel_id, uint8_t new_state);

/** Mark the beginning of a measured interval. */
void embedder_trace_interval_begin(uint16_t channel_id);

/** Mark the end of a measured interval. */
void embedder_trace_interval_end(uint16_t channel_id);

/** Mark the beginning of a named code section/runnable. */
void embedder_trace_section_begin(const char *name);

/** Mark the end of the current code section/runnable. */
void embedder_trace_section_end(void);

/** Register channel metadata (name, unit). Call once per channel. */
void embedder_trace_channel_meta(uint16_t channel_id, const char *name,
				 const char *unit);

#else /* !CONFIG_EMBEDDER_TRACE_RICH_EVENTS */

static inline void embedder_trace_string(uint16_t channel_id,
					 const char *str)
{
	(void)channel_id; (void)str;
}
static inline void embedder_trace_sample_i32(uint16_t channel_id,
					     int32_t value)
{
	(void)channel_id; (void)value;
}
static inline void embedder_trace_sample_f32(uint16_t channel_id, float value)
{
	(void)channel_id; (void)value;
}
static inline void embedder_trace_state(uint16_t channel_id,
					uint8_t new_state)
{
	(void)channel_id; (void)new_state;
}
static inline void embedder_trace_interval_begin(uint16_t channel_id)
{
	(void)channel_id;
}
static inline void embedder_trace_interval_end(uint16_t channel_id)
{
	(void)channel_id;
}
static inline void embedder_trace_section_begin(const char *name)
{
	(void)name;
}
static inline void embedder_trace_section_end(void) {}
static inline void embedder_trace_channel_meta(uint16_t channel_id,
					       const char *name,
					       const char *unit)
{
	(void)channel_id; (void)name; (void)unit;
}

#endif /* CONFIG_EMBEDDER_TRACE_RICH_EVENTS */

/*
 * ── Memory / Stack Helpers ────────────────────────────────────────
 * Only available when CONFIG_EMBEDDER_TRACE_MEMORY=y.
 */
#if defined(CONFIG_EMBEDDER_TRACE_MEMORY) || defined(__DOXYGEN__)

void embedder_trace_heap_alloc(void *ptr, size_t size);
void embedder_trace_heap_free(void *ptr);
void embedder_trace_mem_slab_alloc(struct k_mem_slab *slab, void *ptr);
void embedder_trace_mem_slab_free(struct k_mem_slab *slab, void *ptr);
void embedder_trace_stack_sample(const struct k_thread *thread);
void embedder_trace_stack_sample_all(void);
void embedder_trace_stack_meta(const struct k_thread *thread);

#else /* !CONFIG_EMBEDDER_TRACE_MEMORY */

static inline void embedder_trace_heap_alloc(void *ptr, size_t size)
{
	(void)ptr; (void)size;
}
static inline void embedder_trace_heap_free(void *ptr) { (void)ptr; }
static inline void embedder_trace_mem_slab_alloc(struct k_mem_slab *slab,
						 void *ptr)
{
	(void)slab; (void)ptr;
}
static inline void embedder_trace_mem_slab_free(struct k_mem_slab *slab,
						void *ptr)
{
	(void)slab; (void)ptr;
}
static inline void embedder_trace_stack_sample(const struct k_thread *thread)
{
	(void)thread;
}
static inline void embedder_trace_stack_sample_all(void) {}
static inline void embedder_trace_stack_meta(const struct k_thread *thread)
{
	(void)thread;
}

#endif /* CONFIG_EMBEDDER_TRACE_MEMORY */

#ifdef __cplusplus
}
#endif

#endif /* EMBEDDER_TRACE_H */
