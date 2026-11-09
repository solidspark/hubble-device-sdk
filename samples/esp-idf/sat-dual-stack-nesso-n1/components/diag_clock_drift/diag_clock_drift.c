/*
 * Copyright (c) 2026 Hubble Network, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "driver/temperature_sensor.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "soc/soc.h"
#include "soc/usb_serial_jtag_reg.h"

#include <hubble/sat/packet.h>

#include "diag_clock_drift.h"

static const char *TAG = "drift";

#define WINDOW_MS (CONFIG_DIAG_CLOCK_DRIFT_WINDOW_S * 1000)
#define US_PER_FRAME 1000
/* USB_SERIAL_JTAG_SOF_FRAME_INDEX is 11 bits: it wraps every 2048 ms. */
#define SOF_FRAME_WRAP (USB_SERIAL_JTAG_SOF_FRAME_INDEX_V + 1)
/* Over one frame period, so a live bus always shows an edge. */
#define EDGE_SPIN_CAP_US 2000
/*
 * A loop pass takes about 1 us. A longer gap between the reads around the
 * edge means an interrupt or a task switch, so the edge time is unknown.
 */
#define EDGE_MAX_GAP_US 10
#define EDGE_TRIES 50
/* 20 us over a 1 ms frame: far more than any crystal or host error. */
#define EDGE_PAIR_TOL_US 20
/* Crystal and host are each good to tens of ppm; more is a bad window. */
#define PPM_SANE 200.0
#define EVENT_QUEUE_LEN 16
/* Preamble, header and the longest PDU: 8 + 6 + 46 symbols. */
#define MAX_SYMBOLS 64

enum edge_result {
	EDGE_OK,
	EDGE_NOSOF,
	EDGE_PREEMPTED,
};

enum event_kind {
	EVENT_TX,
	EVENT_HOPS,
};

/* One TX event before a packet, one HOPS event with what the radio was set to. */
struct event {
	enum event_kind kind;
	int64_t ms;
	uint32_t seq;
	uint8_t nsym;
	uint16_t mhz;
	uint8_t channel[HUBBLE_PACKET_FRAME_MAX_SIZE];
	uint8_t sym[MAX_SYMBOLS];
	int16_t offset[MAX_SYMBOLS];
};

static QueueHandle_t _event_q;
static temperature_sensor_handle_t _tsens;
static esp_pm_lock_handle_t _awake_lock;
static volatile float _temp_c = NAN;
static uint32_t _tx_seq;

/* Filled by the phy_set_freq wrap while a packet is on the air. */
static DRAM_ATTR volatile bool _recording;
static DRAM_ATTR volatile uint8_t _rec_n;
static DRAM_ATTR volatile uint16_t _rec_mhz;
static DRAM_ATTR int16_t _rec_offset[MAX_SYMBOLS];

static inline uint32_t _sof_frame(void)
{
	return REG_GET_FIELD(USB_SERIAL_JTAG_FRAM_NUM_REG,
			     USB_SERIAL_JTAG_SOF_FRAME_INDEX);
}

/*
 * No critical section on purpose: masking interrupts here could delay the
 * radio's symbol timer. A preempted edge is detected and retried instead.
 *
 * The index is written from the USB clock domain. Only the next index
 * counts as the edge, and t_last only moves on a read of the old one, so a
 * stray value can neither start nor time an edge.
 */
static enum edge_result _sof_edge_once(int64_t *t_us, uint32_t *frame)
{
	uint32_t start = _sof_frame();
	uint32_t next = (start + 1) & USB_SERIAL_JTAG_SOF_FRAME_INDEX_V;
	int64_t t_last = esp_timer_get_time();
	int64_t deadline = t_last + EDGE_SPIN_CAP_US;

	for (;;) {
		uint32_t f = _sof_frame();
		int64_t t = esp_timer_get_time();

		if (f == next) {
			if (t - t_last > EDGE_MAX_GAP_US) {
				return EDGE_PREEMPTED;
			}
			*t_us = t_last + (t - t_last) / 2;
			*frame = f;
			return EDGE_OK;
		}
		if (t > deadline) {
			/*
			 * Re-read: a preemption between the read and the timer
			 * leaves f stale and would look like a dead bus.
			 */
			return _sof_frame() == start ? EDGE_NOSOF
						     : EDGE_PREEMPTED;
		}
		if (f == start) {
			t_last = t;
		}
	}
}

/*
 * Two edges in a row must be one frame apart. That rejects an edge that
 * was timed wrong even though the reads around it looked clean.
 */
static enum edge_result _sof_edge(int64_t *t_us, uint32_t *frame)
{
	enum edge_result r = EDGE_PREEMPTED;

	for (int i = 0; i < EDGE_TRIES && r == EDGE_PREEMPTED; i++) {
		int64_t t2;
		uint32_t f2;

		r = _sof_edge_once(t_us, frame);
		if (r != EDGE_OK) {
			continue;
		}
		r = _sof_edge_once(&t2, &f2);
		if (r == EDGE_OK &&
		    (f2 != ((*frame + 1) & USB_SERIAL_JTAG_SOF_FRAME_INDEX_V) ||
		     llabs(t2 - *t_us - US_PER_FRAME) > EDGE_PAIR_TOL_US)) {
			printf("DRIFTDBG,pair,%lld,%lu,%lld,%lu\n", *t_us,
			       (unsigned long)*frame, t2, (unsigned long)f2);
			r = EDGE_PREEMPTED;
		}
	}
	return r;
}

static void _temp_update(void)
{
	float c;

	if (_tsens != NULL && temperature_sensor_get_celsius(_tsens, &c) == ESP_OK) {
		_temp_c = c;
	}
}

/* HOP,<ms>,<seq>,<hop>,<channel>,<mhz>,<symbols>,<offsets> (lists space-separated) */
static void _hops_print(const struct event *ev)
{
	for (uint8_t start = 0, hop = 0; start < ev->nsym;
	     start += HUBBLE_PACKET_FRAME_PAYLOAD_MAX_SIZE, hop++) {
		uint8_t end = start + HUBBLE_PACKET_FRAME_PAYLOAD_MAX_SIZE;

		if (end > ev->nsym) {
			end = ev->nsym;
		}
		printf("HOP,%lld,%lu,%u,%u,%u,", ev->ms, (unsigned long)ev->seq,
		       hop, ev->channel[hop], ev->mhz);
		for (uint8_t i = start; i < end; i++) {
			printf(i == start ? "%u" : " %u", ev->sym[i]);
		}
		printf(",");
		for (uint8_t i = start; i < end; i++) {
			printf(i == start ? "%d" : " %d", ev->offset[i]);
		}
		printf("\n");
	}
}

/* Waits out the window while printing TX and HOP events as they arrive. */
static void _tx_drain(int64_t until_us)
{
	static struct event ev;

	for (;;) {
		int64_t left_us = until_us - esp_timer_get_time();
		TickType_t ticks = left_us > 0 ? pdMS_TO_TICKS(left_us / 1000) : 0;

		if (xQueueReceive(_event_q, &ev, ticks) != pdTRUE) {
			return;
		}
		if (ev.kind == EVENT_TX) {
			printf("TX,%lld,%lu,%.1f\n", ev.ms,
			       (unsigned long)ev.seq, _temp_c);
		} else {
			_hops_print(&ev);
		}
	}
}

static void _lock(bool on)
{
	if (_awake_lock == NULL) {
		return;
	}
	if (on) {
		(void)esp_pm_lock_acquire(_awake_lock);
	} else {
		(void)esp_pm_lock_release(_awake_lock);
	}
}

static void _drift_task(void *arg)
{
	for (;;) {
		int64_t t0, t1;
		uint32_t f0, f1;
		enum edge_result r;

		/*
		 * Light sleep stops the crystal and esp_timer then steps by the
		 * RC-timed sleep length, so a window must stay awake.
		 */
		_lock(true);
		r = _sof_edge(&t0, &f0);
		if (r != EDGE_OK) {
			_lock(false);
			_temp_update();
			if (r == EDGE_NOSOF) {
				printf("DRIFT,%lld,nosof,,,%.1f,0\n",
				       esp_timer_get_time() / 1000, _temp_c);
			} else {
				printf("DRIFT,%lld,,,,%.1f,0\n",
				       esp_timer_get_time() / 1000, _temp_c);
			}
			_tx_drain(esp_timer_get_time() + WINDOW_MS * 1000LL);
			continue;
		}

		_tx_drain(t0 + WINDOW_MS * 1000LL);
		r = _sof_edge(&t1, &f1);
		_lock(false);
		_temp_update();

		if (r != EDGE_OK) {
			printf("DRIFT,%lld,,,,%.1f,0\n",
			       esp_timer_get_time() / 1000, _temp_c);
			continue;
		}

		int64_t ticks = t1 - t0;
		uint32_t delta = (f1 - f0) & USB_SERIAL_JTAG_SOF_FRAME_INDEX_V;
		/* Elapsed time resolves the wraps the 11-bit counter hides. */
		int64_t wraps = llround(((double)ticks / US_PER_FRAME - delta) /
					SOF_FRAME_WRAP);
		int64_t frames = delta + wraps * SOF_FRAME_WRAP;
		double ppm = frames > 0 ? ((double)ticks /
					   ((double)frames * US_PER_FRAME) -
					   1.0) * 1e6
					: NAN;
		bool valid = frames > 0 && fabs(ppm) < PPM_SANE;

		printf("DRIFT,%lld,%lld,%lld,%.3f,%.1f,%d\n", t1 / 1000, frames,
		       ticks, ppm, _temp_c, valid);
	}
}

esp_err_t diag_clock_drift_start(void)
{
	temperature_sensor_config_t tcfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
	esp_err_t err;

	_event_q = xQueueCreate(EVENT_QUEUE_LEN, sizeof(struct event));
	if (_event_q == NULL) {
		return ESP_ERR_NO_MEM;
	}

	err = temperature_sensor_install(&tcfg, &_tsens);
	if (err == ESP_OK) {
		err = temperature_sensor_enable(_tsens);
	}
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "No die temperature (%s)", esp_err_to_name(err));
		_tsens = NULL;
	}

	err = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "drift",
				 &_awake_lock);
	if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) {
		return err;
	}

	/* Idle priority: a spin must never delay the radio task. */
	if (xTaskCreate(_drift_task, "drift", 4096, NULL, tskIDLE_PRIORITY,
			NULL) != pdPASS) {
		return ESP_ERR_NO_MEM;
	}

	ESP_LOGI(TAG, "Clock drift diagnostic on: %d s windows, TX hold %d s",
		 CONFIG_DIAG_CLOCK_DRIFT_WINDOW_S, CONFIG_DIAG_TX_START_DELAY_S);
	return ESP_OK;
}

void __real_phy_set_freq(uint16_t freq_mhz, int offset);
void __wrap_phy_set_freq(uint16_t freq_mhz, int offset);

/*
 * In IRAM: the PHY library may call this itself, possibly with the flash
 * cache off. It only records while a satellite packet is being sent.
 */
void IRAM_ATTR __wrap_phy_set_freq(uint16_t freq_mhz, int offset)
{
	if (_recording && _rec_n < MAX_SYMBOLS) {
		_rec_mhz = freq_mhz;
		_rec_offset[_rec_n++] = (int16_t)offset;
	}
	__real_phy_set_freq(freq_mhz, offset);
}

int __real_hubble_sat_board_packet_send(
	const struct hubble_sat_packet_frames *packet);
int __wrap_hubble_sat_board_packet_send(
	const struct hubble_sat_packet_frames *packet);

int __wrap_hubble_sat_board_packet_send(
	const struct hubble_sat_packet_frames *packet)
{
	static struct event ev;
	int64_t hold_us = CONFIG_DIAG_TX_START_DELAY_S * 1000000LL -
			  esp_timer_get_time();

	if (hold_us > 0) {
		vTaskDelay(pdMS_TO_TICKS(hold_us / 1000) + 1);
	}

	/* Queued, not printed: console I/O here would shift the packet. */
	ev.kind = EVENT_TX;
	ev.ms = esp_timer_get_time() / 1000;
	ev.seq = ++_tx_seq;
	if (_event_q != NULL) {
		(void)xQueueSend(_event_q, &ev, 0);
	}

	_rec_n = 0;
	_recording = true;
	int ret = __real_hubble_sat_board_packet_send(packet);
	_recording = false;

	/* A copy of about 200 bytes before the SDK's gap to the next packet. */
	ev.kind = EVENT_HOPS;
	ev.nsym = _rec_n < packet->total_number_of_symbols
			  ? _rec_n
			  : (uint8_t)packet->total_number_of_symbols;
	ev.mhz = _rec_mhz;
	for (uint8_t i = 0; i < HUBBLE_PACKET_FRAME_MAX_SIZE; i++) {
		ev.channel[i] = packet->frame[i].channel;
	}
	for (uint8_t i = 0; i < ev.nsym; i++) {
		ev.sym[i] = packet->frame[i / HUBBLE_PACKET_FRAME_PAYLOAD_MAX_SIZE]
				    .data[i % HUBBLE_PACKET_FRAME_PAYLOAD_MAX_SIZE];
		ev.offset[i] = _rec_offset[i];
	}
	if (_event_q != NULL) {
		(void)xQueueSend(_event_q, &ev, 0);
	}

	return ret;
}
