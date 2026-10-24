/*
 * Copyright (c) 2026 Hubble Network, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_pm.h"
#include "esp_timer.h"
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

/* Front key: a click is a short press; two within the gap is a double. */
#define KEY_POLL_MS    20
/* Slower key polling while dark; only the button wakes the screen then. */
#define KEY_POLL_OFF_MS 50
#define KEY_CLICK_MS   500
#define KEY_DOUBLE_MS  350
#define KEY_TASK_STACK 3072
#define KEY_TASK_PRIO  3

/* Battery gauge in the top-right corner of the main screens */
#define BATT_W         18
#define BATT_H         9
#define BATT_X         (BOARD_LCD_H_RES - BATT_W - 8)
#define BATT_Y         6
#define BATT_REFRESH   50

/* The backlight turns off after this long with no touch or key press. */
#define SCREEN_IDLE_MS 10000

/* Lead-in to a pass: wake this early and flash blurple on each second. */
#define PASS_LEAD_MS   10000
#define PASS_FLASH_MS  300

#define MENU_SPLIT_X   158
#define MENU_TEXT_X    20
#define MENU_TEXT_W    (MENU_SPLIT_X - MENU_TEXT_X - 4)
#define MENU_REFRESH   5

#define MS_PER_SEC     1000ULL
#define MS_PER_HOUR    (3600ULL * MS_PER_SEC)
#define MS_PER_DAY     (24ULL * MS_PER_HOUR)

static lv_display_t *_disp;
static lv_indev_t *_indev;
static lv_timer_t *_ui_timer;

static lv_obj_t *_boot;
static lv_obj_t *_boot_fill;
static lv_obj_t *_boot_lbl;
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

static lv_obj_t *_batt_fill;
static lv_obj_t *_batt_bolt;
static int _batt_tick = BATT_REFRESH;

static lv_obj_t *_picker;
static lv_obj_t *_pick_hl;

enum menu_item {
	MENU_PASS,
	MENU_MODE,
	MENU_PACKETS,
	MENU_SAT_POWER,
	MENU_BEACON,
	MENU_TODAY,
	MENU_BATTERY,
	MENU_PAYLOAD,
	MENU_SETUP,
	MENU_COUNT,
};

/* Beacon interval choices; 1500 ms is the default (app_ble.c). */
static const uint32_t _beacon_steps_ms[] = {500, 1000, 1500, 2000, 5000, 10000};
#define BEACON_STEPS (int)(sizeof(_beacon_steps_ms) / sizeof(_beacon_steps_ms[0]))

static lv_obj_t *_menu;
static lv_obj_t *_menu_title;
static lv_obj_t *_menu_value;
static lv_obj_t *_menu_sub;
static lv_obj_t *_menu_plus;
static lv_obj_t *_menu_minus;
static lv_obj_t *_menu_dots[MENU_COUNT];
static int _menu_idx;
static int _menu_pass;
static int _menu_tick;
static bool _setup_armed;

static bool _screen_off;
static uint32_t _last_activity;

/* Held while the screen is on so drawing and touch stay responsive. */
static esp_pm_lock_handle_t _ui_sleep_lock;
static esp_pm_lock_handle_t _ui_cpu_lock;
static bool _ui_locks_held;

static bool _menu_open(void);
static void _hidden(lv_obj_t *o, bool hidden);
static void _batt_build(lv_obj_t *parent);
static void _lvgl_idle(void);

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

/* Pass times show in CONFIG_HUBBLE_SAMPLE_TZ, set as TZ at boot (main.c). */
static void _fmt_local_clock(char *buf, size_t len, uint64_t unix_ms)
{
	time_t t = unix_ms / MS_PER_SEC;
	struct tm tm;

	localtime_r(&t, &tm);
	snprintf(buf, len, "%02d:%02d", tm.tm_hour, tm.tm_min);
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
	if ((s.phase != APP_PHASE_COUNTDOWN &&
	     s.phase != APP_PHASE_TERRESTRIAL) ||
	    _menu_open()) {
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
	_boot_lbl = lbl;

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

	_batt_build(_run);

	/* A finger is wider than the 14 px marker; give it a bigger target. */
	hit = _box(_run, RING_CX - 22, 0, 44, ZEN_Y + 20);
	lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(hit, _zen_clicked_cb, LV_EVENT_CLICKED, NULL);
}

/* ---------- battery gauge ---------- */

static bool _charging(void)
{
	enum board_charge chg = board_charge_state();

	return chg == BOARD_CHARGE_ON || chg == BOARD_CHARGE_PRE;
}

static void _batt_build(lv_obj_t *parent)
{
	lv_obj_t *body = _box(parent, BATT_X, BATT_Y, BATT_W, BATT_H);
	lv_obj_t *nub = _box(parent, BATT_X + BATT_W, BATT_Y + 3, 2,
			     BATT_H - 6);

	lv_obj_set_style_radius(body, 2, 0);
	lv_obj_set_style_border_color(body, lv_color_hex(COLOR_WHITE), 0);
	lv_obj_set_style_border_width(body, 1, 0);
	lv_obj_set_style_border_opa(body, LV_OPA_70, 0);
	lv_obj_set_style_pad_all(body, 2, 0);

	lv_obj_set_style_bg_color(nub, lv_color_hex(COLOR_WHITE), 0);
	lv_obj_set_style_bg_opa(nub, LV_OPA_70, 0);

	_batt_fill = _box(body, 0, 0, 0, BATT_H - 4);
	lv_obj_set_style_bg_color(_batt_fill, lv_color_hex(COLOR_WHITE), 0);
	lv_obj_set_style_bg_opa(_batt_fill, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(_batt_fill, 1, 0);

	_batt_bolt = _label(parent, &lv_font_montserrat_10, COLOR_WHITE, 0);
	lv_label_set_text(_batt_bolt, LV_SYMBOL_CHARGE);
	lv_obj_set_pos(_batt_bolt, BATT_X - 11, BATT_Y - 1);
}

static void _batt_render(void)
{
	int pct = board_battery_percent();

	if (pct < 0) {
		pct = 0;
	}
	lv_obj_set_width(_batt_fill, ((BATT_W - 4) * pct + 50) / 100);
	_hidden(_batt_bolt, !_charging());
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

	_hidden(_boot, phase != APP_PHASE_BOOT && phase != APP_PHASE_SYNCING);
	if (phase == APP_PHASE_SYNCING) {
		lv_label_set_text(_boot_lbl, "SYNCING TIME");
	}
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
	/* "TRANSMITTING" at 2 px spacing is wider than the ring. */
	lv_obj_set_style_text_letter_space(_top_lbl,
					   phase == APP_PHASE_TX ? 1 : 2, 0);
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
	uint64_t rem = s->next_tx_ms > now ? s->next_tx_ms - now : 0;
	char buf[24];
	char clock[8];
	bool long_fmt;

	/*
	 * The display rounds up, so a new second shows when rem % 1000 wraps
	 * to just under 1000; flash for the first PASS_FLASH_MS of it.
	 */
	if (rem > 0 && rem <= PASS_LEAD_MS) {
		_bg_set(rem % 1000 > 1000 - PASS_FLASH_MS ? COLOR_BLURPLE
							   : COLOR_BLACK);
	}

	_fmt_duration(buf, sizeof(buf), rem, &long_fmt);
	_label_set(_top_lbl, "NEXT PASS");
	lv_obj_set_style_text_font(_big, long_fmt ? &lv_font_montserrat_20
						  : &lv_font_montserrat_28,
				   0);
	_label_set(_big, buf);
	_label_set(_bot_lbl, long_fmt ? "HR MIN SEC" : "MIN  SEC");

	if (s->pass_count > 0) {
		_fmt_local_clock(clock, sizeof(clock), s->pass_start_ms[0]);
		snprintf(buf, sizeof(buf), "%s %s", clock,
			 CONFIG_HUBBLE_SAMPLE_TZ_LABEL);
		_label_set(_info1, buf);
		snprintf(buf, sizeof(buf), "EL %.0f°", s->pass_elevation[0]);
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
	/* The info column is 94 px; at 20 px "Overhead" does not fit. */
	snprintf(buf, sizeof(buf), "EL %.0f°", s->pass_elevation[0]);
	_label_set(_info1, buf);
	_label_set(_info2, "OVERHEAD");
}

static void _render_done(const struct app_state *s)
{
	char buf[16];

	snprintf(buf, sizeof(buf), "%u/%u", s->tx_total, s->tx_total);
	_label_set(_info1, buf);
	_label_set(_info2, "SENT");
}

static void _render_terrestrial(const struct app_state *s, uint64_t now)
{
	char buf[24];
	uint32_t count = app_beacons_today(s, now);

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
	snprintf(buf, sizeof(buf), "BLE  %+d DBM",
		 CONFIG_BT_LE_DFT_TX_POWER_LEVEL_DBM_EFF);
	_label_set(_info2, buf);
}

/* ---------- N1 menu: one value at a time, +/- on the right ---------- */

static bool _menu_editable(int idx, const struct app_state *s)
{
	switch (idx) {
	case MENU_MODE:
		return s->phase == APP_PHASE_COUNTDOWN ||
		       s->phase == APP_PHASE_TERRESTRIAL;
	case MENU_PASS:
		return s->pass_count > 1;
	case MENU_PACKETS:
		return true;
	case MENU_BEACON:
		return !s->home;
	case MENU_SETUP:
		return s->setup_saved_at_s != 0;
	default:
		return false;
	}
}

static int _beacon_step_idx(uint32_t interval_ms)
{
	int best = 0;

	for (int i = 1; i < BEACON_STEPS; i++) {
		if (_beacon_steps_ms[i] <= interval_ms) {
			best = i;
		}
	}
	return best;
}

static const char *_charge_text(enum board_charge chg)
{
	switch (chg) {
	case BOARD_CHARGE_OFF:
		return "Not charging";
	case BOARD_CHARGE_PRE:
		return "Pre-charging";
	case BOARD_CHARGE_ON:
		return "Charging";
	case BOARD_CHARGE_FULL:
		return "Full";
	default:
		return "Charger unknown";
	}
}

static void _menu_text(const struct app_state *s, uint64_t now, char *title,
		       char *value, char *sub, size_t len)
{
	uint8_t payload[APP_PAYLOAD_LEN];
	bool long_fmt;
	char dur[16];
	int pct;
	int16_t temp;
	uint16_t mv;
	int16_t ma;

	switch (_menu_idx) {
	case MENU_MODE:
		snprintf(title, len, "MODE");
		snprintf(value, len, s->radio == APP_RADIO_SAT ? "SAT" : "BLE");
		snprintf(sub, len, s->radio == APP_RADIO_SAT
					   ? "Satellite at each pass, BLE "
					     "beacon between"
					   : "BLE beacon only");
		break;
	case MENU_PASS:
		if (s->pass_count == 0 || now == 0) {
			snprintf(title, len, "NEXT PASS");
			snprintf(value, len, "--:--");
			snprintf(sub, len, "Not provisioned");
			break;
		}
		if (_menu_pass >= s->pass_count) {
			_menu_pass = s->pass_count - 1;
		}
		snprintf(title, len, "PASS %d OF %u", _menu_pass + 1,
			 s->pass_count);
		_fmt_local_clock(value, len, s->pass_start_ms[_menu_pass]);
		_fmt_duration(dur, sizeof(dur),
			      s->pass_start_ms[_menu_pass] > now
				      ? s->pass_start_ms[_menu_pass] - now
				      : 0,
			      &long_fmt);
		snprintf(sub, len, "%s, in %s, EL %.0f°",
			 CONFIG_HUBBLE_SAMPLE_TZ_LABEL, dur,
			 s->pass_elevation[_menu_pass]);
		break;
	case MENU_PACKETS:
		snprintf(title, len, "PACKETS PER PASS");
		snprintf(value, len, "%u", s->sat_high ? 16U : 8U);
		snprintf(sub, len, s->sat_high ? "10 s apart, high reliability"
					       : "20 s apart, normal "
						 "reliability");
		break;
	case MENU_SAT_POWER:
		/* The ESP32 port transmits at its 20 dBm maximum (radio.c). */
		snprintf(title, len, "SAT TX POWER");
		snprintf(value, len, "20");
		snprintf(sub, len, "dBm at the chip; antenna gain not "
				   "included");
		break;
	case MENU_BEACON:
		snprintf(title, len, "BEACON EVERY");
		snprintf(value, len, "%lu.%lu",
			 (unsigned long)s->beacon_interval_ms / 1000,
			 (unsigned long)(s->beacon_interval_ms % 1000) / 100);
		if (s->home) {
			snprintf(sub, len, "seconds; home Wi-Fi in range");
		} else {
			snprintf(sub, len, "seconds, BLE %+d dBm",
				 CONFIG_BT_LE_DFT_TX_POWER_LEVEL_DBM_EFF);
		}
		break;
	case MENU_TODAY:
		snprintf(title, len, "BEACONS TODAY");
		_fmt_thousands(value, len, app_beacons_today(s, now));
		snprintf(sub, len, "Estimate, UTC day");
		break;
	case MENU_BATTERY:
		pct = board_battery_percent();
		temp = board_battery_temp_centi_c();
		snprintf(title, len, "BATTERY");
		if (pct < 0) {
			snprintf(value, len, "--");
		} else {
			snprintf(value, len, "%s%d%%",
				 _charging() ? LV_SYMBOL_CHARGE " " : "", pct);
		}
		if (!board_battery_read(&mv, &ma)) {
			snprintf(sub, len, "Gauge not responding");
		} else {
			snprintf(sub, len, "%s, %+d mA, %u.%02u V, %d °C",
				 _charge_text(board_charge_state()), ma,
				 mv / 1000, (mv % 1000) / 10,
				 temp == INT16_MIN ? 0 : temp / 100);
		}
		break;
	case MENU_SETUP:
		snprintf(title, len, "SAVED SETUP");
		if (s->setup_saved_at_s == 0) {
			snprintf(value, len, "NONE");
			snprintf(sub, len, "Saved after the companion runs");
		} else if (_setup_armed) {
			snprintf(value, len, "CLEAR?");
			snprintf(sub, len, "Press again to clear and restart");
		} else {
			snprintf(value, len, "SAVED");
			snprintf(sub, len, "%u satellites, %llu d old. Press to "
					   "clear",
				 s->setup_sats,
				 now > s->setup_saved_at_s * MS_PER_SEC
					 ? (now / MS_PER_SEC -
					    s->setup_saved_at_s) /
						   (24 * 60 * 60)
					 : 0);
		}
		break;
	case MENU_PAYLOAD:
		app_payload_build(payload);
		snprintf(title, len, "PAYLOAD");
		snprintf(value, len, "%u B", APP_PAYLOAD_LEN);
		snprintf(sub, len, "%02X%02X %02X %02X  temp, battery",
			 payload[0], payload[1], payload[2], payload[3]);
		break;
	default:
		break;
	}
}

static void _menu_render(void)
{
	char title[32];
	char value[24];
	char sub[48];
	struct app_state s;
	bool editable;
	const lv_font_t *font = &lv_font_montserrat_48;

	app_state_get(&s);
	_menu_text(&s, app_time_ms(), title, value, sub, sizeof(title));

	/* Fall back to the smaller face when the value would not fit. */
	if (lv_text_get_width(value, strlen(value), font, 0) > MENU_TEXT_W) {
		font = &lv_font_montserrat_28;
	}
	lv_obj_set_style_text_font(_menu_value, font, 0);
	_label_set(_menu_title, title);
	_label_set(_menu_value, value);
	_label_set(_menu_sub, sub);

	editable = _menu_editable(_menu_idx, &s);
	lv_obj_set_style_opa(_menu_plus, editable ? LV_OPA_COVER : LV_OPA_20,
			     0);
	lv_obj_set_style_opa(_menu_minus, editable ? LV_OPA_COVER : LV_OPA_20,
			     0);

	for (int i = 0; i < MENU_COUNT; i++) {
		lv_obj_set_style_bg_color(_menu_dots[i],
					  lv_color_hex(i == _menu_idx
							       ? COLOR_WHITE
							       : 0x444444),
					  0);
	}
}

static void _menu_step(int dir)
{
	struct app_state s;
	int i;

	app_state_get(&s);
	if (!_menu_editable(_menu_idx, &s)) {
		return;
	}

	switch (_menu_idx) {
	case MENU_MODE:
		app_radio_request(s.radio == APP_RADIO_SAT ? APP_RADIO_TER
							   : APP_RADIO_SAT);
		break;
	case MENU_PASS:
		_menu_pass = (_menu_pass + s.pass_count + dir) % s.pass_count;
		break;
	case MENU_PACKETS:
		app_sat_high_set(!s.sat_high);
		break;
	case MENU_SETUP:
		/* Two presses, so a stray tap cannot wipe the setup. */
		if (!_setup_armed) {
			_setup_armed = true;
		} else {
			app_setup_forget();
		}
		break;
	case MENU_BEACON:
		i = _beacon_step_idx(s.beacon_interval_ms) + dir;
		if (i < 0 || i >= BEACON_STEPS) {
			return;
		}
		app_beacon_interval_set(_beacon_steps_ms[i]);
		break;
	default:
		break;
	}
	_menu_render();
}

static void _menu_move(int dir)
{
	_menu_idx = (_menu_idx + MENU_COUNT + dir) % MENU_COUNT;
	_menu_pass = 0;
	_setup_armed = false;
	_menu_render();
}

static void _menu_plus_cb(lv_event_t *e)
{
	_menu_step(+1);
}

static void _menu_minus_cb(lv_event_t *e)
{
	_menu_step(-1);
}

/* Tapping the top half of the left side goes back, the bottom half on. */
static void _menu_left_cb(lv_event_t *e)
{
	lv_point_t p;
	lv_dir_t dir;

	if (lv_event_get_code(e) == LV_EVENT_SHORT_CLICKED) {
		lv_indev_get_point(lv_indev_active(), &p);
		_menu_move(p.y < BOARD_LCD_V_RES / 2 ? -1 : +1);
		return;
	}

	dir = lv_indev_get_gesture_dir(lv_indev_active());
	if (dir == LV_DIR_TOP) {
		_menu_move(+1);
	} else if (dir == LV_DIR_BOTTOM) {
		_menu_move(-1);
	}
}

static lv_obj_t *_menu_key(int y, const char *symbol, lv_event_cb_t cb)
{
	lv_obj_t *key = _box(_menu, MENU_SPLIT_X + 1, y,
			     BOARD_LCD_H_RES - MENU_SPLIT_X - 1,
			     BOARD_LCD_V_RES / 2);
	lv_obj_t *l = _label(key, &lv_font_montserrat_28, COLOR_WHITE, 0);

	lv_label_set_text(l, symbol);
	lv_obj_center(l);
	lv_obj_set_style_bg_color(key, lv_color_hex(0x1C1C1C),
				  LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(key, LV_OPA_COVER, LV_STATE_PRESSED);
	lv_obj_add_flag(key, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(key, cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(key, cb, LV_EVENT_LONG_PRESSED_REPEAT, NULL);
	return key;
}

static void _menu_build(lv_obj_t *scr)
{
	lv_obj_t *left;
	lv_obj_t *line;
	int dots_top = (BOARD_LCD_V_RES - (MENU_COUNT * 8 - 4)) / 2;

	_menu = _box(scr, 0, 0, BOARD_LCD_H_RES, BOARD_LCD_V_RES);
	lv_obj_set_style_bg_color(_menu, lv_color_hex(COLOR_BLACK), 0);
	lv_obj_set_style_bg_opa(_menu, LV_OPA_COVER, 0);
	lv_obj_add_flag(_menu, LV_OBJ_FLAG_CLICKABLE);

	left = _box(_menu, 0, 0, MENU_SPLIT_X, BOARD_LCD_V_RES);
	lv_obj_add_flag(left, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(left, _menu_left_cb, LV_EVENT_GESTURE, NULL);
	lv_obj_add_event_cb(left, _menu_left_cb, LV_EVENT_SHORT_CLICKED, NULL);

	for (int i = 0; i < MENU_COUNT; i++) {
		_menu_dots[i] = _circle(left, 4, 0x444444, LV_OPA_COVER,
					COLOR_BLACK, 0);
		lv_obj_set_pos(_menu_dots[i], 8, dots_top + i * 8);
	}

	line = _label(left, &lv_font_montserrat_10, COLOR_DIM, 0);
	lv_label_set_text(line, LV_SYMBOL_UP);
	lv_obj_set_pos(line, 5, dots_top - 16);
	line = _label(left, &lv_font_montserrat_10, COLOR_DIM, 0);
	lv_label_set_text(line, LV_SYMBOL_DOWN);
	lv_obj_set_pos(line, 5, dots_top + MENU_COUNT * 8 + 4);

	_menu_title = _label(left, &lv_font_montserrat_10, COLOR_DIM, 2);
	lv_obj_set_pos(_menu_title, MENU_TEXT_X, 14);
	lv_obj_set_style_text_align(_menu_title, LV_TEXT_ALIGN_LEFT, 0);

	_menu_value = _label(left, &lv_font_montserrat_48, COLOR_WHITE, 0);
	lv_obj_set_pos(_menu_value, MENU_TEXT_X, 34);
	lv_obj_set_style_text_align(_menu_value, LV_TEXT_ALIGN_LEFT, 0);

	_menu_sub = _label(left, &lv_font_montserrat_12, COLOR_DIM, 0);
	lv_obj_set_pos(_menu_sub, MENU_TEXT_X, 92);
	lv_obj_set_width(_menu_sub, MENU_TEXT_W);
	lv_label_set_long_mode(_menu_sub, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_text_align(_menu_sub, LV_TEXT_ALIGN_LEFT, 0);

	line = _box(_menu, MENU_SPLIT_X, 0, 1, BOARD_LCD_V_RES);
	lv_obj_set_style_bg_color(line, lv_color_hex(0x2A2A2A), 0);
	lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
	line = _box(_menu, MENU_SPLIT_X, BOARD_LCD_V_RES / 2,
		    BOARD_LCD_H_RES - MENU_SPLIT_X, 1);
	lv_obj_set_style_bg_color(line, lv_color_hex(0x2A2A2A), 0);
	lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);

	_menu_plus = _menu_key(0, LV_SYMBOL_PLUS, _menu_plus_cb);
	_menu_minus = _menu_key(BOARD_LCD_V_RES / 2 + 1, LV_SYMBOL_MINUS,
				_menu_minus_cb);

	lv_obj_add_flag(_menu, LV_OBJ_FLAG_HIDDEN);
}

static bool _menu_open(void)
{
	return !lv_obj_has_flag(_menu, LV_OBJ_FLAG_HIDDEN);
}

static void _menu_show(bool show)
{
	if (show) {
		_hidden(_picker, true);
		_menu_idx = MENU_PASS;
		_menu_pass = 0;
		_setup_armed = false;
		_menu_render();
		lv_obj_move_foreground(_menu);
	}
	_hidden(_menu, !show);
}

/* ---------- front key: click = screen on/off, double = menu ---------- */

static void _ui_locks(bool hold)
{
	if (_ui_sleep_lock == NULL || hold == _ui_locks_held) {
		return;
	}
	if (hold) {
		esp_pm_lock_acquire(_ui_sleep_lock);
		esp_pm_lock_acquire(_ui_cpu_lock);
	} else {
		esp_pm_lock_release(_ui_cpu_lock);
		esp_pm_lock_release(_ui_sleep_lock);
	}
	_ui_locks_held = hold;
}

/*
 * Screen off is fully off: backlight, panel (ST7789 sleep-in) and LVGL all
 * stop, and the UI's power locks go, so the chip can light-sleep. The key
 * task keeps watching keys, touch and state changes to wake it.
 */
static void _screen_set(bool on)
{
	_last_activity = lv_tick_get();
	if (on == !_screen_off) {
		return;
	}
	_screen_off = !on;

	if (on) {
		_ui_locks(true);
		board_touch_set_active(true);
		(void)esp_lcd_panel_disp_sleep(board_lcd_panel(), false);
		(void)lvgl_port_resume();
		lv_timer_resume(lv_display_get_refr_timer(_disp));
		lv_timer_resume(lv_indev_get_read_timer(_indev));
		lv_timer_resume(_ui_timer);
		lv_obj_invalidate(lv_screen_active());
		board_backlight_set(true);
	} else {
		_hidden(_picker, true);
		board_backlight_set(false);
		(void)esp_lcd_panel_disp_sleep(board_lcd_panel(), true);
		_lvgl_idle();
		board_touch_set_active(false);
		_ui_locks(false);
	}
}

/*
 * lvgl_port_stop() alone keeps the chip from light-sleeping: with LVGL's
 * timers disabled, lv_timer_handler() asks to run again in 1 ms, so the port
 * task wakes on every tick. Pause each timer instead and stop the port's
 * 5 ms tick; LVGL then reports nothing scheduled and the task sleeps 500 ms.
 */
static void _lvgl_idle(void)
{
	lv_timer_pause(lv_display_get_refr_timer(_disp));
	lv_timer_pause(lv_indev_get_read_timer(_indev));
	lv_timer_pause(_ui_timer);
	(void)lvgl_port_stop();
	lv_timer_enable(true);
}

static void _wake(void)
{
	if (!lvgl_port_lock(0)) {
		return;
	}
	_screen_set(true);
	lvgl_port_unlock();
}

/* In the menu a click backs out; elsewhere it turns the screen off or on. */
static void _key_click(void)
{
	if (!lvgl_port_lock(0)) {
		return;
	}
	_last_activity = lv_tick_get();
	if (!_screen_off && _menu_open()) {
		_menu_show(false);
	} else {
		_screen_set(_screen_off);
	}
	lvgl_port_unlock();
}

static void _key_double(void)
{
	if (!lvgl_port_lock(0)) {
		return;
	}
	_last_activity = lv_tick_get();
	if (_screen_off) {
		_screen_set(true);
		_menu_show(true);
	} else {
		_menu_show(!_menu_open());
	}
	lvgl_port_unlock();
}

static void _key_task(void *arg)
{
	enum app_phase seen = APP_PHASE_BOOT;
	struct app_state s;
	uint64_t lead_woken = 0;
	bool was_down = false;
	uint32_t down_at = 0;
	uint32_t up_at = 0;
	int clicks = 0;

	for (;;) {
		uint32_t now = esp_timer_get_time() / 1000;
		bool down = board_key_pressed();

		/* LVGL is paused while dark, so wake-ups are watched from here. */
		app_state_get(&s);
		if (_screen_off && s.phase != seen) {
			_wake();
		} else if (_screen_off && s.phase == APP_PHASE_COUNTDOWN &&
			   s.next_tx_ms != lead_woken) {
			uint64_t t = app_time_ms();

			if (t + PASS_LEAD_MS >= s.next_tx_ms &&
			    t < s.next_tx_ms) {
				lead_woken = s.next_tx_ms;
				_wake();
			}
		}
		seen = s.phase;

		if (down && !was_down) {
			down_at = now;
		} else if (!down && was_down && now - down_at < KEY_CLICK_MS) {
			up_at = now;
			if (++clicks == 2) {
				clicks = 0;
				_key_double();
			}
		}

		if (clicks == 1 && !down && now - up_at > KEY_DOUBLE_MS) {
			clicks = 0;
			_key_click();
		}

		was_down = down;
		vTaskDelay(pdMS_TO_TICKS(_screen_off ? KEY_POLL_OFF_MS
						     : KEY_POLL_MS));
	}
}

static void _tick_cb(lv_timer_t *t)
{
	struct app_state s;
	uint64_t now;

	app_state_get(&s);
	now = app_time_ms();
	if (s.phase != _shown_phase) {
		_phase_enter(s.phase);
		/* Show the moment a pass starts, ends or setup finishes. */
		if (_shown_phase != (enum app_phase)-1) {
			_screen_set(true);
		}
		_shown_phase = s.phase;
	}

	/*
	 * Stay on through a satellite window; the phase change back to the
	 * countdown restarts the idle timer, so it goes dark 10 s after.
	 */
	if (!_screen_off && s.phase != APP_PHASE_TX &&
	    s.phase != APP_PHASE_DONE &&
	    !(s.phase == APP_PHASE_COUNTDOWN &&
	      s.next_tx_ms <= now + PASS_LEAD_MS) &&
	    lv_tick_elaps(_last_activity) > SCREEN_IDLE_MS) {
		_screen_set(false);
	}

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

	if (++_batt_tick >= BATT_REFRESH) {
		_batt_tick = 0;
		_batt_render();
	}

	if (_menu_open() && ++_menu_tick >= MENU_REFRESH) {
		_menu_tick = 0;
		_menu_render();
	}
}

/* ---------- LVGL plumbing ---------- */

static void _touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
	static uint16_t x;
	static uint16_t y;

	bool touched = board_touch_read(&x, &y);

	if (touched) {
		_last_activity = lv_tick_get();
		data->state = LV_INDEV_STATE_PRESSED;
	} else {
		data->state = LV_INDEV_STATE_RELEASED;
	}
	data->point.x = x;
	data->point.y = y;
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

	if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "ui",
			       &_ui_sleep_lock) != ESP_OK ||
	    esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "ui_cpu",
			       &_ui_cpu_lock) != ESP_OK) {
		_ui_sleep_lock = NULL;
	}
	_ui_locks(true);

	ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl port");

	_disp = lvgl_port_add_disp(&disp_cfg);
	ESP_RETURN_ON_FALSE(_disp != NULL, ESP_FAIL, TAG, "lvgl display");

	if (!lvgl_port_lock(0)) {
		return ESP_FAIL;
	}

	indev = lv_indev_create();
	_indev = indev;
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
	_menu_build(scr);
	_phase_enter(APP_PHASE_BOOT);
	_shown_phase = APP_PHASE_BOOT;

	_ui_timer = lv_timer_create(_tick_cb, TICK_MS, NULL);
	lvgl_port_unlock();

	ESP_RETURN_ON_FALSE(xTaskCreate(_key_task, "keys", KEY_TASK_STACK, NULL,
					KEY_TASK_PRIO, NULL) == pdPASS,
			    ESP_ERR_NO_MEM, TAG, "key task");

	return ESP_OK;
}
