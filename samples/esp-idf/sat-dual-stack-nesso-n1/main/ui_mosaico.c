/*
 * Copyright (c) 2026 Hubble Network, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * ESP-Mosaico touch UI, after the "Mosaico pass demo" mockup. The mockup is
 * drawn at 300 x 300; every size here is that times 1.6 for the 480 x 480
 * panel.
 *
 *   countdown  black; a 24 h dial runs counterclockwise from the top marker
 *              and each upcoming pass sits on it at its own time
 *   transmit   blurple; the ring fills as packets go out
 *   done       green; full ring, then back to the countdown
 *   terrestrial black; beacons sent today, the marker ripples per beacon
 *
 * Tap the top marker for the Satellite / Terrestrial picker. Swipe up (or
 * tap the bar at the bottom) for passes, transmission settings and payload;
 * swipe down on the sheet's handle to close it, which saves any change.
 */

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "app_state.h"
#include "board.h"
#include "ui.h"

static const char *TAG = "ui";

#define MS_PER_SEC      1000U

#define COLOR_BLACK     0x000000
#define COLOR_WHITE     0xFFFFFF
#define COLOR_BLURPLE   0x5761FF
#define COLOR_GREEN     0x19A948
#define COLOR_TRACK_BG  0x222222
#define COLOR_SUB       0x777777

/* Geometry: the mockup's 300 px screen scaled by 1.6 */
#define CX              240
#define CY              240
#define RING_R          195
#define RING_W          3
#define ZEN_Y           45
#define ZEN_D           44
#define ZEN_DOT_D       16
#define ZEN_DOT_TX_D    22
#define ZEN_HIT_R       42
#define SAT_D           22
#define SHEET_H         461
#define PICK_H          (BOARD_LCD_V_RES / 2)

#define TICK_MS         100
#define SCREEN_IDLE_MS  30000
#define PASS_LEAD_MS    10000
#define BG_FADE_MS      700
#define SHEET_ANIM_MS   380
#define PICK_CLOSE_MS   450
#define RIPPLE_MS       1100
#define RIPPLE_GAP_MS   1200
#define FORGET_ARM_MS   3000
#define TX_BUZZ_MS      80

#define BEACON_MIN_MS   500
#define BEACON_MAX_MS   10000
#define BEACON_STEP_MS  500

extern const uint8_t _font_200_start[] asm("_binary_manrope_200_ttf_start");
extern const uint8_t _font_200_end[] asm("_binary_manrope_200_ttf_end");
extern const uint8_t _font_400_start[] asm("_binary_manrope_400_ttf_start");
extern const uint8_t _font_400_end[] asm("_binary_manrope_400_ttf_end");
extern const lv_image_dsc_t hubble_logo_48;

struct fonts {
	const lv_font_t *big;   /* 66 px in the mockup */
	const lv_font_t *long_; /* 46 */
	const lv_font_t *small; /* 30 */
	const lv_font_t *lbl;   /* 10.5 */
	const lv_font_t *done;  /* 15 */
	const lv_font_t *title; /* 9.5 */
	const lv_font_t *row;   /* 12 */
	const lv_font_t *sub;   /* 10 */
	const lv_font_t *ptime; /* 13 */
	const lv_font_t *step;  /* 16 */
	const lv_font_t *prov;  /* setup screen name */
};

static struct fonts _f;
static lv_display_t *_disp;
static lv_obj_t *_scr;
static lv_timer_t *_ui_timer;

static lv_obj_t *_boot, *_boot_fill, *_boot_sub;
static lv_obj_t *_prov;
static lv_obj_t *_run;
static lv_obj_t *_track, *_prog, *_zen, *_zen_dot, *_ripple;
static lv_obj_t *_sats[APP_PASSES_SHOWN];
static lv_obj_t *_center, *_top_lbl, *_big_row, *_big, *_big_small,
	*_bot_lbl, *_done_msg;
static lv_obj_t *_home_bar;
static lv_obj_t *_picker, *_pick_block, *_pick_sat, *_pick_ter;
static lv_obj_t *_sheet, *_sheet_name, *_sheet_right, *_sheet_body;
static lv_obj_t *_sat_cfg, *_ter_cfg;
static lv_obj_t *_pass_rows[APP_PASSES_SHOWN];
static lv_obj_t *_pass_t[APP_PASSES_SHOWN], *_pass_k[APP_PASSES_SHOWN],
	*_pass_sub[APP_PASSES_SHOWN], *_pass_in[APP_PASSES_SHOWN];
static lv_obj_t *_cnt_out, *_iv_out, *_win_len, *_mode_val;
static lv_obj_t *_ter_today, *_biv_out, *_home_val;
static lv_obj_t *_pay_temp, *_pay_bat, *_pay_size, *_pay_hex;
static lv_obj_t *_setup_val, *_forget_lbl;
static lv_obj_t *_waker;

static enum app_phase _shown_phase = (enum app_phase)-1;
static lv_color_t _bg_now;
static lv_color_t _bg_from, _bg_to;
static bool _screen_off;
static bool _sheet_is_open;
static bool _prefs_dirty;
static uint32_t _last_activity;
static uint32_t _last_count = UINT32_MAX;
static uint32_t _last_beacons = UINT32_MAX;
static uint32_t _last_ripple;
static uint32_t _forget_armed_at;
static bool _key_was_down;
static char _big_text[24];

/* ---------- small helpers ---------- */

static const lv_font_t *_font(bool light, int32_t px)
{
	const uint8_t *start = light ? _font_200_start : _font_400_start;
	const uint8_t *end = light ? _font_200_end : _font_400_end;
	lv_font_t *f = lv_tiny_ttf_create_data_ex(start, end - start, px,
						  LV_FONT_KERNING_NORMAL, 64);

	return f != NULL ? f : &lv_font_montserrat_14;
}

static void _hidden(lv_obj_t *obj, bool hidden)
{
	if (hidden) {
		lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
	}
}

static lv_obj_t *_box(lv_obj_t *parent, int32_t x, int32_t y, int32_t w,
		      int32_t h)
{
	lv_obj_t *o = lv_obj_create(parent);

	lv_obj_remove_style_all(o);
	lv_obj_set_pos(o, x, y);
	lv_obj_set_size(o, w, h);
	lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
	return o;
}

static lv_obj_t *_circle(lv_obj_t *parent, int32_t cx, int32_t cy, int32_t d)
{
	lv_obj_t *o = _box(parent, cx - d / 2, cy - d / 2, d, d);

	lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
	lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
	return o;
}

static lv_obj_t *_label(lv_obj_t *parent, const lv_font_t *font,
			uint32_t color, lv_opa_t opa, int32_t spacing)
{
	lv_obj_t *l = lv_label_create(parent);

	lv_obj_set_style_text_font(l, font, 0);
	lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
	lv_obj_set_style_text_opa(l, opa, 0);
	lv_obj_set_style_text_letter_space(l, spacing, 0);
	return l;
}

static void _label_set(lv_obj_t *l, const char *text)
{
	if (strcmp(lv_label_get_text(l), text) != 0) {
		lv_label_set_text(l, text);
	}
}

static void _fmt_dur(char *buf, size_t len, int64_t ms)
{
	int64_t s = ms <= 0 ? 0 : (ms + MS_PER_SEC - 1) / MS_PER_SEC;
	int h = (int)(s / 3600);
	int m = (int)(s % 3600 / 60);
	int sec = (int)(s % 60);

	if (h > 0) {
		snprintf(buf, len, "%d:%02d:%02d", h, m, sec);
	} else {
		snprintf(buf, len, "%02d:%02d", m, sec);
	}
}

static void _fmt_clock(char *buf, size_t len, uint64_t unix_ms)
{
	time_t t = (time_t)(unix_ms / MS_PER_SEC);
	struct tm tm;

	localtime_r(&t, &tm);
	strftime(buf, len, "%H:%M", &tm);
}

/* ---------- screen power ---------- */

static void _screen_set(bool on)
{
	_last_activity = lv_tick_get();
	if (on == !_screen_off) {
		return;
	}
	_screen_off = !on;
	_hidden(_waker, on);
	if (on) {
		lv_obj_invalidate(_scr);
		board_backlight_set(true);
	} else {
		_hidden(_picker, true);
		lv_obj_move_foreground(_waker);
		board_backlight_set(false);
	}
}

static void _waker_cb(lv_event_t *e)
{
	(void)e;
	_screen_set(true);
}

/* ---------- background fade ---------- */

static void _bg_anim_cb(void *var, int32_t v)
{
	(void)var;
	_bg_now = lv_color_mix(_bg_to, _bg_from, (uint8_t)v);
	lv_obj_set_style_bg_color(_scr, _bg_now, 0);
	lv_obj_set_style_bg_color(_zen, _bg_now, 0);
	for (int i = 0; i < APP_PASSES_SHOWN; i++) {
		lv_obj_set_style_bg_color(_sats[i], _bg_now, 0);
	}
}

static void _bg_set(uint32_t hex)
{
	lv_anim_t a;

	_bg_from = _bg_now;
	_bg_to = lv_color_hex(hex);
	lv_anim_delete(&_bg_now, _bg_anim_cb);
	lv_anim_init(&a);
	lv_anim_set_var(&a, &_bg_now);
	lv_anim_set_exec_cb(&a, _bg_anim_cb);
	lv_anim_set_values(&a, 0, 255);
	lv_anim_set_duration(&a, BG_FADE_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
	lv_anim_start(&a);
}

/* ---------- ripple from the top marker ---------- */

static void _ripple_anim_cb(void *var, int32_t v)
{
	int32_t d = ZEN_D + (ZEN_D * 22 * v) / 10000;

	lv_obj_set_size(var, d, d);
	lv_obj_set_pos(var, CX - d / 2, ZEN_Y - d / 2);
	lv_obj_set_style_border_opa(var, (lv_opa_t)(230 - (230 * v) / 1000),
				    0);
}

static void _ripple_fire(void)
{
	lv_anim_t a;

	_last_ripple = lv_tick_get();
	_hidden(_ripple, false);
	lv_anim_delete(_ripple, _ripple_anim_cb);
	lv_anim_init(&a);
	lv_anim_set_var(&a, _ripple);
	lv_anim_set_exec_cb(&a, _ripple_anim_cb);
	lv_anim_set_values(&a, 0, 1000);
	lv_anim_set_duration(&a, RIPPLE_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_start(&a);
}

/* ---------- boot and setup screens ---------- */

static void _boot_fill_cb(void *var, int32_t v)
{
	lv_obj_set_width(var, v);
}

static void _boot_build(void)
{
	lv_obj_t *logo, *bar;
	lv_anim_t a;

	_boot = _box(_scr, 0, 0, BOARD_LCD_H_RES, BOARD_LCD_V_RES);

	logo = lv_image_create(_boot);
	lv_image_set_src(logo, &hubble_logo_48);
	lv_obj_set_style_image_recolor(logo, lv_color_hex(COLOR_WHITE), 0);
	lv_obj_set_style_image_recolor_opa(logo, LV_OPA_COVER, 0);
	lv_obj_align(logo, LV_ALIGN_CENTER, 0, -40);

	bar = _box(_boot, CX - 96, CY + 24, 192, 3);
	lv_obj_set_style_bg_color(bar, lv_color_hex(COLOR_TRACK_BG), 0);
	lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(bar, 2, 0);

	_boot_fill = _box(bar, 0, 0, 0, 3);
	lv_obj_set_style_bg_color(_boot_fill, lv_color_hex(COLOR_WHITE), 0);
	lv_obj_set_style_bg_opa(_boot_fill, LV_OPA_COVER, 0);

	_boot_sub = _label(_boot, _f.title, COLOR_SUB, LV_OPA_COVER, 4);
	lv_label_set_text(_boot_sub, "LOADING");
	lv_obj_align(_boot_sub, LV_ALIGN_CENTER, 0, 56);

	lv_anim_init(&a);
	lv_anim_set_var(&a, _boot_fill);
	lv_anim_set_exec_cb(&a, _boot_fill_cb);
	lv_anim_set_values(&a, 0, 192);
	lv_anim_set_duration(&a, 1600);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_start(&a);
}

static void _prov_build(void)
{
	lv_obj_t *logo, *l;

	_prov = _box(_scr, 0, 0, BOARD_LCD_H_RES, BOARD_LCD_V_RES);

	logo = lv_image_create(_prov);
	lv_image_set_src(logo, &hubble_logo_48);
	lv_obj_set_style_image_recolor(logo, lv_color_hex(COLOR_WHITE), 0);
	lv_obj_set_style_image_recolor_opa(logo, LV_OPA_COVER, 0);
	lv_obj_align(logo, LV_ALIGN_CENTER, 0, -90);

	l = _label(_prov, _f.lbl, COLOR_WHITE, LV_OPA_60, 5);
	lv_label_set_text(l, "WAITING FOR SETUP");
	lv_obj_align(l, LV_ALIGN_CENTER, 0, -24);

	l = _label(_prov, _f.prov, COLOR_WHITE, LV_OPA_COVER, 0);
	lv_label_set_text(l, "Hubble-ESP");
	lv_obj_align(l, LV_ALIGN_CENTER, 0, 20);

	l = _label(_prov, _f.row, COLOR_WHITE, LV_OPA_60, 0);
	lv_label_set_text(l, "Run dual-stack-companion.py");
	lv_obj_align(l, LV_ALIGN_CENTER, 0, 70);
}

/* ---------- ring ---------- */

static lv_obj_t *_arc(lv_obj_t *parent, lv_opa_t main_opa)
{
	lv_obj_t *a = lv_arc_create(parent);
	int32_t d = 2 * RING_R + RING_W;

	lv_obj_set_size(a, d, d);
	lv_obj_set_pos(a, CX - d / 2, CY - d / 2);
	lv_arc_set_rotation(a, 270);
	lv_arc_set_bg_angles(a, 0, 360);
	lv_arc_set_range(a, 0, 1000);
	lv_arc_set_value(a, 0);
	lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_style_pad_all(a, 0, 0);
	lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, LV_PART_KNOB);
	lv_obj_set_style_pad_all(a, 0, LV_PART_KNOB);
	lv_obj_set_style_arc_width(a, RING_W, LV_PART_MAIN);
	lv_obj_set_style_arc_width(a, RING_W, LV_PART_INDICATOR);
	lv_obj_set_style_arc_rounded(a, false, LV_PART_MAIN);
	lv_obj_set_style_arc_rounded(a, false, LV_PART_INDICATOR);
	lv_obj_set_style_arc_color(a, lv_color_hex(COLOR_WHITE), LV_PART_MAIN);
	lv_obj_set_style_arc_opa(a, main_opa, LV_PART_MAIN);
	lv_obj_set_style_arc_color(a, lv_color_hex(COLOR_WHITE),
				   LV_PART_INDICATOR);
	return a;
}

static void _sat_place(int i, double rem_h)
{
	double frac = rem_h / 24.0;
	double deg;
	double rad;

	if (frac < 0) {
		frac = 0;
	} else if (frac > 1) {
		frac = 1;
	}
	/* 14 degrees keeps the next pass clear of the marker. */
	deg = -90.0 - 14.0 - frac * 346.0;
	rad = deg * M_PI / 180.0;
	lv_obj_set_pos(_sats[i], CX + (int32_t)lround(RING_R * cos(rad)) - SAT_D / 2,
		       CY + (int32_t)lround(RING_R * sin(rad)) - SAT_D / 2);
}

static void _zen_dot_size(int32_t d)
{
	lv_obj_set_size(_zen_dot, d, d);
	lv_obj_set_pos(_zen_dot, CX - d / 2, ZEN_Y - d / 2);
}

static void _run_build(void)
{
	lv_obj_t *l;

	_run = _box(_scr, 0, 0, BOARD_LCD_H_RES, BOARD_LCD_V_RES);

	/* The track is the arc's background; _prog draws only its indicator. */
	_track = _arc(_run, LV_OPA_COVER);
	lv_obj_set_style_arc_opa(_track, LV_OPA_TRANSP, LV_PART_INDICATOR);
	_prog = _arc(_run, LV_OPA_TRANSP);

	for (int i = APP_PASSES_SHOWN - 1; i >= 0; i--) {
		_sats[i] = _circle(_run, CX, CY - RING_R, SAT_D);
		lv_obj_set_style_bg_opa(_sats[i], LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(_sats[i], lv_color_hex(COLOR_BLACK), 0);
		lv_obj_set_style_border_color(_sats[i],
					      lv_color_hex(COLOR_BLURPLE), 0);
		lv_obj_set_style_border_width(_sats[i], 3, 0);
	}

	_ripple = _circle(_run, CX, ZEN_Y, ZEN_D);
	lv_obj_set_style_border_color(_ripple, lv_color_hex(COLOR_WHITE), 0);
	lv_obj_set_style_border_width(_ripple, 2, 0);
	lv_obj_set_style_border_opa(_ripple, LV_OPA_TRANSP, 0);

	_zen = _circle(_run, CX, ZEN_Y, ZEN_D);
	lv_obj_set_style_bg_opa(_zen, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(_zen, lv_color_hex(COLOR_BLACK), 0);
	lv_obj_set_style_border_color(_zen, lv_color_hex(COLOR_WHITE), 0);
	lv_obj_set_style_border_width(_zen, RING_W, 0);

	_zen_dot = _circle(_run, CX, ZEN_Y, ZEN_DOT_D);
	lv_obj_set_style_bg_opa(_zen_dot, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(_zen_dot, lv_color_hex(COLOR_WHITE), 0);

	_center = _box(_run, 0, 0, BOARD_LCD_H_RES, BOARD_LCD_V_RES);
	lv_obj_remove_flag(_center, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_flex_flow(_center, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(_center, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	_top_lbl = _label(_center, _f.lbl, COLOR_WHITE, 158, 5);
	lv_label_set_text(_top_lbl, "NEXT PASS");

	_big_row = _box(_center, 0, 0, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_remove_flag(_big_row, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_flex_flow(_big_row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(_big_row, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
	lv_obj_set_style_pad_ver(_big_row, 16, 0);
	_big = _label(_big_row, _f.big, COLOR_WHITE, LV_OPA_COVER, -2);
	lv_label_set_text(_big, "--:--");
	_big_small = _label(_big_row, _f.small, COLOR_WHITE, LV_OPA_60, 0);
	lv_obj_set_style_pad_bottom(_big_small, 10, 0);
	lv_label_set_text(_big_small, "");

	_bot_lbl = _label(_center, _f.lbl, COLOR_WHITE, 158, 5);
	lv_label_set_text(_bot_lbl, "MIN \xC2\xB7 SEC");

	_done_msg = _label(_center, _f.done, COLOR_WHITE, LV_OPA_COVER, 7);
	lv_label_set_text(_done_msg, "WINDOW COMPLETE");

	_home_bar = _box(_run, CX - 35, BOARD_LCD_V_RES - 16, 70, 5);
	lv_obj_set_style_radius(_home_bar, 3, 0);
	lv_obj_set_style_bg_color(_home_bar, lv_color_hex(COLOR_WHITE), 0);
	lv_obj_set_style_bg_opa(_home_bar, 71, 0);
	lv_obj_add_flag(_home_bar, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_ext_click_area(_home_bar, 20);

	(void)l;
}

/* ---------- picker ---------- */

static void _pick_block_cb(void *var, int32_t v)
{
	lv_obj_set_y(var, v);
}

static void _pick_show(enum app_radio radio, bool animate)
{
	int32_t y = radio == APP_RADIO_SAT ? 0 : PICK_H;

	lv_anim_delete(_pick_block, _pick_block_cb);
	if (!animate) {
		lv_obj_set_y(_pick_block, y);
		return;
	}

	lv_anim_t a;

	lv_anim_init(&a);
	lv_anim_set_var(&a, _pick_block);
	lv_anim_set_exec_cb(&a, _pick_block_cb);
	lv_anim_set_values(&a, lv_obj_get_y(_pick_block), y);
	lv_anim_set_duration(&a, SHEET_ANIM_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_start(&a);
}

static void _pick_close_cb(lv_timer_t *t)
{
	enum app_radio radio =
		(enum app_radio)(uintptr_t)lv_timer_get_user_data(t);
	struct app_state s;

	app_state_get(&s);
	_hidden(_picker, true);
	if (s.radio != radio) {
		app_radio_request(radio);
		app_prefs_save();
	}
}

static void _pick_cb(lv_event_t *e)
{
	enum app_radio radio =
		(enum app_radio)(uintptr_t)lv_event_get_user_data(e);
	lv_timer_t *t;

	_last_activity = lv_tick_get();
	_pick_show(radio, true);
	t = lv_timer_create(_pick_close_cb, PICK_CLOSE_MS,
			    (void *)(uintptr_t)radio);
	lv_timer_set_repeat_count(t, 1);
}

static lv_obj_t *_pick_half(int32_t y, const char *text, enum app_radio radio)
{
	lv_obj_t *half = _box(_picker, 0, y, BOARD_LCD_H_RES, PICK_H);
	lv_obj_t *l;

	lv_obj_add_flag(half, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(half, _pick_cb, LV_EVENT_CLICKED,
			    (void *)(uintptr_t)radio);
	l = _label(half, _f.done, COLOR_WHITE, LV_OPA_COVER, 7);
	lv_label_set_text(l, text);
	lv_obj_center(l);
	return half;
}

static void _picker_build(void)
{
	_picker = _box(_scr, 0, 0, BOARD_LCD_H_RES, BOARD_LCD_V_RES);
	lv_obj_set_style_bg_color(_picker, lv_color_hex(COLOR_BLACK), 0);
	lv_obj_set_style_bg_opa(_picker, LV_OPA_COVER, 0);
	lv_obj_add_flag(_picker, LV_OBJ_FLAG_CLICKABLE);

	_pick_block = _box(_picker, 0, 0, BOARD_LCD_H_RES, PICK_H);
	lv_obj_set_style_bg_color(_pick_block, lv_color_hex(COLOR_BLURPLE), 0);
	lv_obj_set_style_bg_opa(_pick_block, LV_OPA_COVER, 0);
	lv_obj_remove_flag(_pick_block, LV_OBJ_FLAG_CLICKABLE);

	_pick_sat = _pick_half(0, "SATELLITE", APP_RADIO_SAT);
	_pick_ter = _pick_half(PICK_H, "TERRESTRIAL", APP_RADIO_TER);
	_hidden(_picker, true);
}

static void _picker_open(void)
{
	struct app_state s;

	app_state_get(&s);
	if (s.phase != APP_PHASE_COUNTDOWN && s.phase != APP_PHASE_TERRESTRIAL) {
		return;
	}
	_pick_show(s.radio, false);
	_hidden(_picker, false);
	lv_obj_move_foreground(_picker);
}

/* ---------- sheet ---------- */

static lv_obj_t *_sec(lv_obj_t *parent, const char *text)
{
	lv_obj_t *l = _label(parent, _f.title, COLOR_WHITE, 107, 4);

	lv_label_set_text(l, text);
	lv_obj_set_style_pad_top(l, 29, 0);
	lv_obj_set_style_pad_bottom(l, 3, 0);
	return l;
}

static lv_obj_t *_rows(lv_obj_t *parent)
{
	lv_obj_t *c = _box(parent, 0, 0, LV_PCT(100), LV_SIZE_CONTENT);

	lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
	return c;
}

static lv_obj_t *_row(lv_obj_t *rows, const char *key, lv_obj_t **key_out)
{
	lv_obj_t *r = _box(rows, 0, 0, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_t *k;

	lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_min_height(r, 58, 0);
	lv_obj_set_style_pad_ver(r, 14, 0);
	lv_obj_set_style_pad_column(r, 16, 0);
	lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
	lv_obj_set_style_border_width(r, 1, 0);
	lv_obj_set_style_border_color(r, lv_color_hex(COLOR_WHITE), 0);
	lv_obj_set_style_border_opa(r, 26, 0);

	k = _label(r, _f.row, COLOR_WHITE, LV_OPA_COVER, 0);
	lv_label_set_text(k, key);
	lv_obj_set_flex_grow(k, 1);
	if (key_out != NULL) {
		*key_out = k;
	}
	return r;
}

static lv_obj_t *_value(lv_obj_t *row)
{
	lv_obj_t *v = _label(row, _f.row, COLOR_WHITE, 140, 0);

	lv_label_set_text(v, "");
	return v;
}

static lv_obj_t *_step_btn(lv_obj_t *parent, const char *text,
			   lv_event_cb_t cb, intptr_t dir)
{
	lv_obj_t *b = _label(parent, _f.step, COLOR_WHITE, 179, 0);

	lv_label_set_text(b, text);
	lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_ext_click_area(b, 14);
	lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)dir);
	return b;
}

static lv_obj_t *_stepper(lv_obj_t *row, lv_event_cb_t cb)
{
	lv_obj_t *s = _box(row, 0, 0, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_t *out;

	lv_obj_remove_flag(s, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_flex_flow(s, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(s, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(s, 14, 0);
	_step_btn(s, "\xE2\x88\x92", cb, -1);
	out = _label(s, _f.row, COLOR_WHITE, LV_OPA_COVER, 0);
	lv_obj_set_style_min_width(out, 56, 0);
	lv_obj_set_style_text_align(out, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_text(out, "");
	_step_btn(s, "+", cb, +1);
	return out;
}

static void _count_cb(lv_event_t *e)
{
	intptr_t dir = (intptr_t)lv_event_get_user_data(e);
	struct app_state s;

	_last_activity = lv_tick_get();
	app_state_get(&s);
	if (s.phase == APP_PHASE_TX) {
		return;
	}
	if (s.sat_high != (dir > 0)) {
		app_sat_high_set(dir > 0);
		_prefs_dirty = true;
	}
}

static void _beacon_cb(lv_event_t *e)
{
	intptr_t dir = (intptr_t)lv_event_get_user_data(e);
	struct app_state s;
	int32_t ms;

	_last_activity = lv_tick_get();
	app_state_get(&s);
	ms = (int32_t)s.beacon_interval_ms + dir * BEACON_STEP_MS;
	ms = LV_CLAMP(BEACON_MIN_MS, ms, BEACON_MAX_MS);
	if ((uint32_t)ms != s.beacon_interval_ms) {
		app_beacon_interval_set(ms);
		_prefs_dirty = true;
	}
}

static void _forget_cb(lv_event_t *e)
{
	(void)e;
	_last_activity = lv_tick_get();
	if (_forget_armed_at != 0 &&
	    lv_tick_elaps(_forget_armed_at) < FORGET_ARM_MS) {
		_label_set(_forget_lbl, "Forgetting\xE2\x80\xA6");
		app_setup_forget();
		return;
	}
	_forget_armed_at = lv_tick_get();
	_label_set(_forget_lbl, "Tap again");
}

static void _sheet_build(void)
{
	lv_obj_t *head, *grab, *rows, *r, *k;

	_sheet = _box(_scr, 0, BOARD_LCD_V_RES, BOARD_LCD_H_RES, SHEET_H);
	lv_obj_set_style_bg_color(_sheet, lv_color_hex(COLOR_BLACK), 0);
	lv_obj_set_style_bg_opa(_sheet, LV_OPA_COVER, 0);
	lv_obj_set_style_border_side(_sheet, LV_BORDER_SIDE_TOP, 0);
	lv_obj_set_style_border_width(_sheet, 1, 0);
	lv_obj_set_style_border_color(_sheet, lv_color_hex(COLOR_WHITE), 0);
	lv_obj_set_style_border_opa(_sheet, 36, 0);
	lv_obj_add_flag(_sheet, LV_OBJ_FLAG_CLICKABLE);

	head = _box(_sheet, 0, 0, BOARD_LCD_H_RES, 64);
	lv_obj_add_flag(head, LV_OBJ_FLAG_CLICKABLE);
	grab = _box(head, CX - 22, 13, 45, 2);
	lv_obj_set_style_bg_color(grab, lv_color_hex(COLOR_WHITE), 0);
	lv_obj_set_style_bg_opa(grab, LV_OPA_40, 0);

	_sheet_name = _label(head, _f.title, COLOR_WHITE, 158, 4);
	lv_label_set_text(_sheet_name, "PASS SCHEDULE");
	lv_obj_set_pos(_sheet_name, 26, 34);
	_sheet_right = _label(head, _f.title, COLOR_WHITE, LV_OPA_40, 1);
	lv_label_set_text(_sheet_right, "");
	lv_obj_align(_sheet_right, LV_ALIGN_TOP_RIGHT, -26, 34);

	_sheet_body = _box(_sheet, 0, 64, BOARD_LCD_H_RES, SHEET_H - 64);
	lv_obj_add_flag(_sheet_body, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_scroll_dir(_sheet_body, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(_sheet_body, LV_SCROLLBAR_MODE_OFF);
	lv_obj_set_flex_flow(_sheet_body, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_hor(_sheet_body, 26, 0);
	lv_obj_set_style_pad_bottom(_sheet_body, 29, 0);

	/* Satellite */
	_sat_cfg = _rows(_sheet_body);
	_sec(_sat_cfg, "UPCOMING PASSES");
	rows = _rows(_sat_cfg);
	for (int i = 0; i < APP_PASSES_SHOWN; i++) {
		lv_obj_t *col;

		_pass_rows[i] = _row(rows, "", &k);
		lv_obj_delete(k);
		_pass_t[i] = _label(_pass_rows[i], _f.ptime, COLOR_WHITE,
				    LV_OPA_COVER, 0);
		lv_obj_set_width(_pass_t[i], 70);
		lv_obj_move_to_index(_pass_t[i], 0);
		col = _box(_pass_rows[i], 0, 0, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
		lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_flex_grow(col, 1);
		_pass_k[i] = _label(col, _f.row, COLOR_WHITE, LV_OPA_COVER, 0);
		_pass_sub[i] = _label(col, _f.sub, COLOR_WHITE, 107, 0);
		_pass_in[i] = _label(_pass_rows[i], _f.sub, COLOR_WHITE, 140, 0);
	}

	_sec(_sat_cfg, "TRANSMISSION");
	rows = _rows(_sat_cfg);
	_cnt_out = _stepper(_row(rows, "Packets per pass", NULL), _count_cb);
	_iv_out = _value(_row(rows, "Interval", NULL));
	r = _row(rows, "Tx power", NULL);
	lv_label_set_text(_value(r), "+20 dBm");
	_mode_val = _value(_row(rows, "Mode", NULL));
	_win_len = _value(_row(rows, "Window length", NULL));

	/* Terrestrial */
	_ter_cfg = _rows(_sheet_body);
	_sec(_ter_cfg, "BEACON");
	rows = _rows(_ter_cfg);
	_ter_today = _value(_row(rows, "Packets today", NULL));
	_biv_out = _stepper(_row(rows, "Interval", NULL), _beacon_cb);
	_home_val = _value(_row(rows, "Home Wi-Fi", NULL));
	lv_label_set_text(_value(_row(rows, "Radio", NULL)),
			  "BLE \xC2\xB7 2.4 GHz");
	lv_label_set_text(_value(_row(rows, "Mode", NULL)), "Terrestrial beacon");

	/* Both */
	_sec(_sheet_body, "PAYLOAD");
	rows = _rows(_sheet_body);
	_pay_temp = _value(_row(rows, "Temperature", NULL));
	_pay_bat = _value(_row(rows, "Battery", NULL));
	r = _box(_sheet_body, 0, 0, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_style_pad_top(r, 16, 0);
	_pay_size = _label(r, _f.sub, COLOR_WHITE, 107, 1);
	_pay_hex = _label(r, _f.sub, COLOR_WHITE, 107, 1);
	lv_obj_align(_pay_hex, LV_ALIGN_TOP_RIGHT, 0, 0);

	_sec(_sheet_body, "DEVICE");
	rows = _rows(_sheet_body);
	_setup_val = _value(_row(rows, "Saved setup", NULL));
	r = _row(rows, "Forget setup", NULL);
	_forget_lbl = _label(r, _f.row, COLOR_BLURPLE, LV_OPA_COVER, 0);
	lv_label_set_text(_forget_lbl, "Forget");
	lv_obj_add_flag(_forget_lbl, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_ext_click_area(_forget_lbl, 14);
	lv_obj_add_event_cb(_forget_lbl, _forget_cb, LV_EVENT_CLICKED, NULL);
}

static void _sheet_y_cb(void *var, int32_t v)
{
	lv_obj_set_y(var, v);
}

static void _sheet_move(bool open)
{
	lv_anim_t a;

	lv_anim_delete(_sheet, _sheet_y_cb);
	lv_anim_init(&a);
	lv_anim_set_var(&a, _sheet);
	lv_anim_set_exec_cb(&a, _sheet_y_cb);
	lv_anim_set_values(&a, lv_obj_get_y(_sheet),
			   open ? BOARD_LCD_V_RES - SHEET_H : BOARD_LCD_V_RES);
	lv_anim_set_duration(&a, SHEET_ANIM_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_start(&a);
}

static void _sheet_render(void);

static void _sheet_open(void)
{
	struct app_state s;

	app_state_get(&s);
	if (_sheet_is_open ||
	    (s.phase != APP_PHASE_COUNTDOWN && s.phase != APP_PHASE_TX &&
	     s.phase != APP_PHASE_TERRESTRIAL)) {
		return;
	}
	_sheet_is_open = true;
	_forget_armed_at = 0;
	_label_set(_forget_lbl, "Forget");
	lv_obj_scroll_to_y(_sheet_body, 0, LV_ANIM_OFF);
	_sheet_render();
	lv_obj_move_foreground(_sheet);
	_sheet_move(true);
}

static void _sheet_close(void)
{
	if (!_sheet_is_open) {
		return;
	}
	_sheet_is_open = false;
	_sheet_move(false);
	if (_prefs_dirty) {
		_prefs_dirty = false;
		app_prefs_save();
	}
}

static void _sheet_render(void)
{
	char buf[48];
	struct app_state s;
	uint64_t now = app_time_ms();
	bool ter;
	uint8_t payload[APP_PAYLOAD_LEN];
	int16_t temp;
	int bat;
	uint32_t interval_s;
	uint32_t total;

	app_state_get(&s);
	ter = s.phase == APP_PHASE_TERRESTRIAL;
	_hidden(_sat_cfg, ter);
	_hidden(_ter_cfg, !ter);
	_label_set(_sheet_name, ter ? "TERRESTRIAL" : "PASS SCHEDULE");

	bat = board_battery_percent();
	if (bat >= 0) {
		snprintf(buf, sizeof(buf), "BATTERY %d%%", bat);
	} else {
		buf[0] = '\0';
	}
	_label_set(_sheet_right, buf);

	for (int i = 0; i < APP_PASSES_SHOWN; i++) {
		bool live = i == 0 && s.phase == APP_PHASE_TX;

		_hidden(_pass_rows[i], i >= s.pass_count);
		if (i >= s.pass_count) {
			continue;
		}
		_fmt_clock(buf, sizeof(buf), s.pass_start_ms[i]);
		_label_set(_pass_t[i], buf);
		snprintf(buf, sizeof(buf), "Pass %d", i + 1);
		_label_set(_pass_k[i], buf);
		snprintf(buf, sizeof(buf), "%.0f\xC2\xB0 max elevation",
			 s.pass_elevation[i]);
		_label_set(_pass_sub[i], buf);
		if (live) {
			_label_set(_pass_in[i], "now");
		} else {
			char d[16];

			_fmt_dur(d, sizeof(d),
				 (int64_t)s.pass_start_ms[i] - (int64_t)now);
			snprintf(buf, sizeof(buf), "in %s", d);
			_label_set(_pass_in[i], buf);
		}
	}

	interval_s = s.sat_high ? 10 : 20;
	total = s.sat_high ? 16 : 8;
	snprintf(buf, sizeof(buf), "%" PRIu32, total);
	_label_set(_cnt_out, buf);
	snprintf(buf, sizeof(buf), "%" PRIu32 " s", interval_s);
	_label_set(_iv_out, buf);
	_label_set(_mode_val, s.radio == APP_RADIO_TER ? "Terrestrial beacon"
						       : "Satellite direct");
	_fmt_dur(buf, sizeof(buf), (int64_t)total * interval_s * MS_PER_SEC);
	_label_set(_win_len, buf);

	snprintf(buf, sizeof(buf), "%" PRIu32, app_beacons_today(&s, now));
	_label_set(_ter_today, buf);
	snprintf(buf, sizeof(buf), "%" PRIu32 ".%" PRIu32 " s",
		 s.beacon_interval_ms / 1000, (s.beacon_interval_ms % 1000) / 100);
	_label_set(_biv_out, buf);
	_label_set(_home_val, s.home ? "In range" : "Away");

	temp = board_battery_temp_centi_c();
	if (temp != INT16_MIN) {
		snprintf(buf, sizeof(buf), "%.1f \xC2\xB0" "C", temp / 100.0);
	} else {
		snprintf(buf, sizeof(buf), "--");
	}
	_label_set(_pay_temp, buf);
	if (bat >= 0) {
		snprintf(buf, sizeof(buf), "%d %%", bat);
	} else {
		snprintf(buf, sizeof(buf), "--");
	}
	_label_set(_pay_bat, buf);

	app_payload_build(payload);
	snprintf(buf, sizeof(buf), "%u B per packet", APP_PAYLOAD_LEN);
	_label_set(_pay_size, buf);
	snprintf(buf, sizeof(buf), "%02X%02X %02X %02X", payload[0], payload[1],
		 payload[2], payload[3]);
	_label_set(_pay_hex, buf);

	if (s.setup_saved_at_s != 0) {
		time_t t = (time_t)s.setup_saved_at_s;
		struct tm tm;
		char d[16];

		localtime_r(&t, &tm);
		strftime(d, sizeof(d), "%b %d", &tm);
		snprintf(buf, sizeof(buf), "%u satellites \xC2\xB7 %s",
			 s.setup_sats, d);
	} else {
		snprintf(buf, sizeof(buf), "None");
	}
	_label_set(_setup_val, buf);
}

/* ---------- gestures ---------- */

static void _gesture_cb(lv_event_t *e)
{
	lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());

	(void)e;
	if (_screen_off) {
		return;
	}
	_last_activity = lv_tick_get();
	if (dir == LV_DIR_TOP && lv_obj_has_flag(_picker, LV_OBJ_FLAG_HIDDEN)) {
		_sheet_open();
	} else if (dir == LV_DIR_BOTTOM) {
		_sheet_close();
	}
}

static void _run_click_cb(lv_event_t *e)
{
	lv_point_t p;
	int32_t dx, dy;

	(void)e;
	_last_activity = lv_tick_get();
	lv_indev_get_point(lv_indev_active(), &p);
	dx = p.x - CX;
	dy = p.y - ZEN_Y;
	if (dx * dx + dy * dy <= ZEN_HIT_R * ZEN_HIT_R) {
		_picker_open();
	}
}

static void _home_bar_cb(lv_event_t *e)
{
	(void)e;
	_last_activity = lv_tick_get();
	_sheet_open();
}

static void _sheet_head_cb(lv_event_t *e)
{
	(void)e;
	_last_activity = lv_tick_get();
	_sheet_close();
}

static void _activity_cb(lv_event_t *e)
{
	(void)e;
	_last_activity = lv_tick_get();
}

/* ---------- phases ---------- */

static void _set_big(const char *text, const lv_font_t *font,
		     const char *small)
{
	if (strcmp(_big_text, text) != 0) {
		snprintf(_big_text, sizeof(_big_text), "%s", text);
		lv_label_set_text(_big, text);
	}
	if (lv_obj_get_style_text_font(_big, 0) != font) {
		lv_obj_set_style_text_font(_big, font, 0);
	}
	_label_set(_big_small, small);
	_hidden(_big_small, small[0] == '\0');
}

static void _phase_enter(enum app_phase phase)
{
	bool run = phase == APP_PHASE_COUNTDOWN || phase == APP_PHASE_TX ||
		   phase == APP_PHASE_DONE || phase == APP_PHASE_TERRESTRIAL;
	bool ring = phase == APP_PHASE_TX || phase == APP_PHASE_DONE;

	_hidden(_boot, phase != APP_PHASE_BOOT && phase != APP_PHASE_SYNCING);
	_label_set(_boot_sub, phase == APP_PHASE_SYNCING ? "SYNCING TIME"
							 : "LOADING");
	_hidden(_prov, phase != APP_PHASE_PROVISIONING);
	_hidden(_run, !run);

	if (!run || phase == APP_PHASE_DONE) {
		_hidden(_picker, true);
		_sheet_close();
	}

	for (int i = 0; i < APP_PASSES_SHOWN; i++) {
		_hidden(_sats[i], true);
	}
	_hidden(_prog, !ring);
	lv_obj_set_style_arc_opa(_track,
				 phase == APP_PHASE_TX ? LV_OPA_50 : LV_OPA_COVER,
				 LV_PART_MAIN);
	_zen_dot_size(ring ? ZEN_DOT_TX_D : ZEN_DOT_D);

	_hidden(_top_lbl, phase == APP_PHASE_DONE);
	_hidden(_big_row, phase == APP_PHASE_DONE);
	_hidden(_bot_lbl, phase == APP_PHASE_DONE);
	_hidden(_done_msg, phase != APP_PHASE_DONE);
	_big_text[0] = '\0';
	_last_count = UINT32_MAX;
	_last_beacons = UINT32_MAX;

	switch (phase) {
	case APP_PHASE_TX:
		_bg_set(COLOR_BLURPLE);
		lv_arc_set_value(_prog, 0);
		board_vibrate(TX_BUZZ_MS);
		break;
	case APP_PHASE_DONE:
		_bg_set(COLOR_GREEN);
		lv_arc_set_value(_prog, 1000);
		break;
	default:
		_bg_set(COLOR_BLACK);
		break;
	}
}

static void _render_countdown(const struct app_state *s, uint64_t now)
{
	char buf[16];
	int64_t rem = (int64_t)s->next_tx_ms - (int64_t)now;
	bool long_ = rem >= 3600LL * MS_PER_SEC;

	_label_set(_top_lbl, "NEXT PASS");
	_label_set(_bot_lbl, long_ ? "HR \xC2\xB7 MIN \xC2\xB7 SEC"
				   : "MIN \xC2\xB7 SEC");
	_fmt_dur(buf, sizeof(buf), rem);
	_set_big(buf, long_ ? _f.long_ : _f.big, "");

	for (int i = 0; i < APP_PASSES_SHOWN; i++) {
		_hidden(_sats[i], i >= s->pass_count);
		if (i < s->pass_count) {
			_sat_place(i, ((double)s->pass_start_ms[i] - (double)now) /
					      3600000.0);
		}
	}
}

static void _render_tx(const struct app_state *s, uint64_t now)
{
	char buf[24];
	char small[12];
	uint32_t total = s->tx_total ? s->tx_total : 1;
	uint32_t interval_ms = (s->tx_interval_s ? s->tx_interval_s : 1) *
			       MS_PER_SEC;
	uint32_t sent = (now - s->tx_start_ms) / interval_ms + 1;

	if (sent > total) {
		sent = total;
	}
	if (sent != _last_count) {
		_last_count = sent;
		lv_arc_set_value(_prog, (int32_t)((sent * 1000) / total));
		_ripple_fire();
	}

	_label_set(_top_lbl, "TRANSMITTING");
	snprintf(buf, sizeof(buf), "%.0f\xC2\xB0 MAX ELEVATION",
		 s->pass_elevation[0]);
	_label_set(_bot_lbl, buf);
	snprintf(buf, sizeof(buf), "%" PRIu32, sent);
	snprintf(small, sizeof(small), "/%" PRIu32, total);
	_set_big(buf, _f.big, small);
}

static void _render_terrestrial(const struct app_state *s, uint64_t now)
{
	char buf[16];
	uint32_t n = app_beacons_today(s, now);

	_label_set(_top_lbl, "BEACONING");
	_label_set(_bot_lbl, "PACKETS TODAY");
	snprintf(buf, sizeof(buf), "%" PRIu32, n);
	_set_big(buf, _f.long_, "");

	if (n != _last_beacons && _last_beacons != UINT32_MAX &&
	    lv_tick_elaps(_last_ripple) > RIPPLE_GAP_MS) {
		_ripple_fire();
	}
	_last_beacons = n;
}

static void _keys_poll(void)
{
	bool down = board_key_pressed();

	if (down && !_key_was_down) {
		if (_screen_off) {
			_screen_set(true);
		} else if (_sheet_is_open) {
			_sheet_close();
		} else {
			_screen_set(false);
		}
	}
	_key_was_down = down;
}

static void _tick_cb(lv_timer_t *t)
{
	static uint32_t sheet_tick;
	struct app_state s;
	uint64_t now;

	(void)t;
	app_state_get(&s);
	now = app_time_ms();

	if (s.phase != _shown_phase) {
		_phase_enter(s.phase);
		if (_shown_phase != (enum app_phase)-1) {
			_screen_set(true);
		}
		_shown_phase = s.phase;
	}

	_keys_poll();

	/* Stay on through a satellite window and the last seconds before it. */
	if (!_screen_off && s.phase != APP_PHASE_TX &&
	    s.phase != APP_PHASE_DONE &&
	    !(s.phase == APP_PHASE_COUNTDOWN &&
	      s.next_tx_ms <= now + PASS_LEAD_MS) &&
	    lv_tick_elaps(_last_activity) > SCREEN_IDLE_MS) {
		_sheet_close();
		_screen_set(false);
	}

	switch (s.phase) {
	case APP_PHASE_COUNTDOWN:
		_render_countdown(&s, now);
		break;
	case APP_PHASE_TX:
		_render_tx(&s, now);
		break;
	case APP_PHASE_TERRESTRIAL:
		_render_terrestrial(&s, now);
		break;
	default:
		break;
	}

	if (_sheet_is_open && ++sheet_tick >= 1000 / TICK_MS) {
		sheet_tick = 0;
		_sheet_render();
	}
}

/* ---------- LVGL plumbing ---------- */

/* The CO5300 takes only windows that start and end on even pixels. */
static void _rounder_cb(lv_event_t *e)
{
	lv_area_t *area = lv_event_get_param(e);

	area->x1 &= ~1;
	area->y1 &= ~1;
	area->x2 |= 1;
	area->y2 |= 1;
}

void ui_screen_force_off(void)
{
	if (lvgl_port_lock(0)) {
		_screen_set(false);
		lvgl_port_unlock();
	}
}

bool ui_screen_on(void)
{
	return !_screen_off;
}

static void _fonts_load(void)
{
	_f.big = _font(true, 106);
	_f.long_ = _font(true, 74);
	_f.small = _font(true, 48);
	_f.lbl = _font(false, 17);
	_f.done = _font(false, 24);
	_f.title = _font(false, 15);
	_f.row = _font(false, 19);
	_f.sub = _font(false, 16);
	_f.ptime = _font(true, 21);
	_f.step = _font(false, 26);
	_f.prov = _font(false, 34);
}

esp_err_t ui_init(void)
{
	const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
	const lvgl_port_display_cfg_t disp_cfg = {
		.io_handle = board_lcd_io(),
		.panel_handle = board_lcd_panel(),
		.buffer_size = BOARD_LCD_H_RES * 48,
		.double_buffer = true,
		.hres = BOARD_LCD_H_RES,
		.vres = BOARD_LCD_V_RES,
		.color_format = LV_COLOR_FORMAT_RGB565,
		.flags =
			{
				.buff_dma = true,
				.swap_bytes = true,
			},
	};
	lvgl_port_touch_cfg_t touch_cfg = {0};

	ESP_RETURN_ON_FALSE(board_lcd_panel() != NULL, ESP_ERR_INVALID_STATE,
			    TAG, "no display");
	ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl port");

	_disp = lvgl_port_add_disp(&disp_cfg);
	ESP_RETURN_ON_FALSE(_disp != NULL, ESP_FAIL, TAG, "lvgl display");

	if (board_touch_handle() != NULL) {
		touch_cfg.disp = _disp;
		touch_cfg.handle = board_touch_handle();
		if (lvgl_port_add_touch(&touch_cfg) == NULL) {
			ESP_LOGE(TAG, "touch input not added");
		}
	} else {
		ESP_LOGW(TAG, "no touch: the front key still wakes the screen");
	}

	if (!lvgl_port_lock(0)) {
		return ESP_FAIL;
	}

	lv_display_add_event_cb(_disp, _rounder_cb, LV_EVENT_INVALIDATE_AREA,
				NULL);
	_fonts_load();

	_scr = lv_screen_active();
	lv_obj_remove_flag(_scr, LV_OBJ_FLAG_SCROLLABLE);
	_bg_now = lv_color_hex(COLOR_BLACK);
	lv_obj_set_style_bg_color(_scr, _bg_now, 0);
	lv_obj_set_style_bg_opa(_scr, LV_OPA_COVER, 0);
	lv_obj_add_event_cb(_scr, _gesture_cb, LV_EVENT_GESTURE, NULL);
	lv_obj_add_event_cb(_scr, _activity_cb, LV_EVENT_PRESSED, NULL);

	_run_build();
	_prov_build();
	_boot_build();
	_picker_build();
	_sheet_build();

	lv_obj_add_flag(_run, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(_run, _run_click_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(_home_bar, _home_bar_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(lv_obj_get_child(_sheet, 0), _sheet_head_cb,
			    LV_EVENT_CLICKED, NULL);

	/* While the screen is off, one tap anywhere only wakes it. */
	_waker = _box(_scr, 0, 0, BOARD_LCD_H_RES, BOARD_LCD_V_RES);
	lv_obj_add_flag(_waker, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(_waker, _waker_cb, LV_EVENT_PRESSED, NULL);
	_hidden(_waker, true);

	_phase_enter(APP_PHASE_BOOT);
	_shown_phase = APP_PHASE_BOOT;
	_last_activity = lv_tick_get();

	_ui_timer = lv_timer_create(_tick_cb, TICK_MS, NULL);
	lvgl_port_unlock();

	return ESP_OK;
}
