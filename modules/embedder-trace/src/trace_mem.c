/*
 * Copyright (c) 2025 Embedder
 * SPDX-License-Identifier: Apache-2.0
 *
 * Memory and stack helper implementations (B5).
 *
 * Provides heap allocation tracking and periodic thread stack
 * high-water-mark sampling via CTF events.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <embedder/trace_internal.h>

LOG_MODULE_DECLARE(embedder_trace, LOG_LEVEL_INF);

/* ── Heap Tracking ────────────────────────────────────────────── */

void embedder_trace_heap_alloc(void *ptr, size_t size)
{
	uint32_t addr = (uint32_t)(uintptr_t)ptr;
	uint32_t sz = (uint32_t)size;

	EMBEDDER_CTF_EMIT(EMBEDDER_CTF_EVENT_HEAP_ALLOC, addr, sz);
}

void embedder_trace_heap_free(void *ptr)
{
	uint32_t addr = (uint32_t)(uintptr_t)ptr;

	EMBEDDER_CTF_EMIT(EMBEDDER_CTF_EVENT_HEAP_FREE, addr);
}

void embedder_trace_mem_slab_alloc(struct k_mem_slab *slab, void *ptr)
{
	uint32_t slab_addr = (uint32_t)(uintptr_t)slab;
	uint32_t ptr_addr = (uint32_t)(uintptr_t)ptr;

	EMBEDDER_CTF_EMIT(EMBEDDER_CTF_EVENT_MEM_SLAB_ALLOC,
			  slab_addr, ptr_addr);
}

void embedder_trace_mem_slab_free(struct k_mem_slab *slab, void *ptr)
{
	uint32_t slab_addr = (uint32_t)(uintptr_t)slab;
	uint32_t ptr_addr = (uint32_t)(uintptr_t)ptr;

	EMBEDDER_CTF_EMIT(EMBEDDER_CTF_EVENT_MEM_SLAB_FREE,
			  slab_addr, ptr_addr);
}

/* ── Stack Sampling ───────────────────────────────────────────── */

void embedder_trace_stack_sample(const struct k_thread *thread)
{
	uint32_t thread_id = (uint32_t)(uintptr_t)thread;
	size_t unused = 0;
	int ret;

	ret = k_thread_stack_space_get((struct k_thread *)thread, &unused);
	if (ret == 0) {
		uint32_t unused32 = (uint32_t)unused;

		EMBEDDER_CTF_EMIT(EMBEDDER_CTF_EVENT_STACK_SAMPLE,
				  thread_id, unused32);
	}
}

static void stack_sample_cb(const struct k_thread *thread, void *user_data)
{
	ARG_UNUSED(user_data);
	embedder_trace_stack_sample(thread);
}

void embedder_trace_stack_sample_all(void)
{
	k_thread_foreach_unlocked(stack_sample_cb, NULL);
}

void embedder_trace_stack_meta(const struct k_thread *thread)
{
	uint32_t thread_id = (uint32_t)(uintptr_t)thread;
	uint32_t stack_size = (uint32_t)thread->stack_info.size;

	EMBEDDER_CTF_EMIT(EMBEDDER_CTF_EVENT_STACK_META,
			  thread_id, stack_size);
}

/* ── Periodic Auto-Sampling ───────────────────────────────────── */

#if CONFIG_EMBEDDER_TRACE_STACK_SAMPLE_PERIOD_MS > 0

static void stack_sample_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	embedder_trace_stack_sample_all();
}

static K_WORK_DEFINE(stack_sample_work, stack_sample_work_handler);

static void stack_sample_timer_handler(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	k_work_submit(&stack_sample_work);
}

static K_TIMER_DEFINE(stack_sample_timer, stack_sample_timer_handler, NULL);

static int embedder_trace_mem_init(void)
{
	k_timer_start(&stack_sample_timer,
		      K_MSEC(CONFIG_EMBEDDER_TRACE_STACK_SAMPLE_PERIOD_MS),
		      K_MSEC(CONFIG_EMBEDDER_TRACE_STACK_SAMPLE_PERIOD_MS));

	LOG_INF("Stack sampling every %d ms",
		CONFIG_EMBEDDER_TRACE_STACK_SAMPLE_PERIOD_MS);

	return 0;
}

SYS_INIT(embedder_trace_mem_init, APPLICATION,
	 CONFIG_APPLICATION_INIT_PRIORITY);

#endif /* CONFIG_EMBEDDER_TRACE_STACK_SAMPLE_PERIOD_MS > 0 */
