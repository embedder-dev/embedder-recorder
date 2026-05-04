/*
 * Copyright (c) 2025 Embedder
 * SPDX-License-Identifier: Apache-2.0
 *
 * Embedder Tracing Module — Initialization
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <embedder/trace_transport.h>
#include <embedder/trace_internal.h>

LOG_MODULE_REGISTER(embedder_trace, LOG_LEVEL_INF);

/*
 * ── Periodic Trace Health Log ───────────────────────────────────
 *
 * When CONFIG_EMBEDDER_TRACE_HEALTH_INTERVAL_MS > 0, a delayed work
 * item periodically checks the drop counter and prints a LOG_WRN
 * if drops have occurred. The primary drop reporting mechanism is
 * the overflow event (0x301) emitted inline by the emit macros;
 * this health log is a secondary diagnostic aid.
 */
#if CONFIG_EMBEDDER_TRACE_HEALTH_INTERVAL_MS > 0

static struct k_work_delayable health_work;
static uint32_t last_reported_drops;

static void health_check(struct k_work *work)
{
	uint32_t current = (uint32_t)atomic_get(&embedder_trace_dropped);

	if (current != last_reported_drops) {
		uint32_t delta = current - last_reported_drops;

		LOG_WRN("Trace drops: %u new (%u total)", delta, current);
		last_reported_drops = current;
	}

	k_work_schedule(&health_work,
			K_MSEC(CONFIG_EMBEDDER_TRACE_HEALTH_INTERVAL_MS));
}

#endif /* CONFIG_EMBEDDER_TRACE_HEALTH_INTERVAL_MS > 0 */

static int embedder_trace_init(void)
{
	embedder_transport_init();

	LOG_INF("Embedder trace module initialized (compact stream v1)");

#ifdef CONFIG_EMBEDDER_TRACE_SCHEDULING_ONLY
	LOG_INF("Mode: scheduling-only");
#endif

#ifdef CONFIG_EMBEDDER_TRACE_TRANSPORT_RTT
	LOG_INF("Transport: RTT channel %d",
		CONFIG_EMBEDDER_TRACE_RTT_CHANNEL);
#elif defined(CONFIG_EMBEDDER_TRACE_TRANSPORT_ITM)
	LOG_INF("Transport: ITM port %d",
		CONFIG_EMBEDDER_TRACE_ITM_PORT);
#endif

#if CONFIG_EMBEDDER_TRACE_HEALTH_INTERVAL_MS > 0
	k_work_init_delayable(&health_work, health_check);
	k_work_schedule(&health_work,
			K_MSEC(CONFIG_EMBEDDER_TRACE_HEALTH_INTERVAL_MS));
	LOG_INF("Health check every %d ms",
		CONFIG_EMBEDDER_TRACE_HEALTH_INTERVAL_MS);
#endif

	return 0;
}

SYS_INIT(embedder_trace_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
