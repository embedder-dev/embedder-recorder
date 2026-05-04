/*
 * Copyright (c) 2025 Embedder
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tracing Overhead Benchmark Harness (B8)
 *
 * Measures cycles/event and bytes/event for each tracing event type.
 * Uses the DWT cycle counter for cycle-accurate measurement.
 *
 * Build:
 *   west build -b nrf9160dk/nrf9160/ns tests/benchmarks/tracing
 *
 * Results are printed over the console (UART/RTT channel 0).
 */

#include <zephyr/kernel.h>
#include <zephyr/timing/timing.h>
#include <zephyr/logging/log.h>
#include <embedder/trace.h>

LOG_MODULE_REGISTER(bench, LOG_LEVEL_INF);

#define ITERATIONS 1000

/*
 * Measure the average cycles for a given expression over ITERATIONS runs.
 */
#define BENCH(label, expr)                                              \
	do {                                                            \
		timing_t start, end;                                    \
		uint64_t total_cycles = 0;                              \
		timing_init();                                          \
		timing_start();                                         \
		for (int _i = 0; _i < ITERATIONS; _i++) {               \
			start = timing_counter_get();                   \
			expr;                                           \
			end = timing_counter_get();                     \
			total_cycles += timing_cycles_get(&start, &end);\
		}                                                       \
		timing_stop();                                          \
		LOG_INF("%-30s %6llu cycles/event",                     \
			label, total_cycles / ITERATIONS);              \
	} while (0)

int main(void)
{
	LOG_INF("=== Embedder Trace Overhead Benchmark ===");
	LOG_INF("Iterations per test: %d", ITERATIONS);
	LOG_INF("");

	/* ── B3: User Events ──────────────────────────────────── */
	LOG_INF("--- User Events (B3) ---");

	BENCH("capture_start",
	      embedder_trace_capture_start("bench"));

	BENCH("capture_stop",
	      embedder_trace_capture_stop());

	BENCH("event(id, arg0, arg1)",
	      embedder_trace_event(0x01, 0xAAAA, 0xBBBB));

	BENCH("counter(ch, val)",
	      embedder_trace_counter(0x01, 42));

	BENCH("app_metadata(k, v)",
	      embedder_trace_app_metadata("key", "value"));

	/* ── B4: Rich Events ──────────────────────────────────── */
	LOG_INF("");
	LOG_INF("--- Rich Events (B4) ---");

	BENCH("sample_i32(ch, val)",
	      embedder_trace_sample_i32(0x10, -1234));

	BENCH("sample_f32(ch, val)",
	      embedder_trace_sample_f32(0x10, 3.14f));

	BENCH("state(ch, st)",
	      embedder_trace_state(0x20, 2));

	BENCH("interval_begin(ch)",
	      embedder_trace_interval_begin(0x30));

	BENCH("interval_end(ch)",
	      embedder_trace_interval_end(0x30));

	BENCH("section_begin(name)",
	      embedder_trace_section_begin("test_section"));

	BENCH("section_end()",
	      embedder_trace_section_end());

	BENCH("channel_meta(ch, name, unit)",
	      embedder_trace_channel_meta(0x10, "accel_x", "m/s^2"));

	BENCH("string(ch, str)",
	      embedder_trace_string(0x40, "hello"));

	/* ── B5: Memory Events ────────────────────────────────── */
	LOG_INF("");
	LOG_INF("--- Memory Events (B5) ---");

	static uint8_t dummy_buf[64];

	BENCH("heap_alloc(ptr, sz)",
	      embedder_trace_heap_alloc(dummy_buf, sizeof(dummy_buf)));

	BENCH("heap_free(ptr)",
	      embedder_trace_heap_free(dummy_buf));

	BENCH("stack_sample(thread)",
	      embedder_trace_stack_sample(k_current_get()));

	/* ── Summary ──────────────────────────────────────────── */
	LOG_INF("");
	LOG_INF("=== Benchmark complete ===");

	return 0;
}
