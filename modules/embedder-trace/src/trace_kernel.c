/*
 * Copyright (c) 2025 Embedder
 * SPDX-License-Identifier: Apache-2.0
 *
 * Kernel trace hook implementations.
 *
 * Thread and ISR hooks override the __weak _user() functions defined
 * in Zephyr's tracing_user.c. Zephyr's wrapper functions (e.g.
 * sys_trace_k_thread_switched_in) call our _user overrides.
 *
 * Semaphore, mutex, and timer hooks are called directly from the
 * sys_port_trace_* macros in our custom tracing_user.h, since the
 * stock Zephyr tracing_user.c has no wrappers for these.
 *
 * All event IDs are emitted as uint16_t in CTF 1.8 packets, matching
 * the rtos-adapter-boundary.md contract.
 */

#include <zephyr/kernel.h>
#include <zephyr/kernel_structs.h>
#include <embedder/trace_internal.h>

/*
 * ── CTF Kernel Event IDs ─────────────────────────────────────────
 *
 * All kernel events use uint16_t IDs (stored as 2 bytes in the CTF
 * event header) for uniformity with Embedder extension events.
 * Numeric values are unchanged from the original Zephyr mapping.
 */
#define EMBEDDER_CTF_KERNEL_THREAD_SWITCHED_OUT     0x10
#define EMBEDDER_CTF_KERNEL_THREAD_SWITCHED_IN      0x11
#define EMBEDDER_CTF_KERNEL_THREAD_PRIORITY_SET     0x12
#define EMBEDDER_CTF_KERNEL_THREAD_CREATE           0x13
#define EMBEDDER_CTF_KERNEL_THREAD_ABORT            0x14
#define EMBEDDER_CTF_KERNEL_THREAD_SUSPEND          0x15
#define EMBEDDER_CTF_KERNEL_THREAD_RESUME           0x16
#define EMBEDDER_CTF_KERNEL_THREAD_READY            0x17
#define EMBEDDER_CTF_KERNEL_THREAD_PENDING          0x18
#define EMBEDDER_CTF_KERNEL_THREAD_INFO             0x19
#define EMBEDDER_CTF_KERNEL_THREAD_NAME_SET         0x1A
#define EMBEDDER_CTF_KERNEL_ISR_ENTER               0x1B
#define EMBEDDER_CTF_KERNEL_ISR_EXIT                0x1C
#define EMBEDDER_CTF_KERNEL_IDLE                    0x1E
#define EMBEDDER_CTF_KERNEL_SEMAPHORE_INIT          0x21
#define EMBEDDER_CTF_KERNEL_SEMAPHORE_GIVE_ENTER    0x22
#define EMBEDDER_CTF_KERNEL_SEMAPHORE_GIVE_EXIT     0x23
#define EMBEDDER_CTF_KERNEL_SEMAPHORE_TAKE_ENTER    0x24
#define EMBEDDER_CTF_KERNEL_SEMAPHORE_TAKE_BLOCKING 0x25
#define EMBEDDER_CTF_KERNEL_SEMAPHORE_TAKE_EXIT     0x26
#define EMBEDDER_CTF_KERNEL_SEMAPHORE_RESET         0x27
#define EMBEDDER_CTF_KERNEL_MUTEX_INIT              0x28
#define EMBEDDER_CTF_KERNEL_MUTEX_LOCK_ENTER        0x29
#define EMBEDDER_CTF_KERNEL_MUTEX_LOCK_BLOCKING     0x2A
#define EMBEDDER_CTF_KERNEL_MUTEX_LOCK_EXIT         0x2B
#define EMBEDDER_CTF_KERNEL_MUTEX_UNLOCK_ENTER      0x2C
#define EMBEDDER_CTF_KERNEL_MUTEX_UNLOCK_EXIT       0x2D
#define EMBEDDER_CTF_KERNEL_TIMER_INIT              0x2E
#define EMBEDDER_CTF_KERNEL_TIMER_START             0x2F
#define EMBEDDER_CTF_KERNEL_TIMER_STOP              0x30
#define EMBEDDER_CTF_KERNEL_TIMER_SYNC_ENTER        0x31
#define EMBEDDER_CTF_KERNEL_TIMER_SYNC_BLOCKING     0x32
#define EMBEDDER_CTF_KERNEL_TIMER_SYNC_EXIT         0x33

static void get_thread_name(struct k_thread *thread,
			     embedder_ctf_bounded_string_t *name)
{
	const char *tname = k_thread_name_get(thread);

	if (tname != NULL && tname[0] != '\0') {
		strncpy(name->buf, tname, sizeof(name->buf));
		name->buf[sizeof(name->buf) - 1] = '\0';
	}
}

/*
 * ── Thread Hooks ─────────────────────────────────────────────────
 *
 * Override the __weak _user() functions in Zephyr's tracing_user.c.
 * The wrapper functions in tracing_user.c call these.
 */

void sys_trace_thread_switched_out_user(void)
{
	embedder_ctf_bounded_string_t name = { "unknown" };
	struct k_thread *thread = k_sched_current_thread_query();
	uint32_t thread_id = (uint32_t)(uintptr_t)thread;

	get_thread_name(thread, &name);
	EMBEDDER_CTF_EMIT(EMBEDDER_CTF_KERNEL_THREAD_SWITCHED_OUT,
			     thread_id, name);
}

void sys_trace_thread_switched_in_user(void)
{
	embedder_ctf_bounded_string_t name = { "unknown" };
	struct k_thread *thread = k_sched_current_thread_query();
	uint32_t thread_id = (uint32_t)(uintptr_t)thread;

	get_thread_name(thread, &name);
	EMBEDDER_CTF_EMIT(EMBEDDER_CTF_KERNEL_THREAD_SWITCHED_IN,
			     thread_id, name);
}

void sys_trace_thread_priority_set_user(struct k_thread *thread, int prio)
{
	embedder_ctf_bounded_string_t name = { "unknown" };
	uint32_t thread_id = (uint32_t)(uintptr_t)thread;
	int8_t p = (int8_t)thread->base.prio;

	ARG_UNUSED(prio);

	get_thread_name(thread, &name);
	EMBEDDER_CTF_EMIT(EMBEDDER_CTF_KERNEL_THREAD_PRIORITY_SET,
			     thread_id, name, p);
}

void sys_trace_thread_create_user(struct k_thread *thread)
{
	embedder_ctf_bounded_string_t name = { "unknown" };
	uint32_t thread_id = (uint32_t)(uintptr_t)thread;

	get_thread_name(thread, &name);
	EMBEDDER_CTF_EMIT(EMBEDDER_CTF_KERNEL_THREAD_CREATE,
			     thread_id, name);

#if defined(CONFIG_THREAD_STACK_INFO)
	uint32_t sbase = (uint32_t)thread->stack_info.start;
	uint32_t ssize = (uint32_t)thread->stack_info.size;

	EMBEDDER_CTF_EMIT(EMBEDDER_CTF_KERNEL_THREAD_INFO,
			     thread_id, name, sbase, ssize);
#endif
}

void sys_trace_thread_abort_user(struct k_thread *thread)
{
	embedder_ctf_bounded_string_t name = { "unknown" };
	uint32_t thread_id = (uint32_t)(uintptr_t)thread;

	get_thread_name(thread, &name);
	EMBEDDER_CTF_EMIT(EMBEDDER_CTF_KERNEL_THREAD_ABORT,
			     thread_id, name);
}

void sys_trace_thread_suspend_user(struct k_thread *thread)
{
	embedder_ctf_bounded_string_t name = { "unknown" };
	uint32_t thread_id = (uint32_t)(uintptr_t)thread;

	get_thread_name(thread, &name);
	EMBEDDER_CTF_EMIT(EMBEDDER_CTF_KERNEL_THREAD_SUSPEND,
			     thread_id, name);
}

void sys_trace_thread_resume_user(struct k_thread *thread)
{
	embedder_ctf_bounded_string_t name = { "unknown" };
	uint32_t thread_id = (uint32_t)(uintptr_t)thread;

	get_thread_name(thread, &name);
	EMBEDDER_CTF_EMIT(EMBEDDER_CTF_KERNEL_THREAD_RESUME,
			     thread_id, name);
}

void sys_trace_thread_sched_ready_user(struct k_thread *thread)
{
	embedder_ctf_bounded_string_t name = { "unknown" };
	uint32_t thread_id = (uint32_t)(uintptr_t)thread;

	get_thread_name(thread, &name);
	EMBEDDER_CTF_EMIT(EMBEDDER_CTF_KERNEL_THREAD_READY,
			     thread_id, name);
}

void sys_trace_thread_pend_user(struct k_thread *thread)
{

	embedder_ctf_bounded_string_t name = { "unknown" };
	uint32_t thread_id = (uint32_t)(uintptr_t)thread;

	get_thread_name(thread, &name);
	EMBEDDER_CTF_EMIT(EMBEDDER_CTF_KERNEL_THREAD_PENDING,
			     thread_id, name);
}

void sys_trace_thread_info_user(struct k_thread *thread)
{
#if defined(CONFIG_THREAD_STACK_INFO)
	embedder_ctf_bounded_string_t name = { "unknown" };
	uint32_t thread_id = (uint32_t)(uintptr_t)thread;
	uint32_t sbase = (uint32_t)thread->stack_info.start;
	uint32_t ssize = (uint32_t)thread->stack_info.size;

	get_thread_name(thread, &name);
	EMBEDDER_CTF_EMIT(EMBEDDER_CTF_KERNEL_THREAD_INFO,
			     thread_id, name, sbase, ssize);
#else
	ARG_UNUSED(thread);
#endif
}

void sys_trace_thread_name_set_user(struct k_thread *thread)
{
	embedder_ctf_bounded_string_t name = { "unknown" };
	uint32_t thread_id = (uint32_t)(uintptr_t)thread;

	get_thread_name(thread, &name);
	EMBEDDER_CTF_EMIT(EMBEDDER_CTF_KERNEL_THREAD_NAME_SET,
			     thread_id, name);
}

/*
 * ── ISR / Idle Hooks ─────────────────────────────────────────────
 */

void sys_trace_isr_enter_user(void)
{
	EMBEDDER_CTF_EMIT_NOARGS(EMBEDDER_CTF_KERNEL_ISR_ENTER);
}

void sys_trace_isr_exit_user(void)
{
	EMBEDDER_CTF_EMIT_NOARGS(EMBEDDER_CTF_KERNEL_ISR_EXIT);
}

void sys_trace_idle_user(void)
{
	EMBEDDER_CTF_EMIT_NOARGS(EMBEDDER_CTF_KERNEL_IDLE);
}

/*
 * ── Semaphore Hooks ──────────────────────────────────────────────
 *
 * Called directly from sys_port_trace_* macros (no Zephyr wrappers).
 */

#if !defined(CONFIG_EMBEDDER_TRACE_SCHEDULING_ONLY)

void sys_trace_k_sem_init(struct k_sem *sem, int ret)
{
	uint32_t sem_id = (uint32_t)(uintptr_t)sem;
	int32_t r = (int32_t)ret;

	EMBEDDER_CTF_EMIT(EMBEDDER_CTF_KERNEL_SEMAPHORE_INIT, sem_id, r);
}

void sys_trace_k_sem_give_enter(struct k_sem *sem)
{
	uint32_t sem_id = (uint32_t)(uintptr_t)sem;

	EMBEDDER_CTF_EMIT(EMBEDDER_CTF_KERNEL_SEMAPHORE_GIVE_ENTER, sem_id);
}

void sys_trace_k_sem_give_exit(struct k_sem *sem)
{
	uint32_t sem_id = (uint32_t)(uintptr_t)sem;

	EMBEDDER_CTF_EMIT(EMBEDDER_CTF_KERNEL_SEMAPHORE_GIVE_EXIT, sem_id);
}

void sys_trace_k_sem_take_enter(struct k_sem *sem, k_timeout_t timeout)
{
	uint32_t sem_id = (uint32_t)(uintptr_t)sem;
	uint32_t timeout_us = k_ticks_to_us_floor32((uint32_t)timeout.ticks);

	EMBEDDER_CTF_EMIT(EMBEDDER_CTF_KERNEL_SEMAPHORE_TAKE_ENTER,
			     sem_id, timeout_us);
}

void sys_trace_k_sem_take_blocking(struct k_sem *sem, k_timeout_t timeout)
{
	uint32_t sem_id = (uint32_t)(uintptr_t)sem;
	uint32_t timeout_us = k_ticks_to_us_floor32((uint32_t)timeout.ticks);

	EMBEDDER_CTF_EMIT(EMBEDDER_CTF_KERNEL_SEMAPHORE_TAKE_BLOCKING,
			     sem_id, timeout_us);
}

void sys_trace_k_sem_take_exit(struct k_sem *sem, k_timeout_t timeout, int ret)
{

	uint32_t sem_id = (uint32_t)(uintptr_t)sem;
	uint32_t timeout_us = k_ticks_to_us_floor32((uint32_t)timeout.ticks);
	uint32_t r = (uint32_t)ret;

	EMBEDDER_CTF_EMIT(EMBEDDER_CTF_KERNEL_SEMAPHORE_TAKE_EXIT,
			     sem_id, timeout_us, r);
}

void sys_trace_k_sem_reset(struct k_sem *sem)
{
	uint32_t sem_id = (uint32_t)(uintptr_t)sem;

	EMBEDDER_CTF_EMIT(EMBEDDER_CTF_KERNEL_SEMAPHORE_RESET, sem_id);
}

/*
 * ── Mutex Hooks ──────────────────────────────────────────────────
 */

void sys_trace_k_mutex_init(struct k_mutex *mutex, int ret)
{
	uint32_t mutex_id = (uint32_t)(uintptr_t)mutex;
	int32_t r = (int32_t)ret;

	EMBEDDER_CTF_EMIT(EMBEDDER_CTF_KERNEL_MUTEX_INIT, mutex_id, r);
}

void sys_trace_k_mutex_lock_enter(struct k_mutex *mutex, k_timeout_t timeout)
{
	uint32_t mutex_id = (uint32_t)(uintptr_t)mutex;
	uint32_t timeout_us = k_ticks_to_us_floor32((uint32_t)timeout.ticks);

	EMBEDDER_CTF_EMIT(EMBEDDER_CTF_KERNEL_MUTEX_LOCK_ENTER,
			     mutex_id, timeout_us);
}

void sys_trace_k_mutex_lock_blocking(struct k_mutex *mutex,
				     k_timeout_t timeout)
{
	uint32_t mutex_id = (uint32_t)(uintptr_t)mutex;
	uint32_t timeout_us = k_ticks_to_us_floor32((uint32_t)timeout.ticks);

	EMBEDDER_CTF_EMIT(EMBEDDER_CTF_KERNEL_MUTEX_LOCK_BLOCKING,
			     mutex_id, timeout_us);
}

void sys_trace_k_mutex_lock_exit(struct k_mutex *mutex, k_timeout_t timeout,
				 int ret)
{
	uint32_t mutex_id = (uint32_t)(uintptr_t)mutex;
	uint32_t timeout_us = k_ticks_to_us_floor32((uint32_t)timeout.ticks);
	int32_t r = (int32_t)ret;

	EMBEDDER_CTF_EMIT(EMBEDDER_CTF_KERNEL_MUTEX_LOCK_EXIT,
			     mutex_id, timeout_us, r);
}

void sys_trace_k_mutex_unlock_enter(struct k_mutex *mutex)
{
	uint32_t mutex_id = (uint32_t)(uintptr_t)mutex;

	EMBEDDER_CTF_EMIT(EMBEDDER_CTF_KERNEL_MUTEX_UNLOCK_ENTER, mutex_id);
}

void sys_trace_k_mutex_unlock_exit(struct k_mutex *mutex, int ret)
{
	uint32_t mutex_id = (uint32_t)(uintptr_t)mutex;

	ARG_UNUSED(ret);

	EMBEDDER_CTF_EMIT(EMBEDDER_CTF_KERNEL_MUTEX_UNLOCK_EXIT, mutex_id);
}

/*
 * ── Timer Hooks ──────────────────────────────────────────────────
 *
 * Called directly from sys_port_trace_* macros in our tracing_user.h.
 */

void sys_trace_k_timer_init(struct k_timer *timer)
{
	uint32_t timer_id = (uint32_t)(uintptr_t)timer;

	EMBEDDER_CTF_EMIT(EMBEDDER_CTF_KERNEL_TIMER_INIT, timer_id);
}

void sys_trace_k_timer_start(struct k_timer *timer, k_timeout_t duration,
			     k_timeout_t period)
{
	uint32_t timer_id = (uint32_t)(uintptr_t)timer;
	uint32_t dur_us = k_ticks_to_us_floor32((uint32_t)duration.ticks);
	uint32_t per_us = k_ticks_to_us_floor32((uint32_t)period.ticks);

	EMBEDDER_CTF_EMIT(EMBEDDER_CTF_KERNEL_TIMER_START,
			     timer_id, dur_us, per_us);
}

void sys_trace_k_timer_stop(struct k_timer *timer)
{
	uint32_t timer_id = (uint32_t)(uintptr_t)timer;

	EMBEDDER_CTF_EMIT(EMBEDDER_CTF_KERNEL_TIMER_STOP, timer_id);
}

void sys_trace_k_timer_status_sync_enter(struct k_timer *timer)
{
	uint32_t timer_id = (uint32_t)(uintptr_t)timer;

	EMBEDDER_CTF_EMIT(EMBEDDER_CTF_KERNEL_TIMER_SYNC_ENTER, timer_id);
}

void sys_trace_k_timer_status_sync_blocking(struct k_timer *timer,
					    k_timeout_t timeout)
{
	uint32_t timer_id = (uint32_t)(uintptr_t)timer;
	uint32_t timeout_us = k_ticks_to_us_floor32((uint32_t)timeout.ticks);

	EMBEDDER_CTF_EMIT(EMBEDDER_CTF_KERNEL_TIMER_SYNC_BLOCKING,
			     timer_id, timeout_us);
}

void sys_trace_k_timer_status_sync_exit(struct k_timer *timer, uint32_t result)
{
	uint32_t timer_id = (uint32_t)(uintptr_t)timer;

	EMBEDDER_CTF_EMIT(EMBEDDER_CTF_KERNEL_TIMER_SYNC_EXIT,
			     timer_id, result);
}

#endif /* !CONFIG_EMBEDDER_TRACE_SCHEDULING_ONLY */
