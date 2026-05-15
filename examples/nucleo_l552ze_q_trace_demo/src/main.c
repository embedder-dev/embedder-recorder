/*
 * Copyright (c) 2025 Embedder
 * SPDX-License-Identifier: Apache-2.0
 *
 * Nucleo-L552ZE-Q Multi-Task Trace Demo
 *
 * Pure multi-task demo for the NUCLEO-L552ZE-Q with dense embedder-trace
 * instrumentation.  Modeled after the nrf9160dk_gnss_demo but without
 * any modem/GNSS code:
 *   - Producer/consumer pair with semaphore synchronization
 *   - 10 background worker threads generating scheduling contention
 *   - 17 trace channels (counters, intervals, state transitions)
 *   - LED heartbeat via GPIO timer
 *
 * Targets Zephyr 4.x on STM32L552ZE.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/random/random.h>

#include <embedder/trace.h>

LOG_MODULE_REGISTER(l552ze_demo, LOG_LEVEL_INF);

/* --------------------------------------------------------------------------
 * User event channel IDs
 * -------------------------------------------------------------------------- */
#define CH_PRODUCER_COUNT    1
#define CH_CONSUMER_WORK     2
#define CH_PRODUCER_RATE     3
#define CH_QUEUE_DEPTH       4
#define CH_CONSUMER_LATENCY  5
#define CH_APP_STATE         6
#define CH_MAIN_LOOP         7
#define CH_SENSOR_TEMP       8
#define CH_BATTERY_MV        9
#define CH_STORAGE_WRITE     10
#define CH_WATCHDOG_KICK     11
#define CH_CRYPTO_HASH       12
#define CH_STATUS_LOG        13
#define CH_COMMS_TX          14
#define CH_DISPLAY_REFRESH   15
#define CH_DIAG_CHECK        16
#define CH_POWER_STATE       17

/* --------------------------------------------------------------------------
 * Application state machine
 * -------------------------------------------------------------------------- */
enum app_state {
	APP_STATE_IDLE      = 0,
	APP_STATE_PRODUCING = 1,
	APP_STATE_CONSUMING = 2,
};

static enum app_state current_app_state = APP_STATE_IDLE;

/* --------------------------------------------------------------------------
 * LED heartbeat
 * -------------------------------------------------------------------------- */
static const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

static void heartbeat_expiry(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	gpio_pin_toggle_dt(&led0);
}

K_TIMER_DEFINE(heartbeat_timer, heartbeat_expiry, NULL);

/* --------------------------------------------------------------------------
 * Producer / consumer synchronization
 * -------------------------------------------------------------------------- */
static K_SEM_DEFINE(work_sem, 0, 1);
static int32_t produced_items;
static int32_t consumed_items;

/* --------------------------------------------------------------------------
 * Producer thread — generates work items every 50 ms
 * -------------------------------------------------------------------------- */
#define PRODUCER_STACK_SIZE 2048
K_THREAD_STACK_DEFINE(producer_stack, PRODUCER_STACK_SIZE);
static struct k_thread producer_thread;

static void producer_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	while (true) {
		produced_items++;

		current_app_state = APP_STATE_PRODUCING;
		embedder_trace_state(CH_APP_STATE, APP_STATE_PRODUCING);

		embedder_trace_counter(CH_PRODUCER_COUNT, produced_items);

		/* Track production rate: items produced so far */
		embedder_trace_counter(CH_PRODUCER_RATE, produced_items);

		/* Track pending queue depth */
		int32_t depth = produced_items - consumed_items;

		embedder_trace_counter(CH_QUEUE_DEPTH, depth);

		k_sem_give(&work_sem);

		LOG_INF("Producer: item #%d (queue depth %d)",
			produced_items, depth);

		k_sleep(K_MSEC(50));
	}
}

/* --------------------------------------------------------------------------
 * Consumer thread — processes items when semaphore is given
 * -------------------------------------------------------------------------- */
#define CONSUMER_STACK_SIZE 2048
K_THREAD_STACK_DEFINE(consumer_stack, CONSUMER_STACK_SIZE);
static struct k_thread consumer_thread;

static void consumer_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	while (true) {
		k_sem_take(&work_sem, K_FOREVER);

		current_app_state = APP_STATE_CONSUMING;
		embedder_trace_state(CH_APP_STATE, APP_STATE_CONSUMING);

		embedder_trace_interval_begin(CH_CONSUMER_WORK);
		embedder_trace_interval_begin(CH_CONSUMER_LATENCY);

		/* Simulate processing work: 5-15 ms */
		k_busy_wait(5000 + (sys_rand32_get() % 10000));

		consumed_items++;
		embedder_trace_counter(CH_CONSUMER_WORK, consumed_items);
		embedder_trace_interval_end(CH_CONSUMER_WORK);
		embedder_trace_interval_end(CH_CONSUMER_LATENCY);

		current_app_state = APP_STATE_IDLE;
		embedder_trace_state(CH_APP_STATE, APP_STATE_IDLE);

		LOG_INF("Consumer: processed item #%d", consumed_items);
	}
}

/* --------------------------------------------------------------------------
 * Background worker threads
 * -------------------------------------------------------------------------- */
#define STACK_SIZE 1024

K_THREAD_STACK_DEFINE(sensor_stack, STACK_SIZE);
K_THREAD_STACK_DEFINE(battery_stack, STACK_SIZE);
K_THREAD_STACK_DEFINE(storage_stack, STACK_SIZE);
K_THREAD_STACK_DEFINE(watchdog_stack, STACK_SIZE);
K_THREAD_STACK_DEFINE(crypto_stack, STACK_SIZE);
K_THREAD_STACK_DEFINE(status_stack, STACK_SIZE);
K_THREAD_STACK_DEFINE(comms_stack, STACK_SIZE);
K_THREAD_STACK_DEFINE(display_stack, STACK_SIZE);
K_THREAD_STACK_DEFINE(diagnostics_stack, STACK_SIZE);
K_THREAD_STACK_DEFINE(power_mgr_stack, STACK_SIZE);

static struct k_thread sensor_thread;
static struct k_thread battery_thread;
static struct k_thread storage_thread;
static struct k_thread watchdog_thread;
static struct k_thread crypto_thread;
static struct k_thread status_thread;
static struct k_thread comms_thread;
static struct k_thread display_thread;
static struct k_thread diagnostics_thread;
static struct k_thread power_mgr_thread;

/* Sensor: simulated temperature reading at 1 kHz */
static void sensor_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
	int32_t temp = 2200; /* centidegrees C */

	while (true) {
		embedder_trace_interval_begin(CH_SENSOR_TEMP);
		temp += (sys_rand32_get() % 100) - 50;
		embedder_trace_counter(CH_SENSOR_TEMP, temp);
		embedder_trace_interval_end(CH_SENSOR_TEMP);
		LOG_INF("Sensor: temp=%d.%02d C", temp / 100, temp % 100);
		k_sleep(K_MSEC(1));
	}
}

/* Battery: simulated voltage monitor every 500 ms */
static void battery_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
	int32_t voltage_mv = 3700;

	while (true) {
		embedder_trace_interval_begin(CH_BATTERY_MV);
		k_busy_wait(3000);
		voltage_mv -= (sys_rand32_get() % 5);
		if (voltage_mv < 3000) {
			voltage_mv = 3700;
		}
		embedder_trace_counter(CH_BATTERY_MV, voltage_mv);
		embedder_trace_interval_end(CH_BATTERY_MV);
		LOG_INF("Battery: %d mV", voltage_mv);
		k_sleep(K_MSEC(500));
	}
}

/* Storage: simulated flash write every 400-800 ms */
static void storage_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
	int32_t write_count = 0;

	while (true) {
		write_count++;
		embedder_trace_interval_begin(CH_STORAGE_WRITE);
		k_busy_wait(20000);
		embedder_trace_interval_end(CH_STORAGE_WRITE);
		embedder_trace_counter(CH_STORAGE_WRITE, write_count);
		LOG_INF("Storage: write #%d", write_count);
		k_sleep(K_MSEC(400 + (sys_rand32_get() % 400)));
	}
}

/* Watchdog: simulated kick every 300-500 ms */
static void watchdog_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
	int32_t kick_count = 0;

	while (true) {
		kick_count++;
		k_busy_wait(2000);
		embedder_trace_counter(CH_WATCHDOG_KICK, kick_count);
		LOG_INF("Watchdog: kick #%d", kick_count);
		k_sleep(K_MSEC(300 + (sys_rand32_get() % 200)));
	}
}

/* Crypto: simulated hash computation every 200-500 ms */
static void crypto_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
	int32_t hash_count = 0;

	while (true) {
		hash_count++;
		embedder_trace_interval_begin(CH_CRYPTO_HASH);
		k_busy_wait(12000);
		embedder_trace_interval_end(CH_CRYPTO_HASH);
		embedder_trace_counter(CH_CRYPTO_HASH, hash_count);
		LOG_INF("Crypto: hash #%d", hash_count);
		k_sleep(K_MSEC(200 + (sys_rand32_get() % 300)));
	}
}

/* Status logger: system stats every 300 ms */
static void status_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
	int32_t log_seq = 0;

	while (true) {
		log_seq++;
		embedder_trace_interval_begin(CH_STATUS_LOG);
		k_busy_wait(4000);
		embedder_trace_interval_end(CH_STATUS_LOG);
		embedder_trace_counter(CH_STATUS_LOG, log_seq);
		LOG_INF("Status: seq #%d, uptime=%u ms", log_seq,
			k_uptime_get_32());
		k_sleep(K_MSEC(300));
	}
}

/* Comms: simulated packet TX every 1 ms */
static void comms_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
	int32_t tx_bytes = 0;

	while (true) {
		int32_t pkt_size = 64 + (sys_rand32_get() % 192);

		embedder_trace_interval_begin(CH_COMMS_TX);
		k_busy_wait(8000);
		embedder_trace_interval_end(CH_COMMS_TX);
		tx_bytes += pkt_size;
		embedder_trace_counter(CH_COMMS_TX, tx_bytes);
		LOG_INF("Comms: tx %d bytes (total %d)", pkt_size, tx_bytes);
		k_sleep(K_MSEC(1));
	}
}

/* Display: simulated refresh every 300-600 ms */
static void display_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
	int32_t frame = 0;

	while (true) {
		frame++;
		embedder_trace_interval_begin(CH_DISPLAY_REFRESH);
		k_busy_wait(15000);
		embedder_trace_interval_end(CH_DISPLAY_REFRESH);
		embedder_trace_counter(CH_DISPLAY_REFRESH, frame);
		LOG_INF("Display: frame #%d", frame);
		k_sleep(K_MSEC(300 + (sys_rand32_get() % 300)));
	}
}

/* Diagnostics: self-check every 500-900 ms */
static void diagnostics_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
	int32_t check_count = 0;

	while (true) {
		check_count++;
		embedder_trace_interval_begin(CH_DIAG_CHECK);
		k_busy_wait(9000);
		embedder_trace_interval_end(CH_DIAG_CHECK);
		embedder_trace_counter(CH_DIAG_CHECK, check_count);
		LOG_INF("Diagnostics: check #%d OK", check_count);
		k_sleep(K_MSEC(500 + (sys_rand32_get() % 400)));
	}
}

/* Power manager: simulated sleep policy every 1 s */
static void power_mgr_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
	int32_t cycle = 0;
	uint8_t state = 0; /* 0=active, 1=light_sleep, 2=deep_sleep */

	while (true) {
		cycle++;
		state = (uint8_t)(cycle % 3);
		k_busy_wait(3000);
		embedder_trace_state(CH_POWER_STATE, state);
		embedder_trace_counter(CH_POWER_STATE, cycle);
		LOG_INF("Power: cycle #%d state=%d", cycle, state);
		k_sleep(K_SECONDS(1));
	}
}

/* --------------------------------------------------------------------------
 * Main
 * -------------------------------------------------------------------------- */
int main(void)
{
	int err;

	LOG_INF("Nucleo-L552ZE-Q Multi-Task Trace Demo started");

	/* ---- LED setup ---- */
	if (!gpio_is_ready_dt(&led0)) {
		LOG_ERR("LED device not ready");
		return -ENODEV;
	}

	err = gpio_pin_configure_dt(&led0, GPIO_OUTPUT_ACTIVE);
	if (err < 0) {
		LOG_ERR("Failed to configure LED: %d", err);
		return err;
	}

	/* ---- Register trace channel metadata ---- */
	embedder_trace_channel_meta(CH_PRODUCER_COUNT, "producer_count", "items");
	embedder_trace_channel_meta(CH_CONSUMER_WORK, "consumer_work", "us");
	embedder_trace_channel_meta(CH_PRODUCER_RATE, "producer_rate", "items/s");
	embedder_trace_channel_meta(CH_QUEUE_DEPTH, "queue_depth", "items");
	embedder_trace_channel_meta(CH_CONSUMER_LATENCY, "consumer_latency", "us");
	embedder_trace_channel_meta(CH_APP_STATE, "app_state", "");
	embedder_trace_channel_meta(CH_MAIN_LOOP, "main_loop", "us");
	embedder_trace_channel_meta(CH_SENSOR_TEMP, "sensor_temp", "cC");
	embedder_trace_channel_meta(CH_BATTERY_MV, "battery_mv", "mV");
	embedder_trace_channel_meta(CH_STORAGE_WRITE, "storage_write", "");
	embedder_trace_channel_meta(CH_WATCHDOG_KICK, "watchdog_kick", "");
	embedder_trace_channel_meta(CH_CRYPTO_HASH, "crypto_hash", "");
	embedder_trace_channel_meta(CH_STATUS_LOG, "status_log", "");
	embedder_trace_channel_meta(CH_COMMS_TX, "comms_tx", "bytes");
	embedder_trace_channel_meta(CH_DISPLAY_REFRESH, "display_refresh", "");
	embedder_trace_channel_meta(CH_DIAG_CHECK, "diag_check", "");
	embedder_trace_channel_meta(CH_POWER_STATE, "power_state", "");

	/* Emit application metadata */
	embedder_trace_app_metadata("fw_version", "1.0.0");
	embedder_trace_app_metadata("board", "nucleo_l552ze_q");
	embedder_trace_app_metadata("mode", "multi_task");

	/* Start continuous capture */
	embedder_trace_capture_start("l552ze_demo");

	/* Emit initial state */
	current_app_state = APP_STATE_IDLE;
	embedder_trace_state(CH_APP_STATE, APP_STATE_IDLE);

	/* ---- Start heartbeat timer ---- */
	k_timer_start(&heartbeat_timer, K_SECONDS(1), K_SECONDS(1));

	/* ---- Create producer/consumer threads ---- */
	k_thread_create(&producer_thread, producer_stack,
			K_THREAD_STACK_SIZEOF(producer_stack),
			producer_entry, NULL, NULL, NULL,
			3, 0, K_NO_WAIT);
	k_thread_name_set(&producer_thread, "producer");

	k_thread_create(&consumer_thread, consumer_stack,
			K_THREAD_STACK_SIZEOF(consumer_stack),
			consumer_entry, NULL, NULL, NULL,
			4, 0, K_NO_WAIT);
	k_thread_name_set(&consumer_thread, "consumer");

	/* ---- Create background worker threads ---- */
	k_thread_create(&sensor_thread, sensor_stack,
			K_THREAD_STACK_SIZEOF(sensor_stack),
			sensor_entry, NULL, NULL, NULL,
			7, 0, K_NO_WAIT);
	k_thread_name_set(&sensor_thread, "sensor");

	k_thread_create(&battery_thread, battery_stack,
			K_THREAD_STACK_SIZEOF(battery_stack),
			battery_entry, NULL, NULL, NULL,
			6, 0, K_NO_WAIT);
	k_thread_name_set(&battery_thread, "battery");

	k_thread_create(&storage_thread, storage_stack,
			K_THREAD_STACK_SIZEOF(storage_stack),
			storage_entry, NULL, NULL, NULL,
			11, 0, K_NO_WAIT);
	k_thread_name_set(&storage_thread, "storage");

	k_thread_create(&watchdog_thread, watchdog_stack,
			K_THREAD_STACK_SIZEOF(watchdog_stack),
			watchdog_entry, NULL, NULL, NULL,
			9, 0, K_NO_WAIT);
	k_thread_name_set(&watchdog_thread, "watchdog");

	k_thread_create(&crypto_thread, crypto_stack,
			K_THREAD_STACK_SIZEOF(crypto_stack),
			crypto_entry, NULL, NULL, NULL,
			14, 0, K_NO_WAIT);
	k_thread_name_set(&crypto_thread, "crypto");

	k_thread_create(&status_thread, status_stack,
			K_THREAD_STACK_SIZEOF(status_stack),
			status_entry, NULL, NULL, NULL,
			8, 0, K_NO_WAIT);
	k_thread_name_set(&status_thread, "status");

	k_thread_create(&comms_thread, comms_stack,
			K_THREAD_STACK_SIZEOF(comms_stack),
			comms_entry, NULL, NULL, NULL,
			10, 0, K_NO_WAIT);
	k_thread_name_set(&comms_thread, "comms");

	k_thread_create(&display_thread, display_stack,
			K_THREAD_STACK_SIZEOF(display_stack),
			display_entry, NULL, NULL, NULL,
			12, 0, K_NO_WAIT);
	k_thread_name_set(&display_thread, "display");

	k_thread_create(&diagnostics_thread, diagnostics_stack,
			K_THREAD_STACK_SIZEOF(diagnostics_stack),
			diagnostics_entry, NULL, NULL, NULL,
			8, 0, K_NO_WAIT);
	k_thread_name_set(&diagnostics_thread, "diagnostics");

	k_thread_create(&power_mgr_thread, power_mgr_stack,
			K_THREAD_STACK_SIZEOF(power_mgr_stack),
			power_mgr_entry, NULL, NULL, NULL,
			5, 0, K_NO_WAIT);
	k_thread_name_set(&power_mgr_thread, "power_mgr");

	LOG_INF("12 threads active, tracing enabled");

	return 0;
}
