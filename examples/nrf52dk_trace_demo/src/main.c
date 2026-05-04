/*
 * Copyright (c) 2025 Embedder
 * SPDX-License-Identifier: Apache-2.0
 *
 * nRF52 DK Trace Demo
 *
 * Multi-threaded application demonstrating the embedder-trace module:
 *   - Kernel tracing (thread switches, semaphores, timers, ISRs)
 *   - User events (counters, intervals, state transitions)
 *   - Capture markers
 *   - LED heartbeat via GPIO
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/random/random.h>
#include <embedder/trace.h>

LOG_MODULE_REGISTER(trace_demo, LOG_LEVEL_INF);

/* LED0 on the nRF52 DK */
static const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

/* Semaphore for producer/consumer synchronization */
K_SEM_DEFINE(work_sem, 0, 1);

/* Thread stacks */
#define STACK_SIZE 1024
K_THREAD_STACK_DEFINE(producer_stack, STACK_SIZE);
K_THREAD_STACK_DEFINE(consumer_stack, STACK_SIZE);
K_THREAD_STACK_DEFINE(sensor_stack, STACK_SIZE);
K_THREAD_STACK_DEFINE(logger_stack, STACK_SIZE);
K_THREAD_STACK_DEFINE(watchdog_stack, STACK_SIZE);
K_THREAD_STACK_DEFINE(comms_stack, STACK_SIZE);
K_THREAD_STACK_DEFINE(storage_stack, STACK_SIZE);
K_THREAD_STACK_DEFINE(display_stack, STACK_SIZE);
K_THREAD_STACK_DEFINE(audio_stack, STACK_SIZE);
K_THREAD_STACK_DEFINE(crypto_stack, STACK_SIZE);
K_THREAD_STACK_DEFINE(network_stack, STACK_SIZE);
K_THREAD_STACK_DEFINE(diagnostics_stack, STACK_SIZE);
static struct k_thread producer_thread;
static struct k_thread consumer_thread;
static struct k_thread sensor_thread;
static struct k_thread logger_thread;
static struct k_thread watchdog_thread;
static struct k_thread comms_thread;
static struct k_thread storage_thread;
static struct k_thread display_thread;
static struct k_thread audio_thread;
static struct k_thread crypto_thread;
static struct k_thread network_thread;
static struct k_thread diagnostics_thread;

/* User event channel IDs */
#define CH_PRODUCER_COUNT  1
#define CH_CONSUMER_WORK   2
#define CH_APP_STATE       3
#define CH_SENSOR_TEMP     4
#define CH_LOGGER_FLUSH    5
#define CH_WATCHDOG_KICK   6
#define CH_COMMS_TX        7
#define CH_STORAGE_WRITE   8
#define CH_DISPLAY_REFRESH 9
#define CH_AUDIO_SAMPLE    10
#define CH_CRYPTO_HASH     11
#define CH_NETWORK_POLL    12
#define CH_DIAG_CHECK      13

/* Application states */
enum app_state {
	STATE_IDLE = 0,
	STATE_PRODUCING = 1,
	STATE_CONSUMING = 2,
};

/* Timer for LED heartbeat */
static void heartbeat_expiry(struct k_timer *timer);
K_TIMER_DEFINE(heartbeat_timer, heartbeat_expiry, NULL);

static void heartbeat_expiry(struct k_timer *timer)
{
	ARG_UNUSED(timer);
	gpio_pin_toggle_dt(&led0);
}

/*
 * Producer thread: gives the semaphore every 500ms and emits
 * a counter event with an incrementing value.
 */
static void producer_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	int32_t count = 0;

	while (true) {
		embedder_trace_state(CH_APP_STATE, STATE_PRODUCING);

		count++;
		embedder_trace_counter(CH_PRODUCER_COUNT, count);
		LOG_INF("Producer: count=%d", count);

		k_sem_give(&work_sem);

		embedder_trace_state(CH_APP_STATE, STATE_IDLE);
		k_sleep(K_MSEC(500));
	}
}

/*
 * Consumer thread: waits on the semaphore, then simulates work
 * with a busy-wait while emitting interval events.
 */
static void consumer_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		k_sem_take(&work_sem, K_FOREVER);

		embedder_trace_state(CH_APP_STATE, STATE_CONSUMING);

		/* Measure the simulated work with an interval */
		embedder_trace_interval_begin(CH_CONSUMER_WORK);
		k_busy_wait(1000); /* 1ms of simulated work */
		embedder_trace_interval_end(CH_CONSUMER_WORK);

		LOG_INF("Consumer: work done");

		embedder_trace_state(CH_APP_STATE, STATE_IDLE);
	}
}

/*
 * Sensor thread: reads a simulated temperature sensor every 2-4s.
 */
static void sensor_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
	int32_t temp = 2000; /* centidegrees */

	while (true) {
		LOG_INF("Sensor: task started");
		embedder_trace_interval_begin(CH_SENSOR_TEMP);
		k_busy_wait(500); /* 0.5ms ADC read */
		temp += (sys_rand32_get() % 100) - 50;
		embedder_trace_counter(CH_SENSOR_TEMP, temp);
		embedder_trace_interval_end(CH_SENSOR_TEMP);
		LOG_INF("Sensor: temp=%d.%02d C", temp / 100, temp % 100);
		k_sleep(K_MSEC(2000 + (sys_rand32_get() % 2000)));
	}
}

/*
 * Logger thread: flushes a simulated log buffer every 1-3s.
 */
static void logger_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
	int32_t flush_count = 0;

	while (true) {
		LOG_INF("Logger: task started");
		flush_count++;
		embedder_trace_interval_begin(CH_LOGGER_FLUSH);
		k_busy_wait(200);
		embedder_trace_interval_end(CH_LOGGER_FLUSH);
		embedder_trace_counter(CH_LOGGER_FLUSH, flush_count);
		LOG_INF("Logger: flush #%d", flush_count);
		k_sleep(K_MSEC(1000 + (sys_rand32_get() % 2000)));
	}
}

/*
 * Watchdog thread: kicks a simulated watchdog every 3-5s.
 */
static void watchdog_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
	int32_t kick_count = 0;

	while (true) {
		LOG_INF("Watchdog: task started");
		kick_count++;
		embedder_trace_counter(CH_WATCHDOG_KICK, kick_count);
		LOG_INF("Watchdog: kick #%d", kick_count);
		k_sleep(K_MSEC(3000 + (sys_rand32_get() % 2000)));
	}
}

/*
 * Comms thread: simulates transmitting a packet every 1-2s.
 */
static void comms_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
	int32_t tx_bytes = 0;

	while (true) {
		LOG_INF("Comms: task started");
		int32_t pkt_size = 64 + (sys_rand32_get() % 192);

		embedder_trace_interval_begin(CH_COMMS_TX);
		k_busy_wait(800);
		embedder_trace_interval_end(CH_COMMS_TX);
		tx_bytes += pkt_size;
		embedder_trace_counter(CH_COMMS_TX, tx_bytes);
		LOG_INF("Comms: tx %d bytes (total %d)", pkt_size, tx_bytes);
		k_sleep(K_MSEC(1000 + (sys_rand32_get() % 1000)));
	}
}

/*
 * Storage thread: writes simulated data to flash every 4-8s.
 */
static void storage_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
	int32_t write_count = 0;

	while (true) {
		LOG_INF("Storage: task started");
		write_count++;
		embedder_trace_interval_begin(CH_STORAGE_WRITE);
		k_busy_wait(2000); /* 2ms flash write */
		embedder_trace_interval_end(CH_STORAGE_WRITE);
		embedder_trace_counter(CH_STORAGE_WRITE, write_count);
		LOG_INF("Storage: write #%d", write_count);
		k_sleep(K_MSEC(4000 + (sys_rand32_get() % 4000)));
	}
}

/*
 * Display thread: refreshes a simulated display every 3-6s.
 */
static void display_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
	int32_t frame = 0;

	while (true) {
		LOG_INF("Display: task started");
		frame++;
		embedder_trace_interval_begin(CH_DISPLAY_REFRESH);
		k_busy_wait(1500); /* 1.5ms display update */
		embedder_trace_interval_end(CH_DISPLAY_REFRESH);
		embedder_trace_counter(CH_DISPLAY_REFRESH, frame);
		LOG_INF("Display: frame #%d", frame);
		k_sleep(K_MSEC(3000 + (sys_rand32_get() % 3000)));
	}
}

/*
 * Audio thread: processes a simulated audio buffer every 5-10s.
 */
static void audio_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
	int32_t sample_count = 0;

	while (true) {
		LOG_INF("Audio: task started");
		sample_count += 256;
		embedder_trace_interval_begin(CH_AUDIO_SAMPLE);
		k_busy_wait(3000); /* 3ms DSP work */
		embedder_trace_interval_end(CH_AUDIO_SAMPLE);
		embedder_trace_counter(CH_AUDIO_SAMPLE, sample_count);
		LOG_INF("Audio: processed %d samples", sample_count);
		k_sleep(K_MSEC(5000 + (sys_rand32_get() % 5000)));
	}
}

/*
 * Crypto thread: computes a simulated hash every 2-5s.
 */
static void crypto_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
	int32_t hash_count = 0;

	while (true) {
		LOG_INF("Crypto: task started");
		hash_count++;
		embedder_trace_interval_begin(CH_CRYPTO_HASH);
		k_busy_wait(1200); /* 1.2ms hash compute */
		embedder_trace_interval_end(CH_CRYPTO_HASH);
		embedder_trace_counter(CH_CRYPTO_HASH, hash_count);
		LOG_INF("Crypto: hash #%d", hash_count);
		k_sleep(K_MSEC(2000 + (sys_rand32_get() % 3000)));
	}
}

/*
 * Network thread: polls a simulated network interface every 1-3s.
 */
static void network_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
	int32_t rx_packets = 0;

	while (true) {
		LOG_INF("Network: task started");
		rx_packets++;
		embedder_trace_interval_begin(CH_NETWORK_POLL);
		k_busy_wait(600);
		embedder_trace_interval_end(CH_NETWORK_POLL);
		embedder_trace_counter(CH_NETWORK_POLL, rx_packets);
		LOG_INF("Network: rx pkt #%d", rx_packets);
		k_sleep(K_MSEC(1000 + (sys_rand32_get() % 2000)));
	}
}

/*
 * Diagnostics thread: runs a self-check every 5-9s.
 */
static void diagnostics_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
	int32_t check_count = 0;

	while (true) {
		LOG_INF("Diagnostics: task started");
		check_count++;
		embedder_trace_interval_begin(CH_DIAG_CHECK);
		k_busy_wait(900);
		embedder_trace_interval_end(CH_DIAG_CHECK);
		embedder_trace_counter(CH_DIAG_CHECK, check_count);
		LOG_INF("Diagnostics: check #%d OK", check_count);
		k_sleep(K_MSEC(5000 + (sys_rand32_get() % 4000)));
	}
}

int main(void)
{
	int ret;

	LOG_INF("Embedder Trace Demo started");

	/* Configure LED */
	if (!gpio_is_ready_dt(&led0)) {
		LOG_ERR("LED device not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&led0, GPIO_OUTPUT_ACTIVE);
	if (ret < 0) {
		LOG_ERR("Failed to configure LED: %d", ret);
		return ret;
	}

	/* Register channel metadata so the host decoder can label them */
	embedder_trace_channel_meta(CH_PRODUCER_COUNT, "producer_count", "items");
	embedder_trace_channel_meta(CH_CONSUMER_WORK, "consumer_work", "us");
	embedder_trace_channel_meta(CH_APP_STATE, "app_state", "");
	embedder_trace_channel_meta(CH_SENSOR_TEMP, "sensor_temp", "cC");
	embedder_trace_channel_meta(CH_LOGGER_FLUSH, "logger_flush", "");
	embedder_trace_channel_meta(CH_WATCHDOG_KICK, "watchdog_kick", "");
	embedder_trace_channel_meta(CH_COMMS_TX, "comms_tx", "bytes");
	embedder_trace_channel_meta(CH_STORAGE_WRITE, "storage_write", "");
	embedder_trace_channel_meta(CH_DISPLAY_REFRESH, "display_refresh", "");
	embedder_trace_channel_meta(CH_AUDIO_SAMPLE, "audio_sample", "samples");
	embedder_trace_channel_meta(CH_CRYPTO_HASH, "crypto_hash", "");
	embedder_trace_channel_meta(CH_NETWORK_POLL, "network_poll", "pkts");
	embedder_trace_channel_meta(CH_DIAG_CHECK, "diag_check", "");

	/* Emit application metadata */
	embedder_trace_app_metadata("fw_version", "1.0.0");
	embedder_trace_app_metadata("board", "nrf52dk");

	/* Start continuous capture */
	embedder_trace_capture_start("demo");

	/* Start heartbeat timer — toggles LED0 every 1 second */
	k_timer_start(&heartbeat_timer, K_SECONDS(1), K_SECONDS(1));

	/* Create worker threads */
	k_thread_create(&producer_thread, producer_stack,
			K_THREAD_STACK_SIZEOF(producer_stack),
			producer_entry, NULL, NULL, NULL,
			5, 0, K_NO_WAIT);
	k_thread_name_set(&producer_thread, "producer");

	k_thread_create(&consumer_thread, consumer_stack,
			K_THREAD_STACK_SIZEOF(consumer_stack),
			consumer_entry, NULL, NULL, NULL,
			6, 0, K_NO_WAIT);
	k_thread_name_set(&consumer_thread, "consumer");

	k_thread_create(&sensor_thread, sensor_stack,
			K_THREAD_STACK_SIZEOF(sensor_stack),
			sensor_entry, NULL, NULL, NULL,
			7, 0, K_NO_WAIT);
	k_thread_name_set(&sensor_thread, "sensor");

	k_thread_create(&logger_thread, logger_stack,
			K_THREAD_STACK_SIZEOF(logger_stack),
			logger_entry, NULL, NULL, NULL,
			8, 0, K_NO_WAIT);
	k_thread_name_set(&logger_thread, "logger");

	k_thread_create(&watchdog_thread, watchdog_stack,
			K_THREAD_STACK_SIZEOF(watchdog_stack),
			watchdog_entry, NULL, NULL, NULL,
			9, 0, K_NO_WAIT);
	k_thread_name_set(&watchdog_thread, "watchdog");

	k_thread_create(&comms_thread, comms_stack,
			K_THREAD_STACK_SIZEOF(comms_stack),
			comms_entry, NULL, NULL, NULL,
			10, 0, K_NO_WAIT);
	k_thread_name_set(&comms_thread, "comms");

	k_thread_create(&storage_thread, storage_stack,
			K_THREAD_STACK_SIZEOF(storage_stack),
			storage_entry, NULL, NULL, NULL,
			11, 0, K_NO_WAIT);
	k_thread_name_set(&storage_thread, "storage");

	k_thread_create(&display_thread, display_stack,
			K_THREAD_STACK_SIZEOF(display_stack),
			display_entry, NULL, NULL, NULL,
			12, 0, K_NO_WAIT);
	k_thread_name_set(&display_thread, "display");

	k_thread_create(&audio_thread, audio_stack,
			K_THREAD_STACK_SIZEOF(audio_stack),
			audio_entry, NULL, NULL, NULL,
			13, 0, K_NO_WAIT);
	k_thread_name_set(&audio_thread, "audio");

	k_thread_create(&crypto_thread, crypto_stack,
			K_THREAD_STACK_SIZEOF(crypto_stack),
			crypto_entry, NULL, NULL, NULL,
			14, 0, K_NO_WAIT);
	k_thread_name_set(&crypto_thread, "crypto");

	k_thread_create(&network_thread, network_stack,
			K_THREAD_STACK_SIZEOF(network_stack),
			network_entry, NULL, NULL, NULL,
			7, 0, K_NO_WAIT);
	k_thread_name_set(&network_thread, "network");

	k_thread_create(&diagnostics_thread, diagnostics_stack,
			K_THREAD_STACK_SIZEOF(diagnostics_stack),
			diagnostics_entry, NULL, NULL, NULL,
			8, 0, K_NO_WAIT);
	k_thread_name_set(&diagnostics_thread, "diagnostics");

	LOG_INF("Threads started, tracing active");

	return 0;
}
