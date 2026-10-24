/*
 * Copyright (c) 2026 Hubble Network, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "app_state.h"
#include "board.h"
#include "ui.h"

static const char *TAG = "ui";

extern const lv_image_dsc_t hubble_logo;

#define COLOR_BLACK    0x000000
#define COLOR_WHITE    0xFFFFFF
#define COLOR_BLURPLE  0x5761FF
#define COLOR_GREEN    0x19A948
#define COLOR_DIM      0x9E9E9E

/* Ring geometry: the dial fills the left square of the landscape panel. */
#define RING_CX        67
#define RING_CY        67
#define RING_R         52
#define ZEN_Y          (RING_CY - RING_R)
#define ZEN_D          14
#define SAT_D          9
#define RIPPLE_GROW    32

#define INFO_X         140
#define TICK_MS        100
#define PICK_SLIDE_MS  380
#define PICK_CLOSE_MS  450
#define RIPPLE_MS      1100
#define RIPPLE_GAP_MS  1200
#define BOOT_FILL_MS   1600

#define MS_PER_SEC     1000ULL
#define MS_PER_HOUR    (3600ULL * MS_PER_SEC)
#define MS_PER_DAY     (24ULL * MS_PER_HOUR)

static lv_display_t *_disp;

static lv_obj_t *_boot;
static lv_obj_t *_boot_fill;
static lv_obj_t *_prov;
static lv_obj_t *_run;

static lv_obj_t *_ring;
static lv_obj_t *_zen;
static lv_obj_t *_zen_dot;
static lv_obj_t *_ripple;
static lv_obj_t *_sats[APP_PASSES_SHOWN];

static lv_obj_t *_top_lbl;
static lv_obj_t *_big;
static lv_obj_t *_bot_lbl;
static lv_obj_t *_done_lbl;
static lv_obj_t *_info1;
static lv_obj_t *_info2;

static lv_obj_t *_picker;
static lv_obj_t *_pick_hl;

static enum app_phase _shown_phase = (enum app_phase)-1;
static enum app_radio _picked;
static uint32_t _last_count = UINT32_MAX;
static uint32_t _last_ripple_ms;

/* A bare container: no theme styles, no scrolling. */
static lv_obj_t *_box(lv_obj_t *parent, int x, int y, int w, int h)
{
	lv_obj_t *o = lv_obj_create(parent);

	lv_obj_remove_style_all(o);
	lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_pos(o, x, y);
	lv_obj_set_size(o, w, h);
	return o;
}

static lv_obj_t *_circle(lv_obj_t *parent, int d, uint32_t fill,
			 lv_opa_t fill_opa, uint32_t border, int border_w)
{
	lv_obj_t *o = _box(parent, 0, 0, d, d);

	lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(o, lv_color_hex(fill), 0);
	lv_obj_set_style_bg_opa(o, fill_opa, 0);
	lv_obj_set_style_border_color(o, lv_color_hex(border), 0);
	lv_obj_set_style_border_width(o, border_w, 0);
	return o;
}

static lv_obj_t *_label(lv_obj_t *parent, const lv_font_t *font,
			uint32_t color, int letter_space)
{
	lv_obj_t *l = lv_label_create(parent);

	lv_obj_set_style_text_font(l, font, 0);
	lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
	lv_obj_set_style_text_letter_space(l, letter_space, 0);
	lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
	return l;
}

static lv_obj_t *_logo(lv_obj_t *parent)
{
	lv_obj_t *img = lv_image_create(parent);

	lv_image_set_src(img, &hubble_logo);
	lv_obj_set_style_image_recolor(img, lv_color_hex(COLOR_WHITE), 0);
	lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
	return img;
}

static void _fmt_duration(char *buf, size_t len, uint64_t ms, bool *long_fmt)
{
	uint64_t s = (ms + 999) / MS_PER_SEC;
	unsigned h = s / 3600;
	unsigned m = (s % 3600) / 60;
	unsigned sec = s % 60;

	*long_fmt = h > 0;
	if (h > 0) {
		snprintf(buf, len, "%u:%02u:%02u", h, m, sec);
	} else {
		snprintf(buf, len, "%02u:%02u", m, sec);
	}
}

static void _fmt_thousands(char *buf, size_t len, uint32_t n)
{
	if (n >= 1000000) {
		snprintf(buf, len, "%lu,%03lu,%03lu", (unsigned long)n / 1000000,
			 (unsigned long)(n / 1000) % 1000,
			 (unsigned long)n % 1000);
	} else if (n >= 1000) {
		snprintf(buf, len, "%lu,%03lu", (unsigned long)n / 1000,
			 (unsigned long)n % 1000);
	} else {
		snprintf(buf, len, "%lu", (unsigned long)n);
	}
}

static void _fmt_utc_clock(char *buf, size_t len, uint64_t unix_ms)
{
	uint64_t s = (unix_ms % MS_PER_DAY) / MS_PER_SEC;

	snprintf(buf, len, "%02u:%02u", (unsigned)(s / 3600),
		 (unsigned)((s % 3600) / 60));
}

static void _label_set(lv_obj_t *l, const char *text)
{
	if (strcmp(lv_label_get_text(l), text) != 0) {
		lv_label_set_text(l, text);
	}
}

/* ---------- ripple from the top marker ---------- */

static void _ripple_exec(void *var, int32_t v)
{
	lv_obj_t *o = var;
	int d = ZEN_D + (RIPPLE_GROW * v) / 1000;

	lv_obj_set_size(o, d, d);
	lv_obj_set_pos(o, RING_CX - d / 2, ZEN_Y - d / 2);
	lv_obj_set_style_opa(o, (230 * (1000 - v)) / 1000, 0);
}

static void _ripple_fire(void)
{
	lv_anim_t a;

	lv_anim_delete(_ripple, _ripple_exec);
	lv_anim_init(&a);
	lv_anim_set_var(&a, _ripple);
	lv_anim_set_exec_cb(&a, _ripple_exec);
	lv_anim_set_values(&a, 0, 1000);
	lv_anim_set_duration(&a, RIPPLE_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_start(&a);
	_last_ripple_ms = lv_tick_get();
}

/* ---------- mode picker ---------- */

static void _pick_hl_exec(void *var, int32_t v)
{
	lv_obj_set_y(var, v);
}

static void _picker_close_cb(lv_timer_t *t)
{
	lv_obj_add_flag(_picker, LV_OBJ_FLAG_HIDDEN);
	app_radio_request(_picked);
}

static void _picker_choose(enum app_radio radio)
{
	lv_anim_t a;

	_picked = radio;
	lv_anim_init(&a);
	lv_anim_set_var(&a, _pick_hl);
	lv_anim_set_exec_cb(&a, _pick_hl_exec);
	lv_anim_set_values(&a, lv_obj_get_y(_pick_hl),
			   radio == APP_RADIO_TER ? BOARD_LCD_V_RES / 2 : 0);
	lv_anim_set_duration(&a, PICK_SLIDE_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_start(&a);

	lv_timer_set_repeat_count(
		lv_timer_create(_picker_close_cb, PICK_CLOSE_MS, NULL), 1);
}

static void _pick_sat_cb(lv_event_t *e)
{
	_picker_choose(APP_RADIO_SAT);
}

static void _pick_ter_cb(lv_event_t *e)
{
	_picker_choose(APP_RADIO_TER);
}

static void _zen_clicked_cb(lv_event_t *e)
{
	struct app_state s;

	app_state_get(&s);
	if (s.phase != APP_PHASE_COUNTDOWN &&
	    s.phase != APP_PHASE_TERRESTRIAL) {
		return;
	}

	lv_obj_set_y(_pick_hl, s.radio == APP_RADIO_TER ? BOARD_LCD_V_RES / 2
						     : 0);
	lv_obj_remove_flag(_picker, LV_OBJ_FLAG_HIDDEN);
	lv_obj_move_foreground(_picker);
}

static lv_obj_t *_pick_option(int y, const char *text, lv_event_cb_t cb)
{
	lv_obj_t *opt = _box(_picker, 0, y, BOARD_LCD_H_RES,
			     BOARD_LCD_V_RES / 2 + 1);
	lv_obj_t *l = _label(opt, &lv_font_montserrat_12, COLOR_WHITE, 4);

	lv_label_set_text(l, text);
	lv_obj_center(l);
	lv_obj_add_flag(opt, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(opt, cb, LV_EVENT_CLICKED, NULL);
	return opt;
}

static void _picker_build(lv_obj_t *scr)
{
	_picker = _box(scr, 0, 0, BOARD_LCD_H_RES, BOARD_LCD_V_RES);
	lv_obj_set_style_bg_color(_picker, lv_color_hex(COLOR_BLACK), 0);
	lv_obj_set_style_bg_opa(_picker, LV_OPA_COVER, 0);
	lv_obj_add_flag(_picker, LV_OBJ_FLAG_CLICKABLE);

	_pick_hl = _box(_picker, 0, 0, BOARD_LCD_H_RES,
			BOARD_LCD_V_RES / 2 + 1);
	lv_obj_set_style_bg_color(_pick_hl, lv_color_hex(COLOR_BLURPLE), 0);
	lv_obj_set_style_bg_opa(_pick_hl, LV_OPA_COVER, 0);

	_pick_option(0, "SATELLITE", _pick_sat_cb);
	_pick_option(BOARD_LCD_V_RES / 2, "TERRESTRIAL", _pick_ter_cb);
	lv_obj_add_flag(_picker, LV_OBJ_FLAG_HIDDEN);
}

/* ---------- layers ---------- */

static void _boot_fill_exec(void *var, int32_t v)
{
	lv_obj_set_width(var, v);
}

static void _boot_build(lv_obj_t *scr)
{
	lv_obj_t *logo;
	lv_obj_t *bar;
	lv_obj_t *lbl;
	lv_anim_t a;

	_boot = _box(scr, 0, 0, BOARD_LCD_H_RES, BOARD_LCD_V_RES);

	logo = _logo(_boot);
	lv_obj_align(logo, LV_ALIGN_CENTER, 0, -18);

	bar = _box(_boot, 0, 0, 64, 2);
	lv_obj_set_style_bg_color(bar, lv_color_hex(0x222222), 0);
	lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
	lv_obj_align(bar, LV_ALIGN_CENTER, 0, 12);

	_boot_fill = _box(bar, 0, 0, 0, 2);
	lv_obj_set_style_bg_color(_boot_fill, lv_color_hex(COLOR_WHITE), 0);
	lv_obj_set_style_bg_opa(_boot_fill, LV_OPA_COVER, 0);

	lbl = _label(_boot, &lv_font_montserrat_10, 0x777777, 3);
	lv_label_set_text(lbl, "LOADING");
	lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 28);

	lv_anim_init(&a);
	lv_anim_set_var(&a, _boot_fill);
	lv_anim_set_exec_cb(&a, _boot_fill_exec);
	lv_anim_set_values(&a, 0, 64);
	lv_anim_set_duration(&a, BOOT_FILL_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_start(&a);
}

static void _prov_build(lv_obj_t *scr)
{
	lv_obj_t *logo;
	lv_obj_t *l;

	_prov = _box(scr, 0, 0, BOARD_LCD_H_RES, BOARD_LCD_V_RES);

	logo = _logo(_prov);
	lv_obj_align(logo, LV_ALIGN_TOP_MID, 0, 18);

	l = _label(_prov, &lv_font_montserrat_10, COLOR_DIM, 3);
	lv_label_set_text(l, "WAITING FOR SETUP");
	lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 54);

	l = _label(_prov, &lv_font_montserrat_20, COLOR_WHITE, 0);
	lv_label_set_text(l, "Hubble-ESP");
	lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 72);

	l = _label(_prov, &lv_font_montserrat_10, COLOR_DIM, 1);
	lv_label_set_text(l, "Run dual-stack-companion.py");
	lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 104);
}

static void _run_build(lv_obj_t *scr)
{
	lv_obj_t *center;
	lv_obj_t *info;
	lv_obj_t *hit;

	_run = _box(scr, 0, 0, BOARD_LCD_H_RES, BOARD_LCD_V_RES);

	_ring = lv_arc_create(_run);
	lv_obj_set_size(_ring, RING_R * 2 + 2, RING_R * 2 + 2);
	lv_obj_set_pos(_ring, RING_CX - RING_R - 1, RING_CY - RING_R - 1);
	lv_obj_remove_style(_ring, NULL, LV_PART_KNOB);
	lv_obj_remove_flag(_ring, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_style_pad_all(_ring, 0, 0);
	lv_arc_set_rotation(_ring, 270);
	lv_arc_set_bg_angles(_ring, 0, 360);
	lv_arc_set_range(_ring, 0, 1000);
	lv_arc_set_value(_ring, 0);
	lv_obj_set_style_arc_width(_ring, 2, LV_PART_MAIN);
	lv_obj_set_style_arc_width(_ring, 2, LV_PART_INDICATOR);
	lv_obj_set_style_arc_rounded(_ring, false, LV_PART_INDICATOR);
	lv_obj_set_style_arc_color(_ring, lv_color_hex(COLOR_WHITE),
				   LV_PART_MAIN);
	lv_obj_set_style_arc_color(_ring, lv_color_hex(COLOR_WHITE),
				   LV_PART_INDICATOR);

	for (int i = APP_PASSES_SHOWN - 1; i >= 0; i--) {
		_sats[i] = _circle(_run, SAT_D, COLOR_BLACK, LV_OPA_COVER,
				   COLOR_BLURPLE, 1);
	}

	_ripple = _circle(_run, ZEN_D, COLOR_BLACK, LV_OPA_TRANSP,
			  COLOR_WHITE, 1);
	lv_obj_set_style_opa(_ripple, LV_OPA_TRANSP, 0);

	_zen = _circle(_run, ZEN_D, COLOR_BLACK, LV_OPA_COVER, COLOR_WHITE, 1);
	lv_obj_set_pos(_zen, RING_CX - ZEN_D / 2, ZEN_Y - ZEN_D / 2);
	_zen_dot = _circle(_zen, 5, COLOR_WHITE, LV_OPA_COVER, COLOR_WHITE, 0);
	lv_obj_center(_zen_dot);

	center = _box(_run, RING_CX - 48, RING_CY - 40, 96, 80);
	lv_obj_set_flex_flow(center, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(center, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(center, 4, 0);
	_top_lbl = _label(center, &lv_font_montserrat_10, COLOR_DIM, 2);
	_big = _label(center, &lv_font_montserrat_28, COLOR_WHITE, 0);
	_bot_lbl = _label(center, &lv_font_montserrat_10, COLOR_DIM, 2);
	_done_lbl = _label(center, &lv_font_montserrat_12, COLOR_WHITE, 3);
	lv_label_set_text(_done_lbl, "WINDOW\nCOMPLETE");
	lv_obj_set_style_text_line_space(_done_lbl, 6, 0);

	info = _box(_run, INFO_X, 0, BOARD_LCD_H_RES - INFO_X - 6,
		    BOARD_LCD_V_RES);
	lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(info, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
			      LV_FLEX_ALIGN_START);
	lv_obj_set_style_pad_row(info, 6, 0);
	_info1 = _label(info, &lv_font_montserrat_20, COLOR_WHITE, 0);
	_info2 = _label(info, &lv_font_montserrat_10, COLOR_DIM, 2);
	lv_obj_set_style_text_align(_info1, LV_TEXT_ALIGN_LEFT, 0);
	lv_obj_set_style_text_align(_info2, LV_TEXT_ALIGN_LEFT, 0);

	/* A finger is wider than the 14 px marker; give it a bigger target. */
	hit = _box(_run, RING_CX - 22, 0, 44, ZEN_Y + 20);
	lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(hit, _zen_clicked_cb, LV_EVENT_CLICKED, NULL);
}

/* ---------- per-phase rendering ---------- */

static void _bg_set(uint32_t color)
{
	lv_obj_t *scr = lv_screen_active();

	lv_obj_set_style_bg_color(scr, lv_color_hex(color), 0);
	lv_obj_set_style_bg_color(_zen, lv_color_hex(color), 0);
	for (int i = 0; i < APP_PASSES_SHOWN; i++) {
		lv_obj_set_style_bg_color(_sats[i], lv_color_hex(color), 0);
	}
}

static void _hidden(lv_obj_t *o, bool hidden)
{
	if (hidden) {
		lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
	}
}

static void _phase_enter(enum app_phase phase)
{
	bool running = phase >= APP_PHASE_COUNTDOWN;

	_hidden(_boot, phase != APP_PHASE_BOOT);
	_hidden(_prov, phase != APP_PHASE_PROVISIONING);
	_hidden(_run, !running);
	if (!running) {
		_hidden(_picker, true);
		return;
	}

	_bg_set(phase == APP_PHASE_TX     ? COLOR_BLURPLE
		: phase == APP_PHASE_DONE ? COLOR_GREEN
					  : COLOR_BLACK);
	lv_obj_set_style_arc_opa(_ring,
				 phase == APP_PHASE_TX ? LV_OPA_50
						       : LV_OPA_COVER,
				 LV_PART_MAIN);
	_hidden(_top_lbl, phase == APP_PHASE_DONE);
	_hidden(_big, phase == APP_PHASE_DONE);
	_hidden(_bot_lbl, phase == APP_PHASE_DONE);
	_hidden(_done_lbl, phase != APP_PHASE_DONE);
	for (int i = 0; i < APP_PASSES_SHOWN; i++) {
		_hidden(_sats[i], phase != APP_PHASE_COUNTDOWN);
	}

	lv_obj_set_size(_zen_dot, phase >= APP_PHASE_TX ? 7 : 5,
			phase >= APP_PHASE_TX ? 7 : 5);
	lv_obj_center(_zen_dot);

	if (phase == APP_PHASE_DONE) {
		lv_arc_set_value(_ring, 1000);
	} else if (phase != APP_PHASE_TX) {
		lv_arc_set_value(_ring, 0);
	}

	if (phase != APP_PHASE_COUNTDOWN && phase != APP_PHASE_TERRESTRIAL) {
		_hidden(_picker, true);
	}
	_last_count = UINT32_MAX;
}

/* The ring is a 24 h dial, counterclockwise from the top marker. */
static void _sats_place(const struct app_state *s, uint64_t now)
{
	for (int i = 0; i < APP_PASSES_SHOWN; i++) {
		float rem_h;
		float deg;
		float rad;

		if (i >= s->pass_count || s->pass_start_ms[i] <= now) {
			_hidden(_sats[i], true);
			continue;
		}

		rem_h = (float)(s->pass_start_ms[i] - now) / MS_PER_HOUR;
		deg = -90.0f - 14.0f - fminf(rem_h / 24.0f, 1.0f) * 346.0f;
		rad = deg * (float)M_PI / 180.0f;
		lv_obj_set_pos(_sats[i],
			       RING_CX + lroundf(RING_R * cosf(rad)) - SAT_D / 2,
			       RING_CY + lroundf(RING_R * sinf(rad)) -
				       SAT_D / 2);
		_hidden(_sats[i], false);
	}
}

static void _render_countdown(const struct app_state *s, uint64_t now)
{
	char buf[24];
	bool long_fmt;

	_fmt_duration(buf, sizeof(buf),
		      s->next_tx_ms > now ? s->next_tx_ms - now : 0,
		      &long_fmt);
	_label_set(_top_lbl, "NEXT PASS");
	lv_obj_set_style_text_font(_big, long_fmt ? &lv_font_montserrat_20
						  : &lv_font_montserrat_28,
				   0);
	_label_set(_big, buf);
	_label_set(_bot_lbl, long_fmt ? "HR MIN SEC" : "MIN  SEC");

	if (s->pass_count > 0) {
		_fmt_utc_clock(buf, sizeof(buf), s->pass_start_ms[0]);
		_label_set(_info1, buf);
		snprintf(buf, sizeof(buf), "UTC  %.0f° MAX",
			 s->pass_elevation[0]);
		_label_set(_info2, buf);
	}
	_sats_place(s, now);
}

static void _render_tx(const struct app_state *s, uint64_t now)
{
	char buf[16];
	uint32_t total = s->tx_total ? s->tx_total : 1;
	uint32_t sent =
		(now - s->tx_start_ms) / (s->tx_interval_s * MS_PER_SEC) + 1;

	if (sent > total) {
		sent = total;
	}

	if (sent != _last_count) {
		_last_count = sent;
		lv_arc_set_value(_ring, (sent * 1000) / total);
		_ripple_fire();
	}

	snprintf(buf, sizeof(buf), "%lu/%lu", (unsigned long)sent,
		 (unsigned long)total);
	_label_set(_top_lbl, "TRANSMITTING");
	lv_obj_set_style_text_font(_big, &lv_font_montserrat_28, 0);
	_label_set(_big, buf);
	_label_set(_bot_lbl, "PACKETS SENT");
	_label_set(_info1, "Overhead");
	snprintf(buf, sizeof(buf), "%.0f° MAX", s->pass_elevation[0]);
	_label_set(_info2, buf);
}

static void _render_done(const struct app_state *s)
{
	char buf[16];

	snprintf(buf, sizeof(buf), "%u/%u sent", s->tx_total, s->tx_total);
	_label_set(_info1, buf);
	_label_set(_info2, "SATELLITE");
}

static void _render_terrestrial(const struct app_state *s, uint64_t now)
{
	char buf[24];
	uint32_t count = s->beacons_before;

	if (s->beacon_since_ms != 0 && now > s->beacon_since_ms) {
		count += (now - s->beacon_since_ms) / s->beacon_interval_ms;
	}

	if (count != _last_count) {
		if (_last_count != UINT32_MAX &&
		    lv_tick_elaps(_last_ripple_ms) > RIPPLE_GAP_MS) {
			_ripple_fire();
		}
		_last_count = count;
	}

	_fmt_thousands(buf, sizeof(buf), count);
	_label_set(_top_lbl, "BEACONING");
	lv_obj_set_style_text_font(_big, &lv_font_montserrat_20, 0);
	_label_set(_big, buf);
	_label_set(_bot_lbl, "PACKETS TODAY");
	snprintf(buf, sizeof(buf), "Every %lu.%lu s",
		 (unsigned long)s->beacon_interval_ms / 1000,
		 (unsigned long)(s->beacon_interval_ms % 1000) / 100);
	_label_set(_info1, buf);
	_label_set(_info2, "BLE  0 DBM");
}

static void _tick_cb(lv_timer_t *t)
{
	struct app_state s;
	uint64_t now;

	app_state_get(&s);
	if (s.phase != _shown_phase) {
		_phase_enter(s.phase);
		_shown_phase = s.phase;
	}

	now = app_time_ms();
	switch (s.phase) {
	case APP_PHASE_COUNTDOWN:
		_render_countdown(&s, now);
		break;
	case APP_PHASE_TX:
		_render_tx(&s, now);
		break;
	case APP_PHASE_DONE:
		_render_done(&s);
		break;
	case APP_PHASE_TERRESTRIAL:
		_render_terrestrial(&s, now);
		break;
	default:
		break;
	}
}

/* ---------- LVGL plumbing ---------- */

static void _touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
	static uint16_t x;
	static uint16_t y;

	if (board_touch_read(&x, &y)) {
		data->state = LV_INDEV_STATE_PRESSED;
	} else {
		data->state = LV_INDEV_STATE_RELEASED;
	}
	data->point.x = x;
	data->point.y = y;
}

esp_err_t ui_init(void)
{
	const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
	const lvgl_port_display_cfg_t disp_cfg = {
		.io_handle = board_lcd_io(),
		.panel_handle = board_lcd_panel(),
		.buffer_size = BOARD_LCD_H_RES * 40,
		.double_buffer = true,
		.hres = BOARD_LCD_H_RES,
		.vres = BOARD_LCD_V_RES,
		.color_format = LV_COLOR_FORMAT_RGB565,
		.rotation =
			{
				.swap_xy = true,
				.mirror_x = true,
				.mirror_y = false,
			},
		.flags =
			{
				.buff_dma = true,
				.swap_bytes = true,
			},
	};
	lv_indev_t *indev;
	lv_obj_t *scr;

	ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl port");

	_disp = lvgl_port_add_disp(&disp_cfg);
	ESP_RETURN_ON_FALSE(_disp != NULL, ESP_FAIL, TAG, "lvgl display");

	if (!lvgl_port_lock(0)) {
		return ESP_FAIL;
	}

	indev = lv_indev_create();
	lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
	lv_indev_set_read_cb(indev, _touch_read_cb);
	lv_indev_set_display(indev, _disp);

	scr = lv_screen_active();
	lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_BLACK), 0);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

	_run_build(scr);
	_prov_build(scr);
	_boot_build(scr);
	_picker_build(scr);
	_phase_enter(APP_PHASE_BOOT);
	_shown_phase = APP_PHASE_BOOT;

	lv_timer_create(_tick_cb, TICK_MS, NULL);
	lvgl_port_unlock();

	return ESP_OK;
}
