/*
 * Copyright (c) 2025 Embedder
 * SPDX-License-Identifier: Apache-2.0
 *
 * nRF9160 DK GNSS Demo with Embedder Tracing
 *
 * Demonstrates real GNSS acquisition on the nRF9160 DK with
 * embedder-trace instrumentation:
 *   - Modem initialization and GNSS-only mode (LTE deactivated)
 *   - Semaphore-driven GNSS PVT processing thread
 *   - GNSS state machine tracing (searching/fix/blocked)
 *   - 11 background threads generating dense scheduling contention
 *   - LED heartbeat via GPIO timer
 *
 * No SIM card or network required -- runs in GNSS-only mode.
 * Uses the onboard GNSS antenna by default (CONFIG_MODEM_ANTENNA_GNSS_ONBOARD).
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/random/random.h>

#include <nrf_modem_gnss.h>
#include <modem/lte_lc.h>
#include <modem/nrf_modem_lib.h>

#include <embedder/trace.h>

LOG_MODULE_REGISTER(gnss_demo, LOG_LEVEL_INF);

/* --------------------------------------------------------------------------
 * User event channel IDs
 * -------------------------------------------------------------------------- */
#define CH_GNSS_SATS       1
#define CH_GNSS_LAT        2
#define CH_GNSS_LON        3
#define CH_GNSS_ALT        4
#define CH_GNSS_HDOP       5
#define CH_GNSS_STATE      6
#define CH_GNSS_PROCESS    7
#define CH_SENSOR_TEMP     8
#define CH_BATTERY_MV      9
#define CH_STORAGE_WRITE   10
#define CH_WATCHDOG_KICK   11
#define CH_CRYPTO_HASH     12
#define CH_STATUS_LOG      13
#define CH_COMMS_TX        14
#define CH_DISPLAY_REFRESH 15
#define CH_DIAG_CHECK      16
#define CH_POWER_STATE     17

/* --------------------------------------------------------------------------
 * GNSS state machine
 * -------------------------------------------------------------------------- */
enum gnss_state {
	GNSS_STATE_INITIALIZING = 0,
	GNSS_STATE_SEARCHING    = 1,
	GNSS_STATE_FIX_ACQUIRED = 2,
	GNSS_STATE_BLOCKED      = 3,
};

static enum gnss_state current_gnss_state = GNSS_STATE_INITIALIZING;

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
 * GNSS data and synchronization
 * -------------------------------------------------------------------------- */
static struct nrf_modem_gnss_pvt_data_frame pvt_data;
static K_SEM_DEFINE(gnss_sem, 0, 1);
static bool fix_valid;

static void gnss_event_handler(int event_id)
{
	switch (event_id) {
	case NRF_MODEM_GNSS_EVT_PVT:
		k_sem_give(&gnss_sem);
		break;
	case NRF_MODEM_GNSS_EVT_FIX:
		fix_valid = true;
		break;
	case NRF_MODEM_GNSS_EVT_BLOCKED:
		current_gnss_state = GNSS_STATE_BLOCKED;
		break;
	case NRF_MODEM_GNSS_EVT_UNBLOCKED:
		current_gnss_state = GNSS_STATE_SEARCHING;
		break;
	default:
		break;
	}
}

/* --------------------------------------------------------------------------
 * GNSS processing thread
 * -------------------------------------------------------------------------- */
#define GNSS_STACK_SIZE 2048
K_THREAD_STACK_DEFINE(gnss_stack, GNSS_STACK_SIZE);
static struct k_thread gnss_thread;

static int count_tracked_svs(const struct nrf_modem_gnss_pvt_data_frame *pvt)
{
	int count = 0;

	for (int i = 0; i < NRF_MODEM_GNSS_MAX_SATELLITES; i++) {
		if (pvt->sv[i].sv > 0) {
			count++;
		}
	}
	return count;
}

static void gnss_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	int err;

	while (true) {
		k_sem_take(&gnss_sem, K_FOREVER);

		embedder_trace_interval_begin(CH_GNSS_PROCESS);

		err = nrf_modem_gnss_read(&pvt_data, sizeof(pvt_data),
					  NRF_MODEM_GNSS_DATA_PVT);
		if (err) {
			LOG_WRN("Failed to read PVT data: %d", err);
			embedder_trace_interval_end(CH_GNSS_PROCESS);
			continue;
		}

		int sv_count = count_tracked_svs(&pvt_data);

		embedder_trace_counter(CH_GNSS_SATS, sv_count);
		embedder_trace_sample_f32(CH_GNSS_HDOP, pvt_data.hdop);

		if (pvt_data.flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID) {
			if (current_gnss_state != GNSS_STATE_FIX_ACQUIRED) {
				current_gnss_state = GNSS_STATE_FIX_ACQUIRED;
				embedder_trace_state(CH_GNSS_STATE,
						     GNSS_STATE_FIX_ACQUIRED);
			}

			embedder_trace_sample_f32(CH_GNSS_LAT,
						  (float)pvt_data.latitude);
			embedder_trace_sample_f32(CH_GNSS_LON,
						  (float)pvt_data.longitude);
			embedder_trace_sample_f32(CH_GNSS_ALT,
						  pvt_data.altitude);

			LOG_INF("Fix: lat=%.6f lon=%.6f alt=%.1f sats=%d hdop=%.1f",
				pvt_data.latitude, pvt_data.longitude,
				(double)pvt_data.altitude, sv_count,
				(double)pvt_data.hdop);
		} else {
			if (current_gnss_state == GNSS_STATE_FIX_ACQUIRED) {
				current_gnss_state = GNSS_STATE_SEARCHING;
				embedder_trace_state(CH_GNSS_STATE,
						     GNSS_STATE_SEARCHING);
			}
			LOG_INF("Searching: sats=%d hdop=%.1f",
				sv_count, (double)pvt_data.hdop);
		}

		/* Trace blocked/unblocked state changes from the handler */
		if (current_gnss_state == GNSS_STATE_BLOCKED) {
			embedder_trace_state(CH_GNSS_STATE, GNSS_STATE_BLOCKED);
		}

		embedder_trace_interval_end(CH_GNSS_PROCESS);
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

/* Sensor: simulated temperature reading every 2-4s */
static void sensor_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
	int32_t temp = 2200; /* centidegrees C */

	while (true) {
		embedder_trace_interval_begin(CH_SENSOR_TEMP);
		k_busy_wait(5000);
		temp += (sys_rand32_get() % 100) - 50;
		embedder_trace_counter(CH_SENSOR_TEMP, temp);
		embedder_trace_interval_end(CH_SENSOR_TEMP);
		LOG_INF("Sensor: temp=%d.%02d C", temp / 100, temp % 100);
		k_sleep(K_MSEC(200 + (sys_rand32_get() % 200)));
	}
}

/* Battery: simulated voltage monitor every 5s */
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

/* Storage: simulated flash write every 4-8s */
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

/* Watchdog: simulated kick every 3-5s */
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

/* Crypto: simulated hash computation every 2-5s */
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

/* Status logger: system stats every 3s */
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

/* Comms: simulated packet TX every 1-2s */
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
		k_sleep(K_MSEC(100 + (sys_rand32_get() % 100)));
	}
}

/* Display: simulated refresh every 3-6s */
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

/* Diagnostics: self-check every 5-9s */
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

/* Power manager: simulated sleep policy every 10s */
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

	LOG_INF("nRF9160 DK GNSS Demo started");

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

	/* ---- Modem initialization ---- */
	LOG_INF("Initializing modem...");
	err = nrf_modem_lib_init();
	if (err) {
		LOG_ERR("Modem library init failed: %d", err);
		return err;
	}
	LOG_INF("Modem initialized");

	/* Deactivate LTE -- GNSS-only mode, no SIM required */
	err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_DEACTIVATE_LTE);
	if (err) {
		LOG_ERR("Failed to deactivate LTE: %d", err);
		return err;
	}

	/* Activate GNSS */
	err = lte_lc_func_mode_set(LTE_LC_FUNC_MODE_ACTIVATE_GNSS);
	if (err) {
		LOG_ERR("Failed to activate GNSS: %d", err);
		return err;
	}
	LOG_INF("LTE deactivated, GNSS activated");

	/* ---- Configure GNSS ---- */
	err = nrf_modem_gnss_event_handler_set(gnss_event_handler);
	if (err) {
		LOG_ERR("Failed to set GNSS event handler: %d", err);
		return err;
	}

	/* Continuous 1Hz navigation */
	nrf_modem_gnss_fix_interval_set(1);
	nrf_modem_gnss_fix_retry_set(0);

	/* 5-degree elevation mask */
	nrf_modem_gnss_elevation_threshold_set(5);

	/* Performance use case */
	nrf_modem_gnss_use_case_set(NRF_MODEM_GNSS_USE_CASE_MULTIPLE_HOT_START);

	/* No power saving */
	nrf_modem_gnss_power_mode_set(NRF_MODEM_GNSS_PSM_DISABLED);

	LOG_INF("GNSS configured: 1Hz continuous, 5deg elevation");

	/* ---- Register trace channel metadata ---- */
	embedder_trace_channel_meta(CH_GNSS_SATS, "gnss_sats", "");
	embedder_trace_channel_meta(CH_GNSS_LAT, "gnss_lat", "deg");
	embedder_trace_channel_meta(CH_GNSS_LON, "gnss_lon", "deg");
	embedder_trace_channel_meta(CH_GNSS_ALT, "gnss_alt", "m");
	embedder_trace_channel_meta(CH_GNSS_HDOP, "gnss_hdop", "");
	embedder_trace_channel_meta(CH_GNSS_STATE, "gnss_state", "");
	embedder_trace_channel_meta(CH_GNSS_PROCESS, "gnss_process", "us");
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
	embedder_trace_app_metadata("board", "nrf9160dk");
	embedder_trace_app_metadata("mode", "gnss_only");

	/* Start continuous capture */
	embedder_trace_capture_start("gnss_demo");

	/* Emit initial state */
	current_gnss_state = GNSS_STATE_SEARCHING;
	embedder_trace_state(CH_GNSS_STATE, GNSS_STATE_SEARCHING);

	/* ---- Start heartbeat timer ---- */
	k_timer_start(&heartbeat_timer, K_SECONDS(1), K_SECONDS(1));

	/* ---- Create GNSS processing thread ---- */
	k_thread_create(&gnss_thread, gnss_stack,
			K_THREAD_STACK_SIZEOF(gnss_stack),
			gnss_entry, NULL, NULL, NULL,
			3, 0, K_NO_WAIT);
	k_thread_name_set(&gnss_thread, "gnss");

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

	/* ---- Start GNSS ---- */
	err = nrf_modem_gnss_start();
	if (err) {
		LOG_ERR("Failed to start GNSS: %d", err);
		return err;
	}

	LOG_INF("GNSS started, %d threads active, tracing enabled",
		11 /* gnss + 10 workers */);

	return 0;
}
