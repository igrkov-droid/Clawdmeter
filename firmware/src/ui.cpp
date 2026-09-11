#include "ui.h"
#include "splash.h"
#include <lvgl.h>
#include <time.h>
#include "logo.h"
#include "clawd_still.h"
#include "icons.h"
#include "hal/board_caps.h"

// Custom fonts (scaled for 314 PPI, ~1.9x from original 165 PPI)
LV_FONT_DECLARE(font_tiempos_56);
LV_FONT_DECLARE(font_tiempos_34);
LV_FONT_DECLARE(font_styrene_48);
LV_FONT_DECLARE(font_styrene_28);
LV_FONT_DECLARE(font_styrene_24);
LV_FONT_DECLARE(font_styrene_20);
LV_FONT_DECLARE(font_styrene_16);
LV_FONT_DECLARE(font_styrene_14);
LV_FONT_DECLARE(font_styrene_12);
LV_FONT_DECLARE(font_mono_32);
LV_FONT_DECLARE(font_mono_18);

// Cyrillic faces. Tiempos and Styrene ship no Cyrillic glyphs whatsoever, so a
// Russian event title renders as blank space. PT Serif and PT Sans supply the
// missing characters; see link_cyrillic_fallbacks() below.
LV_FONT_DECLARE(font_cyr_serif_34);
LV_FONT_DECLARE(font_cyr_serif_56);
LV_FONT_DECLARE(font_cyr_sans_20);

// Layout values computed from the active board's geometry. Populated once
// in ui_init() and treated as const for the rest of the program. Adding a
// new display size means extending compute_layout() with another
// breakpoint — never editing the screen-builder functions below.
struct Layout {
    int16_t scr_w, scr_h;
    int16_t margin;
    int16_t title_y;
    int16_t content_y;
    int16_t content_w;

    // Usage screen
    int16_t usage_panel_h;
    int16_t usage_panel_gap;
    int16_t usage_bar_y;
    int16_t usage_reset_y;
    int16_t bar_h;
    int16_t panel_pad_x, panel_pad_y;
    int16_t pill_pad_x, pill_pad_y;
    const lv_font_t* title_font;     // screen title / clock
    const lv_font_t* pct_font;       // big percentage number
    const lv_font_t* ent_pct_font;   // enterprise spending number
    const lv_font_t* pill_font;      // "Current" / "Weekly" pill
    const lv_font_t* reset_font;     // "Resets in ..." line
    const lv_font_t* pace_font;      // enterprise "Under/On/Over pace" line
    const lv_font_t* anim_font;      // animated status line
    int16_t anim_y;                  // status line offset from bottom
    bool    small_icons;             // 40px logo + 24px battery (vs 80/48) on small screens
    int16_t title_nudge;             // title x-shift balancing the corner logo
    int16_t logo_y;                  // logo top edge
    int16_t batt_y;                  // battery icon top edge
    int16_t batt_w;                  // battery icon width, for position math

    // Pairing hint / idle screen
    int16_t pair_y1, pair_y2, pair_y3;
    int16_t idle_px;                 // sleeping-creature size on the idle screen

    // Agenda screen
    int16_t ag_date_y;               // baseline of the date, level with the battery
    int16_t ag_hero_y, ag_hero_h;
    int16_t ag_row_y, ag_row_h;
    int16_t ag_foot_y;
    uint8_t ag_rows;                 // list rows below the hero card that fit
    const lv_font_t* ag_kicker_font; // "IN 12 MIN" over the hero card
    const lv_font_t* ag_title_font;  // hero event title
    const lv_font_t* ag_meta_font;   // hero time range
    const lv_font_t* ag_row_font;    // list row title
    const lv_font_t* ag_time_font;   // list row time, tabular
    const lv_font_t* ag_foot_font;   // "+2 later"
    const lv_font_t* ag_alarm_font;  // ringing reminder title
    const lv_font_t* ag_btn_font;    // Snooze / Done

    // Bluetooth screen
    int16_t bt_info_panel_h;
    int16_t bt_reset_zone_h;
    const lv_font_t* bt_title_font;
    const lv_font_t* bt_status_font;
    const lv_font_t* bt_device_font;
    const lv_font_t* bt_credit_1_font;
    const lv_font_t* bt_credit_2_font;
};
static Layout L = {};

// ---- Cyrillic fallback ----
//
// Tiempos and Styrene contain no Cyrillic glyphs at all, so a Russian event
// title draws as blank space. LVGL can chain a second font per glyph through
// lv_font_t::fallback, but the generated fonts are `const` and live in
// read-only memory — writing the field into them faults (SIGBUS on the sim,
// a crash on the board). Instead each slot that shows user text gets a small
// mutable copy of its font: the copy shares every glyph bitmap by pointer, so
// it costs one struct, not a second typeface.
//
// Only slots carrying user-supplied text are wrapped. The rest of the screen
// is firmware's own wording and is Latin by construction.
//
// The fallback sizes (34/56 serif, 20 sans) are matched to the 480x480 board.
// A smaller port that picks 16px rows will render Cyrillic slightly larger
// than its Latin neighbours until a matching size is generated.
static lv_font_t f_title_ru, f_alarm_ru, f_row_ru, f_meta_ru;

static const lv_font_t* with_fallback(lv_font_t* copy, const lv_font_t* base,
                                      const lv_font_t* fb) {
    *copy = *base;
    copy->fallback = fb;
    return copy;
}

// Pick layout values from the active board's pixel dimensions. The two
// existing boards happen to land on the two breakpoints below; new ports
// inherit the closer one — visually OK, may need a polish pass for
// pixel-perfect alignment but never blocks the port from booting.
static void compute_layout(const BoardCaps& c) {
    L.scr_w = c.width;
    L.scr_h = c.height;
    L.margin = 20;
    L.title_y = 30;

    // Values shared by the two original breakpoints; the small branch below
    // overrides them wholesale.
    L.bar_h = 24;
    L.panel_pad_x = 16;
    L.panel_pad_y = 12;
    L.pill_pad_x = 18;
    L.pill_pad_y = 6;
    L.title_font   = &font_tiempos_56;
    L.pct_font     = &font_styrene_48;
    L.ent_pct_font = &font_tiempos_56;
    L.pill_font    = &font_styrene_28;
    L.reset_font   = &font_styrene_28;
    L.pace_font    = &font_styrene_16;
    L.anim_font    = &font_mono_32;
    L.anim_y = -15;
    L.small_icons = false;
    L.title_nudge = 16;
    L.logo_y = L.title_y - 10;
    L.batt_y = L.title_y;
    L.batt_w = ICON_BATTERY_W;
    L.pair_y1 = 40;
    L.pair_y2 = 120;
    L.pair_y3 = 160;
    L.idle_px = 160;

    if (c.height >= 460) {
        // Large layout — tuned for 480x480 (AMOLED-2.16).
        L.content_y = 100;
        L.usage_panel_h = 150;
        L.usage_panel_gap = 16;
        L.usage_bar_y = 56;
        L.usage_reset_y = 94;
        L.ag_date_y = 47;            // centres 14px type against the 48px battery at y=30
        L.ag_hero_y = 92;            // clears the 48px battery, which ends at 78
        L.ag_hero_h = 132;
        L.ag_row_y = 252;
        L.ag_row_h = 52;
        L.ag_foot_y = 424;
        L.ag_rows = 3;
        L.ag_kicker_font = &font_styrene_16;
        L.ag_title_font  = &font_tiempos_34;
        L.ag_meta_font   = &font_styrene_20;
        L.ag_row_font    = &font_styrene_20;
        L.ag_time_font   = &font_mono_18;
        L.ag_foot_font   = &font_styrene_14;
        L.ag_alarm_font  = &font_tiempos_56;
        L.ag_btn_font    = &font_styrene_24;
        L.bt_info_panel_h = 160;
        L.bt_reset_zone_h = 110;
        L.bt_title_font    = &font_tiempos_56;
        L.bt_status_font   = &font_styrene_48;
        L.bt_device_font   = &font_styrene_28;
        L.bt_credit_1_font = &font_styrene_24;
        L.bt_credit_2_font = &font_styrene_20;
    } else if (c.height >= 300) {
        // Compact layout — tuned for 368x448 (AMOLED-1.8).
        L.content_y = 85;
        L.usage_panel_h = 130;
        L.usage_panel_gap = 12;
        L.usage_bar_y = 48;
        L.usage_reset_y = 78;
        L.ag_date_y = 47;
        L.ag_hero_y = 86;
        L.ag_hero_h = 118;
        L.ag_row_y = 232;
        L.ag_row_h = 46;
        L.ag_foot_y = 400;
        L.ag_rows = 3;
        L.ag_kicker_font = &font_styrene_14;
        L.ag_title_font  = &font_tiempos_34;
        L.ag_meta_font   = &font_styrene_16;
        L.ag_row_font    = &font_styrene_16;
        L.ag_time_font   = &font_mono_18;
        L.ag_foot_font   = &font_styrene_12;
        L.ag_alarm_font  = &font_tiempos_34;
        L.ag_btn_font    = &font_styrene_20;
        L.bt_info_panel_h = 140;
        L.bt_reset_zone_h = 90;
        L.bt_title_font    = &font_tiempos_34;
        L.bt_status_font   = &font_styrene_28;
        L.bt_device_font   = &font_styrene_20;
        L.bt_credit_1_font = &font_styrene_16;
        L.bt_credit_2_font = &font_styrene_14;
    } else {
        // Small layout — tuned for 240x240 (LCD-1.54 and similar square TFTs).
        // Everything shrinks: fonts two steps down, panels ~half height, and
        // the corner logo/battery switch to the 40px/24px small assets.
        L.margin = 8;
        L.title_y = 4;
        L.content_y = 44;
        L.usage_panel_h = 74;
        L.usage_panel_gap = 6;
        L.usage_bar_y = 30;
        L.usage_reset_y = 46;
        L.bar_h = 12;
        L.panel_pad_x = 10;
        L.panel_pad_y = 6;
        L.pill_pad_x = 8;
        L.pill_pad_y = 2;
        L.title_font   = &font_tiempos_34;
        L.pct_font     = &font_styrene_24;
        L.ent_pct_font = &font_tiempos_34;
        L.pill_font    = &font_styrene_14;
        L.reset_font   = &font_styrene_14;
        L.pace_font    = &font_styrene_12;
        L.anim_font    = &font_mono_18;
        // Center the status line in the strip below the weekly panel; flush
        // against the bottom edge it reads as unevenly spaced.
        L.anim_y = -10;
        L.small_icons = true;
        L.title_nudge = 8;
        L.logo_y = 2;
        L.batt_y = 10;
        L.batt_w = ICON_BATTERY_SMALL_W;
        L.pair_y1 = 12;
        L.pair_y2 = 56;
        L.pair_y3 = 80;
        L.idle_px = 96;
        L.ag_date_y = 16;            // 24px battery at y=10
        L.ag_hero_y = 42;            // clears the 24px battery, which ends at 34
        L.ag_hero_h = 76;
        L.ag_row_y = 140;
        L.ag_row_h = 34;
        L.ag_foot_y = 208;
        L.ag_rows = 2;
        L.ag_kicker_font = &font_styrene_12;
        L.ag_title_font  = &font_styrene_20;
        L.ag_meta_font   = &font_styrene_14;
        L.ag_row_font    = &font_styrene_14;
        L.ag_time_font   = &font_mono_18;
        L.ag_foot_font   = &font_styrene_12;
        L.ag_alarm_font  = &font_tiempos_34;
        L.ag_btn_font    = &font_styrene_16;
        L.bt_info_panel_h = 90;
        L.bt_reset_zone_h = 60;
        L.bt_title_font    = &font_tiempos_34;
        L.bt_status_font   = &font_styrene_20;
        L.bt_device_font   = &font_styrene_14;
        L.bt_credit_1_font = &font_styrene_12;
        L.bt_credit_2_font = &font_styrene_12;
    }

    L.content_w = L.scr_w - 2 * L.margin;

    // Wrap the user-text slots so Cyrillic resolves through PT Serif / PT Sans.
    L.ag_title_font = with_fallback(&f_title_ru, L.ag_title_font, &font_cyr_serif_34);
    L.ag_alarm_font = with_fallback(&f_alarm_ru, L.ag_alarm_font, &font_cyr_serif_56);
    L.ag_row_font   = with_fallback(&f_row_ru,   L.ag_row_font,   &font_cyr_sans_20);
    L.ag_meta_font  = with_fallback(&f_meta_ru,  L.ag_meta_font,  &font_cyr_sans_20);
}

// Anthropic brand palette — design tokens live in theme.h
#include "theme.h"
#define COL_BG        THEME_BG
#define COL_PANEL     THEME_PANEL
#define COL_TEXT      THEME_TEXT
#define COL_DIM       THEME_DIM
#define COL_ACCENT    THEME_ACCENT
#define COL_GREEN     THEME_GREEN
#define COL_AMBER     THEME_AMBER
#define COL_RED       THEME_RED
#define COL_BAR_BG    THEME_BAR_BG

// ---- Usage screen widgets (single non-splash view) ----
static lv_obj_t* usage_container;
static lv_obj_t* lbl_title;
// Clock fed by the daemon: base epoch (local wall-clock seconds) + the lv_tick at
// which it landed, so the title ticks forward locally between 60s payloads.
static long     clock_base_epoch = 0;
static uint32_t clock_base_ms = 0;
static int      clock_fmt = 24;   // 12 or 24, set from the daemon payload
static int      clock_last_min = -1;   // last rendered minute; avoids redrawing the title every tick
static lv_obj_t* usage_group;   // the two usage panels — shown when connected
static lv_obj_t* pair_group;    // pairing hint — shown when disconnected
static lv_obj_t* bar_session;
static lv_obj_t* lbl_session_pct;
static lv_obj_t* lbl_session_label;
static lv_obj_t* lbl_session_reset;
static lv_obj_t* bar_weekly;
static lv_obj_t* lbl_weekly_pct;
static lv_obj_t* lbl_weekly_label;
static lv_obj_t* lbl_weekly_reset;
static lv_obj_t* panel_session = nullptr;
static lv_obj_t* panel_weekly = nullptr;
// Enterprise-only widgets inside panel_session
static lv_obj_t* lbl_session_pct_sym = nullptr;  // "%" in smaller font
static lv_obj_t* lbl_spending_desc = nullptr;     // "of your monthly budget"
static lv_obj_t* lbl_spending_status = nullptr;   // "Under pace" / "On pace" / "Over pace"
static lv_obj_t* lbl_anim;      // status line: connection state + whimsical idle

// ---- Battery indicator (shared, on top) ----
static lv_obj_t* battery_img;
static lv_obj_t* logo_img;
static lv_image_dsc_t battery_dscs[5];  // empty, low, medium, full, charging

// ---- Live-data freshness → which usage sub-view to show ----
// usage panels when data is flowing, an idle "Zzz" screen when the host is
// connected but no usage update landed within DATA_FRESH_MS, the pairing hint
// when BLE is down. Re-evaluated every loop in ui_tick_anim().
static lv_obj_t* idle_group;            // the "Zzz" idle screen
static uint32_t  last_data_ms = 0;      // lv_tick when the last valid usage update landed
static bool      data_received = false; // any valid update since boot
static bool      data_ok = true;        // last payload's ok flag; a {"ok":false} beat = "no fresh data"
static int       view_state = -1;       // -1 unknown / 0 pair / 1 idle / 2 usage
static const uint32_t DATA_FRESH_MS = 90000;  // usage counts as "live" within this window (daemon sends ~60s)

// ---- Shared ----
static lv_image_dsc_t logo_dsc;
static screen_t current_screen = SCREEN_USAGE;
static bool     s_ble_connected = false;   // cached BLE connection state
static uint32_t connected_at_ms = 0;       // when we last entered CONNECTED ("Connected" dwell)

// Animation state
static uint32_t anim_last_ms = 0;
static uint8_t anim_spinner_idx = 0;
static uint8_t anim_phase = 0;
static uint8_t anim_msg_idx = 0;
static uint32_t anim_msg_start = 0;
#define ANIM_MSG_MS     4000

static const char* const spinner_frames[] = {
    "\xC2\xB7", "\xE2\x9C\xBB", "\xE2\x9C\xBD",
    "\xE2\x9C\xB6", "\xE2\x9C\xB3", "\xE2\x9C\xA2",
};
#define SPINNER_COUNT 6
#define SPINNER_PHASES (2 * (SPINNER_COUNT - 1))  // 10: ping-pong 0..5..0

static const uint16_t spinner_ms[SPINNER_COUNT] = {
    260, 130, 130, 130, 130, 260,
};

static const char* const anim_messages[] = {
    "Accomplishing", "Elucidating", "Perusing",
    "Actioning", "Enchanting", "Philosophising",
    "Actualizing", "Envisioning", "Pondering",
    "Baking", "Finagling", "Pontificating",
    "Booping", "Flibbertigibbeting", "Processing",
    "Brewing", "Forging", "Puttering",
    "Calculating", "Forming", "Puzzling",
    "Cerebrating", "Frolicking", "Reticulating",
    "Channelling", "Generating", "Ruminating",
    "Churning", "Germinating", "Scheming",
    "Clauding", "Hatching", "Schlepping",
    "Coalescing", "Herding", "Shimmying",
    "Cogitating", "Honking", "Shucking",
    "Combobulating", "Hustling", "Simmering",
    "Computing", "Ideating", "Smooshing",
    "Concocting", "Imagining", "Spelunking",
    "Conjuring", "Incubating", "Spinning",
    "Considering", "Inferring", "Stewing",
    "Contemplating", "Jiving", "Sussing",
    "Cooking", "Manifesting", "Synthesizing",
    "Crafting", "Marinating", "Thinking",
    "Creating", "Meandering", "Tinkering",
    "Crunching", "Moseying", "Transmuting",
    "Deciphering", "Mulling", "Unfurling",
    "Deliberating", "Mustering", "Unravelling",
    "Determining", "Musing", "Vibing",
    "Discombobulating", "Noodling", "Wandering",
    "Divining", "Percolating", "Whirring",
    "Doing", "Wibbling",
    "Effecting", "Wizarding",
    "Working", "Wrangling",
};
#define ANIM_MSG_COUNT (sizeof(anim_messages) / sizeof(anim_messages[0]))

static lv_color_t pct_color(float pct) {
    if (pct >= 80.0f) return COL_RED;
    if (pct >= 50.0f) return COL_AMBER;
    return COL_GREEN;
}

static void format_reset_time(int mins, char* buf, size_t len) {
    if (mins < 0) {
        snprintf(buf, len, "---");
    } else if (mins < 60) {
        snprintf(buf, len, "Resets in %dm", mins);
    } else if (mins < 1440) {
        snprintf(buf, len, "Resets in %dh %dm", mins / 60, mins % 60);
    } else {
        snprintf(buf, len, "Resets in %dd %dh", mins / 1440, (mins % 1440) / 60);
    }
}

// Forward decls — callbacks defined near ui_show_screen below
static void global_click_cb(lv_event_t* e);

static lv_obj_t* make_panel(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_bg_color(panel, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 8, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_left(panel, L.panel_pad_x, 0);
    lv_obj_set_style_pad_right(panel, L.panel_pad_x, 0);
    lv_obj_set_style_pad_top(panel, L.panel_pad_y, 0);
    lv_obj_set_style_pad_bottom(panel, L.panel_pad_y, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_EVENT_BUBBLE);
    return panel;
}

static lv_obj_t* make_bar(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* bar = lv_bar_create(parent);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, w, h);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, COL_BAR_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, COL_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 6, LV_PART_INDICATOR);
    return bar;
}

static void init_icon_dsc_rgb565a8(lv_image_dsc_t* dsc, int w, int h, const uint8_t* data) {
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565A8;
    dsc->header.stride = w * 2;
    dsc->data = data;
    dsc->data_size = w * h * 3;
}

static lv_obj_t* make_pill(lv_obj_t* parent, const char* text) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, L.pill_font, 0);
    lv_obj_set_style_text_color(lbl, COL_TEXT, 0);
    lv_obj_set_style_bg_color(lbl, COL_BAR_BG, 0);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(lbl, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_left(lbl, L.pill_pad_x, 0);
    lv_obj_set_style_pad_right(lbl, L.pill_pad_x, 0);
    lv_obj_set_style_pad_top(lbl, L.pill_pad_y, 0);
    lv_obj_set_style_pad_bottom(lbl, L.pill_pad_y, 0);
    return lbl;
}

static void init_battery_icons(void) {
    if (L.small_icons) {
        init_icon_dsc_rgb565a8(&battery_dscs[0], ICON_BATTERY_SMALL_W, ICON_BATTERY_SMALL_H, icon_battery_small_data);
        init_icon_dsc_rgb565a8(&battery_dscs[1], ICON_BATTERY_LOW_SMALL_W, ICON_BATTERY_LOW_SMALL_H, icon_battery_low_small_data);
        init_icon_dsc_rgb565a8(&battery_dscs[2], ICON_BATTERY_MEDIUM_SMALL_W, ICON_BATTERY_MEDIUM_SMALL_H, icon_battery_medium_small_data);
        init_icon_dsc_rgb565a8(&battery_dscs[3], ICON_BATTERY_FULL_SMALL_W, ICON_BATTERY_FULL_SMALL_H, icon_battery_full_small_data);
        init_icon_dsc_rgb565a8(&battery_dscs[4], ICON_BATTERY_CHARGING_SMALL_W, ICON_BATTERY_CHARGING_SMALL_H, icon_battery_charging_small_data);
        return;
    }
    init_icon_dsc_rgb565a8(&battery_dscs[0], ICON_BATTERY_W, ICON_BATTERY_H, icon_battery_data);
    init_icon_dsc_rgb565a8(&battery_dscs[1], ICON_BATTERY_LOW_W, ICON_BATTERY_LOW_H, icon_battery_low_data);
    init_icon_dsc_rgb565a8(&battery_dscs[2], ICON_BATTERY_MEDIUM_W, ICON_BATTERY_MEDIUM_H, icon_battery_medium_data);
    init_icon_dsc_rgb565a8(&battery_dscs[3], ICON_BATTERY_FULL_W, ICON_BATTERY_FULL_H, icon_battery_full_data);
    init_icon_dsc_rgb565a8(&battery_dscs[4], ICON_BATTERY_CHARGING_W, ICON_BATTERY_CHARGING_H, icon_battery_charging_data);
}

// ======== Usage Screen ========

static lv_obj_t* make_usage_panel(lv_obj_t* parent, int y, const char* pill_text,
                                  lv_obj_t** out_pct, lv_obj_t** out_pill,
                                  lv_obj_t** out_bar, lv_obj_t** out_reset) {
    lv_obj_t* panel = make_panel(parent, L.margin, y, L.content_w, L.usage_panel_h);

    *out_pct = lv_label_create(panel);
    lv_label_set_text(*out_pct, "---%");
    lv_obj_set_style_text_font(*out_pct, L.pct_font, 0);
    lv_obj_set_style_text_color(*out_pct, COL_TEXT, 0);
    lv_obj_set_pos(*out_pct, 0, 0);

    *out_pill = make_pill(panel, pill_text);
    lv_obj_align(*out_pill, LV_ALIGN_TOP_RIGHT, 0, 1);

    *out_bar = make_bar(panel, 0, L.usage_bar_y,
                        L.content_w - 2 * L.panel_pad_x, L.bar_h);

    *out_reset = lv_label_create(panel);
    lv_label_set_text(*out_reset, "---");
    lv_obj_set_style_text_font(*out_reset, L.reset_font, 0);
    lv_obj_set_style_text_color(*out_reset, COL_DIM, 0);
    lv_obj_set_pos(*out_reset, 0, L.usage_reset_y);

    return panel;
}

// Pairing hint — shown when disconnected so the screen isn't empty and the
// user knows how to (re)pair. Wording matches the 3-second release gesture.
static void build_pair_group(lv_obj_t* parent) {
    pair_group = lv_obj_create(parent);
    lv_obj_set_size(pair_group, L.scr_w, L.scr_h - L.content_y);
    lv_obj_set_pos(pair_group, 0, L.content_y);
    lv_obj_set_style_bg_opa(pair_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pair_group, 0, 0);
    lv_obj_set_style_pad_all(pair_group, 0, 0);
    lv_obj_clear_flag(pair_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t* l1 = lv_label_create(pair_group);
    lv_label_set_text(l1, "To pair");
    lv_obj_set_style_text_font(l1, L.bt_status_font, 0);
    lv_obj_set_style_text_color(l1, COL_TEXT, 0);
    lv_obj_align(l1, LV_ALIGN_TOP_MID, 0, L.pair_y1);

    lv_obj_t* l2 = lv_label_create(pair_group);
    lv_label_set_text(l2, "hold the power button");
    lv_obj_set_style_text_font(l2, L.bt_device_font, 0);
    lv_obj_set_style_text_color(l2, COL_DIM, 0);
    lv_obj_align(l2, LV_ALIGN_TOP_MID, 0, L.pair_y2);

    lv_obj_t* l3 = lv_label_create(pair_group);
    lv_label_set_text(l3, "for 3 seconds, then release");
    lv_obj_set_style_text_font(l3, L.bt_device_font, 0);
    lv_obj_set_style_text_color(l3, COL_DIM, 0);
    lv_obj_align(l3, LV_ALIGN_TOP_MID, 0, L.pair_y3);

    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_HIDDEN);  // ui_update_ble_status decides
}

// Idle "Zzz" screen — shown when the host is connected but no usage update has
// landed recently (token expired, daemon down, host asleep…). Full-screen, like
// the pairing hint, so we never render hours-old numbers as if they were live.
static void build_idle_group(lv_obj_t* parent) {
    idle_group = lv_obj_create(parent);
    lv_obj_set_size(idle_group, L.scr_w, L.scr_h - L.content_y);
    lv_obj_set_pos(idle_group, 0, L.content_y);
    lv_obj_set_style_bg_opa(idle_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(idle_group, 0, 0);
    lv_obj_set_style_pad_all(idle_group, 0, 0);
    lv_obj_clear_flag(idle_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    // A shrunk-down resting creature (the official cloud-ride animation)
    // sits between the header and the status line; the animated "Listening…"
    // status line carries the words, so no extra text is needed here.
    lv_obj_t* creature = splash_mini_create(idle_group, "cloud", L.idle_px);
    if (creature) lv_obj_align(creature, LV_ALIGN_CENTER, 0, -20);

    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_HIDDEN);  // update_view_state decides
}

static void init_usage_screen(lv_obj_t* scr) {
    usage_container = lv_obj_create(scr);
    lv_obj_set_size(usage_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(usage_container, 0, 0);
    lv_obj_set_style_bg_opa(usage_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(usage_container, 0, 0);
    lv_obj_set_style_pad_all(usage_container, 0, 0);
    lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(usage_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    lbl_title = lv_label_create(usage_container);
    lv_label_set_text(lbl_title, "Usage");
    lv_obj_set_style_text_font(lbl_title, L.title_font, 0);
    lv_obj_set_style_text_color(lbl_title, COL_TEXT, 0);
    // The nudge balances the corner logo on the left; smaller on small
    // screens where the logo is 40px and the battery icon sits closer.
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, L.title_nudge, L.title_y);

    // Usage panels (shown when connected) live in a transparent full-size group
    // so they can be toggled against the pairing hint as one unit.
    usage_group = lv_obj_create(usage_container);
    lv_obj_set_size(usage_group, L.scr_w, L.scr_h);
    lv_obj_set_pos(usage_group, 0, 0);
    lv_obj_set_style_bg_opa(usage_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(usage_group, 0, 0);
    lv_obj_set_style_pad_all(usage_group, 0, 0);
    lv_obj_clear_flag(usage_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(usage_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    panel_session = make_usage_panel(usage_group, L.content_y, "Current",
                     &lbl_session_pct, &lbl_session_label,
                     &bar_session, &lbl_session_reset);

    // Enterprise-only overlays inside panel_session — hidden until enterprise data arrives
    lbl_session_pct_sym = lv_label_create(panel_session);
    lv_label_set_text(lbl_session_pct_sym, "%");
    lv_obj_set_style_text_font(lbl_session_pct_sym, L.reset_font, 0);
    lv_obj_set_style_text_color(lbl_session_pct_sym, COL_TEXT, 0);
    lv_obj_add_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);

    lbl_spending_desc = lv_label_create(panel_session);
    lv_label_set_text(lbl_spending_desc, "of your monthly budget");
    lv_obj_set_style_text_font(lbl_spending_desc, L.reset_font, 0);
    lv_obj_set_style_text_color(lbl_spending_desc, COL_DIM, 0);
    lv_obj_set_pos(lbl_spending_desc, 0, L.usage_reset_y);
    lv_obj_add_flag(lbl_spending_desc, LV_OBJ_FLAG_HIDDEN);

    lbl_spending_status = lv_label_create(panel_session);
    lv_label_set_text(lbl_spending_status, "");
    lv_obj_set_style_text_font(lbl_spending_status, L.pace_font, 0);
    lv_obj_set_pos(lbl_spending_status, 0, L.usage_reset_y + 20);
    lv_obj_add_flag(lbl_spending_status, LV_OBJ_FLAG_HIDDEN);

    panel_weekly = make_usage_panel(usage_group,
                     L.content_y + L.usage_panel_h + L.usage_panel_gap, "Weekly",
                     &lbl_weekly_pct, &lbl_weekly_label,
                     &bar_weekly, &lbl_weekly_reset);
    // Recolor enabled so enterprise period box can color pace and reset separately
    lv_label_set_recolor(lbl_weekly_reset, true);

    build_pair_group(usage_container);
    build_idle_group(usage_container);

    // Status line — always visible on the usage view. Driven by ui_tick_anim().
    lbl_anim = lv_label_create(usage_container);
    lv_label_set_text(lbl_anim, "");
    lv_obj_set_style_text_font(lbl_anim, L.anim_font, 0);
    lv_obj_set_style_text_color(lbl_anim, COL_ACCENT, 0);
    lv_obj_align(lbl_anim, LV_ALIGN_BOTTOM_MID, 0, L.anim_y);
}

// ======== Agenda Screen ========
//
// The daemon sends absolute timestamps and pre-truncated titles; everything
// here is presentation plus the one thing the board must own — counting down.
// "in 12 min" is recomputed from the shared wall clock every second, so it
// stays right between polls and while the link is down.

static lv_obj_t* agenda_container = nullptr;
static lv_obj_t* ag_date = nullptr;
static lv_obj_t* ag_hero = nullptr;
static lv_obj_t* ag_stripe = nullptr;
static lv_obj_t* ag_kicker = nullptr;
static lv_obj_t* ag_title = nullptr;
static lv_obj_t* ag_meta = nullptr;
static lv_obj_t* ag_progress = nullptr;
static lv_obj_t* ag_foot = nullptr;
static lv_obj_t* ag_empty_group = nullptr;
static lv_obj_t* ag_empty_title = nullptr;
static lv_obj_t* ag_empty_sub = nullptr;

struct AgendaRow {
    lv_obj_t* group;
    lv_obj_t* rule;
    lv_obj_t* time;
    lv_obj_t* dot;
    lv_obj_t* name;
};
static AgendaRow agenda_rows[AGENDA_MAX_ITEMS - 1];

static AgendaData agenda;            // last payload, kept for the per-second tick
static int  ag_last_kicker_sec = -1; // so the tick only relays out on a change

// Alarm screen
static lv_obj_t* alarm_container = nullptr;
static lv_obj_t* al_kicker = nullptr;
static lv_obj_t* al_title = nullptr;
static lv_obj_t* al_time = nullptr;
static lv_obj_t* al_btn_snooze = nullptr;
static lv_obj_t* al_btn_done = nullptr;
static AgendaItem alarm_item;
static bool alarm_item_valid = false;
static ui_alarm_action_cb alarm_cb = nullptr;

static lv_color_t agenda_color(unsigned char c) {
    switch (c) {
    case 1:  return COL_GREEN;
    case 2:  return COL_DIM;
    default: return COL_ACCENT;
    }
}

// The daemon ships an already-local-shifted epoch (time + gmtoff), so clock
// arithmetic is plain division — no timezone database on the board.
static void format_clock_time(long epoch, char* buf, size_t len) {
    long secs_today = epoch % 86400L;
    if (secs_today < 0) secs_today += 86400L;
    int h = (int)(secs_today / 3600);
    int m = (int)((secs_today / 60) % 60);
    if (clock_fmt == 12) {
        int h12 = h % 12; if (h12 == 0) h12 = 12;
        snprintf(buf, len, "%d:%02d", h12, m);
    } else {
        snprintf(buf, len, "%02d:%02d", h, m);
    }
}

// "in 12 min" / "in 2 h 15" / "now". Anything past today's horizon is the
// daemon's problem — it only sends what's worth counting down to.
static void format_lead_time(long secs, char* buf, size_t len) {
    if (secs <= 0) { snprintf(buf, len, "STARTING NOW"); return; }
    long mins = (secs + 59) / 60;
    if (mins < 60)      snprintf(buf, len, "IN %ld MIN", mins);
    else if (mins < 600) snprintf(buf, len, "IN %ld H %02ld", mins / 60, mins % 60);
    else                 snprintf(buf, len, "IN %ld H", mins / 60);
}

static void render_agenda(void);

static lv_obj_t* make_agenda_label(lv_obj_t* parent, const lv_font_t* font,
                                   lv_color_t color, int x, int y) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, "");
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, color, 0);
    lv_obj_set_pos(lbl, x, y);
    return lbl;
}

static void init_agenda_screen(lv_obj_t* scr) {
    agenda_container = lv_obj_create(scr);
    lv_obj_set_size(agenda_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(agenda_container, 0, 0);
    lv_obj_set_style_bg_opa(agenda_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(agenda_container, 0, 0);
    lv_obj_set_style_pad_all(agenda_container, 0, 0);
    lv_obj_clear_flag(agenda_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(agenda_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(agenda_container, global_click_cb, LV_EVENT_CLICKED, NULL);

    ag_date = make_agenda_label(agenda_container, L.ag_foot_font, COL_DIM,
                                L.margin, L.ag_date_y);

    // Hero card — the one thing readable from across the desk.
    ag_hero = make_panel(agenda_container, L.margin, L.ag_hero_y,
                         L.content_w, L.ag_hero_h);

    ag_stripe = lv_obj_create(ag_hero);
    lv_obj_set_size(ag_stripe, 5, L.ag_hero_h);
    lv_obj_set_pos(ag_stripe, -L.panel_pad_x, -L.panel_pad_y);
    lv_obj_set_style_bg_color(ag_stripe, COL_ACCENT, 0);
    lv_obj_set_style_bg_opa(ag_stripe, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ag_stripe, 0, 0);
    lv_obj_set_style_radius(ag_stripe, 0, 0);
    lv_obj_clear_flag(ag_stripe, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ag_stripe, LV_OBJ_FLAG_EVENT_BUBBLE);

    const int hero_inner_w = L.content_w - 2 * L.panel_pad_x;
    ag_kicker = make_agenda_label(ag_hero, L.ag_kicker_font, COL_ACCENT, 8, 4);
    ag_title  = make_agenda_label(ag_hero, L.ag_title_font, COL_TEXT, 8, L.ag_hero_h / 4);
    lv_label_set_long_mode(ag_title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(ag_title, hero_inner_w - 8);
    // Pin the height to one line. LV_LABEL_LONG_DOT only clips what exceeds
    // the label's *height*; with the height left to grow, a long title simply
    // wrapped onto a second line and drew straight over the time below it.
    lv_obj_set_height(ag_title, lv_font_get_line_height(L.ag_title_font));
    ag_meta   = make_agenda_label(ag_hero, L.ag_meta_font, COL_DIM, 8, L.ag_hero_h * 5 / 8);

    // Under the card, not inside it: an event that isn't running yet has no
    // progress to show, and a permanent gap reserved for one reads as a
    // layout mistake rather than an empty state.
    ag_progress = make_bar(agenda_container, L.margin + L.panel_pad_x + 8,
                           L.ag_hero_y + L.ag_hero_h + 10,
                           hero_inner_w - 8, 6);
    lv_obj_set_style_bg_color(ag_progress, COL_GREEN, LV_PART_INDICATOR);
    lv_obj_add_flag(ag_progress, LV_OBJ_FLAG_HIDDEN);

    // List rows. A row is a transparent strip so the whole set can be hidden
    // together when fewer items arrive than there are slots.
    for (int i = 0; i < AGENDA_MAX_ITEMS - 1; i++) {
        AgendaRow& r = agenda_rows[i];
        const int y = L.ag_row_y + i * L.ag_row_h;

        r.rule = lv_obj_create(agenda_container);
        lv_obj_set_size(r.rule, L.content_w, 1);
        lv_obj_set_pos(r.rule, L.margin, y);
        lv_obj_set_style_bg_color(r.rule, COL_BAR_BG, 0);
        lv_obj_set_style_bg_opa(r.rule, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(r.rule, 0, 0);
        lv_obj_set_style_radius(r.rule, 0, 0);
        lv_obj_add_flag(r.rule, LV_OBJ_FLAG_EVENT_BUBBLE);

        r.group = lv_obj_create(agenda_container);
        lv_obj_set_size(r.group, L.content_w, L.ag_row_h);
        lv_obj_set_pos(r.group, L.margin, y);
        lv_obj_set_style_bg_opa(r.group, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(r.group, 0, 0);
        lv_obj_set_style_pad_all(r.group, 0, 0);
        lv_obj_clear_flag(r.group, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(r.group, LV_OBJ_FLAG_EVENT_BUBBLE);

        const int text_y = (L.ag_row_h - 22) / 2;
        r.time = make_agenda_label(r.group, L.ag_time_font, COL_DIM, 0, text_y + 2);

        r.dot = lv_obj_create(r.group);
        lv_obj_set_size(r.dot, 9, 9);
        lv_obj_set_pos(r.dot, L.content_w / 6, L.ag_row_h / 2 - 4);
        lv_obj_set_style_border_width(r.dot, 0, 0);
        lv_obj_set_style_bg_opa(r.dot, LV_OPA_COVER, 0);
        lv_obj_add_flag(r.dot, LV_OBJ_FLAG_EVENT_BUBBLE);

        r.name = make_agenda_label(r.group, L.ag_row_font, COL_TEXT,
                                   L.content_w / 6 + 26, text_y);
        lv_label_set_long_mode(r.name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(r.name, L.content_w - (L.content_w / 6 + 26));
        // One line, same reason as the hero title: without a fixed height the
        // label wraps and the second line lands on the row below.
        lv_obj_set_height(r.name, lv_font_get_line_height(L.ag_row_font));
    }

    ag_foot = make_agenda_label(agenda_container, L.ag_foot_font, COL_DIM,
                                L.margin, L.ag_foot_y);

    // An empty day is a normal evening state, so it gets a composition of its
    // own rather than a blank panel.
    ag_empty_group = lv_obj_create(agenda_container);
    lv_obj_set_size(ag_empty_group, L.scr_w, L.scr_h);
    lv_obj_set_pos(ag_empty_group, 0, 0);
    lv_obj_set_style_bg_opa(ag_empty_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ag_empty_group, 0, 0);
    lv_obj_clear_flag(ag_empty_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ag_empty_group, LV_OBJ_FLAG_EVENT_BUBBLE);

    ag_empty_title = lv_label_create(ag_empty_group);
    lv_label_set_text(ag_empty_title, "Nothing left today");
    lv_obj_set_style_text_font(ag_empty_title, L.ag_title_font, 0);
    lv_obj_set_style_text_color(ag_empty_title, COL_TEXT, 0);
    lv_obj_align(ag_empty_title, LV_ALIGN_CENTER, 0, -22);

    ag_empty_sub = lv_label_create(ag_empty_group);
    lv_label_set_text(ag_empty_sub, "");
    lv_obj_set_style_text_font(ag_empty_sub, L.ag_meta_font, 0);
    lv_obj_set_style_text_color(ag_empty_sub, COL_DIM, 0);
    lv_obj_align(ag_empty_sub, LV_ALIGN_CENTER, 0, 26);

    render_agenda();   // start in the empty state rather than a bare skeleton
}

// ======== Alarm Screen ========

static void alarm_btn_cb(lv_event_t* e) {
    const bool done = ((lv_obj_t*)lv_event_get_target(e) == al_btn_done);
    if (alarm_cb && alarm_item_valid) alarm_cb(alarm_item.handle, done);
    alarm_item_valid = false;
    ui_show_screen(SCREEN_AGENDA);
}

static lv_obj_t* make_alarm_button(lv_obj_t* parent, int x, const char* text,
                                   lv_color_t bg, lv_color_t fg) {
    const int w = (L.content_w - L.margin) / 2;
    const int h = L.scr_h / 6;
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_pos(btn, x, L.scr_h - h - L.margin * 2);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 18, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, alarm_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, L.ag_btn_font, 0);
    lv_obj_set_style_text_color(lbl, fg, 0);
    lv_obj_center(lbl);
    return btn;
}

static void init_alarm_screen(lv_obj_t* scr) {
    alarm_container = lv_obj_create(scr);
    lv_obj_set_size(alarm_container, L.scr_w, L.scr_h);
    lv_obj_set_pos(alarm_container, 0, 0);
    lv_obj_set_style_bg_color(alarm_container, COL_BG, 0);
    lv_obj_set_style_bg_opa(alarm_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(alarm_container, 0, 0);
    lv_obj_set_style_pad_all(alarm_container, 0, 0);
    lv_obj_clear_flag(alarm_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(alarm_container, LV_OBJ_FLAG_HIDDEN);

    al_kicker = lv_label_create(alarm_container);
    lv_label_set_text(al_kicker, "REMINDER");
    lv_obj_set_style_text_font(al_kicker, L.ag_kicker_font, 0);
    lv_obj_set_style_text_color(al_kicker, COL_ACCENT, 0);
    lv_obj_align(al_kicker, LV_ALIGN_TOP_MID, 0, L.scr_h / 5);

    al_title = lv_label_create(alarm_container);
    lv_label_set_text(al_title, "");
    lv_obj_set_style_text_font(al_title, L.ag_alarm_font, 0);
    lv_obj_set_style_text_color(al_title, COL_TEXT, 0);
    lv_obj_set_style_text_align(al_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(al_title, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(al_title, L.content_w);
    lv_obj_align(al_title, LV_ALIGN_TOP_MID, 0, L.scr_h / 5 + 40);

    al_time = lv_label_create(alarm_container);
    lv_label_set_text(al_time, "");
    lv_obj_set_style_text_font(al_time, L.ag_time_font, 0);
    lv_obj_set_style_text_color(al_time, COL_DIM, 0);
    lv_obj_align(al_time, LV_ALIGN_TOP_MID, 0, L.scr_h * 3 / 5);

    // Two targets, thumb-sized: no physical button is free for this.
    al_btn_snooze = make_alarm_button(alarm_container, L.margin,
                                      "+10 MIN", COL_PANEL, COL_TEXT);
    al_btn_done   = make_alarm_button(alarm_container,
                                      L.margin + (L.content_w - L.margin) / 2 + L.margin,
                                      "DONE", COL_ACCENT, COL_BG);
}

// ======== Public API ========

void ui_init(void) {
    compute_layout(board_caps());

    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

#ifndef BOARD_HAS_PSRAM
    // Static corner mascot (see clawd_still.h) — the animated one needs PSRAM.
    if (L.small_icons) init_icon_dsc_rgb565a8(&logo_dsc, CLAWD_STILL_SMALL_W, CLAWD_STILL_SMALL_H, clawd_still_small_data);
    else               init_icon_dsc_rgb565a8(&logo_dsc, CLAWD_STILL_W, CLAWD_STILL_H, clawd_still_data);
#endif
    init_battery_icons();

    init_usage_screen(scr);
    init_agenda_screen(scr);
    init_alarm_screen(scr);
    splash_init(scr);

    if (splash_get_root()) {
        lv_obj_add_event_cb(splash_get_root(), global_click_cb, LV_EVENT_CLICKED, NULL);
    }

    // Corner mascot in the old logo slot. The still Clawd is shorter than the
    // 80/40 px slot the spark logo used; center it vertically in that slot.
    {
        const int slot  = L.small_icons ? LOGO_SMALL_HEIGHT : LOGO_HEIGHT;
        const int art_h = L.small_icons ? CLAWD_STILL_SMALL_H : CLAWD_STILL_H;
        const int top   = L.logo_y + (slot - art_h) / 2;
#ifdef BOARD_HAS_PSRAM
        // Animated: idles, does acts, and takes walk-off/lurk trips.
        splash_mascot_create(scr, L.margin, top + art_h, L.small_icons ? 2 : 3);
#else
        logo_img = lv_image_create(scr);
        lv_image_set_src(logo_img, &logo_dsc);
        lv_obj_set_pos(logo_img, L.margin, top);
#endif
    }

    battery_img = lv_image_create(scr);
    lv_image_set_src(battery_img, &battery_dscs[0]);
    lv_obj_set_pos(battery_img, L.scr_w - L.batt_w - L.margin, L.batt_y);
    // Boards without battery telemetry never show the indicator (per the HAL
    // contract; previously every board drew the empty-battery glyph).
    if (!board_caps().has_battery) {
        lv_obj_del(battery_img);
        battery_img = nullptr;
    }
}

void ui_update(const UsageData* data) {
    if (!data->valid) return;
    data_ok = data->ok;
    if (!data->ok) return;          // a {"ok":false} "no data" beat → fall through to idle, keep last numbers
    last_data_ms = lv_tick_get();   // a real usage update just landed
    data_received = true;

    if (data->clock_epoch > 0) {    // daemon supplied wall-clock time → drive the title clock
        clock_base_epoch = data->clock_epoch;
        clock_base_ms = last_data_ms;
        clock_fmt = data->clock_fmt;
    } else if (clock_base_epoch != 0) {   // clock turned off daemon-side → revert title to "Usage"
        clock_base_epoch = 0;
        clock_last_min = -1;
        lv_label_set_text(lbl_title, "Usage");
    }

    int s_pct = (int)(data->session_pct + 0.5f);

    if (data->enterprise) {
        // Spending box: big number-only label + small "%" symbol + desc + pace
        lv_obj_set_style_text_font(lbl_session_pct, L.ent_pct_font, 0);
        lv_label_set_text(lbl_session_label, "Spending");
        lv_obj_add_flag(lbl_session_reset, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lbl_spending_desc,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_spending_status,   LV_OBJ_FLAG_HIDDEN);
        if (panel_weekly) lv_obj_clear_flag(panel_weekly, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_set_style_text_font(lbl_session_pct, L.pct_font, 0);
        lv_label_set_text(lbl_session_label, "Current");
        lv_obj_clear_flag(lbl_session_reset, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_session_pct_sym, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_spending_desc,   LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lbl_spending_status, LV_OBJ_FLAG_HIDDEN);
        if (panel_weekly) lv_obj_clear_flag(panel_weekly, LV_OBJ_FLAG_HIDDEN);
    }

    char buf[48];

    // Pace vars used in both enterprise blocks below
    const char* pace_text = "Under pace";
    lv_color_t  pace_color = COL_GREEN;
    const char* pace_hex   = "788c5d";   // matches THEME_GREEN
    if (data->session_pct > (float)data->time_pct + 15.0f) {
        pace_text = "Over pace";  pace_color = COL_RED;   pace_hex = "c0392b";
    } else if (data->session_pct > (float)data->time_pct - 15.0f) {
        pace_text = "On pace";    pace_color = COL_AMBER; pace_hex = "d97757";
    }

    if (data->enterprise) {
        lv_label_set_text_fmt(lbl_session_pct, "%d", s_pct);
        lv_obj_align_to(lbl_session_pct_sym, lbl_session_pct,
                        LV_ALIGN_OUT_RIGHT_TOP, 4, 12);
    } else {
        lv_label_set_text_fmt(lbl_session_pct, "%d%%", s_pct);
        format_reset_time(data->session_reset_mins, buf, sizeof(buf));
        lv_label_set_text(lbl_session_reset, buf);
    }

    lv_bar_set_value(bar_session, s_pct, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar_session, pct_color(data->session_pct), LV_PART_INDICATOR);

    if (data->enterprise) {
        // Period box: time % + dynamic pace color + "Resets <date>" label
        lv_label_set_text(lbl_weekly_label, "Period");
        lv_label_set_text_fmt(lbl_weekly_pct, "%d%%", data->time_pct);
        lv_bar_set_value(bar_weekly, data->time_pct, LV_ANIM_ON);
        lv_color_t bar_pace = (data->session_pct <= (float)data->time_pct) ? COL_GREEN :
                              (data->session_pct <= (float)data->time_pct + 15.0f) ? COL_AMBER :
                              COL_RED;
        lv_obj_set_style_bg_color(bar_weekly, bar_pace, LV_PART_INDICATOR);
        snprintf(buf, sizeof(buf), "#%s %s# - #faf9f5 Resets %s#",
                 pace_hex, pace_text, data->reset_date);
        lv_label_set_text(lbl_weekly_reset, buf);
    } else {
        int w_pct = (int)(data->weekly_pct + 0.5f);
        lv_label_set_text_fmt(lbl_weekly_pct, "%d%%", w_pct);
        lv_bar_set_value(bar_weekly, w_pct, LV_ANIM_ON);
        lv_obj_set_style_bg_color(bar_weekly, pct_color(data->weekly_pct), LV_PART_INDICATOR);
        format_reset_time(data->weekly_reset_mins, buf, sizeof(buf));
        lv_label_set_text(lbl_weekly_reset, buf);
    }
}

// Pick the usage-view sub-screen: pairing hint (BLE down), the idle "Zzz" screen
// (connected but data has gone stale), or the live usage panels. Only re-lays-out
// on an actual change. The animated status line stays visible everywhere — it
// reads "Listening…" on the idle screen, keeping it alive rather than frozen.
static void update_view_state(void) {
    if (!usage_group || !pair_group || !idle_group) return;
    int v;
    if (!s_ble_connected) {
        v = 0;  // pairing hint
    } else if (data_received && data_ok && (lv_tick_get() - last_data_ms) < DATA_FRESH_MS) {
        v = 2;  // live usage
    } else {
        v = 1;  // idle / Zzz
    }
    if (v == view_state) return;
    view_state = v;
    lv_obj_add_flag(pair_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(idle_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(usage_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(v == 0 ? pair_group : v == 1 ? idle_group : usage_group,
                      LV_OBJ_FLAG_HIDDEN);
}

void ui_tick_anim(void) {
    if (current_screen != SCREEN_USAGE) return;
    update_view_state();
    if (view_state == 1) splash_mini_tick();   // animate the sleeping creature on the idle screen

    uint32_t now = lv_tick_get();

    // Title clock: once the daemon has sent wall-clock time, replace "Usage" with
    // the live time, advanced locally so it ticks every minute between payloads.
    if (clock_base_epoch > 0) {
        time_t cur = (time_t)(clock_base_epoch + (now - clock_base_ms) / 1000);
        struct tm tmv;
        gmtime_r(&cur, &tmv);   // epoch is already local wall-clock → gmtime keeps it as-is
        if (tmv.tm_min != clock_last_min) {   // only rewrite the title when the minute changes
            clock_last_min = tmv.tm_min;
            char tbuf[12];
            if (clock_fmt == 12) {
                int h12 = tmv.tm_hour % 12;
                if (h12 == 0) h12 = 12;
                snprintf(tbuf, sizeof(tbuf), "%d:%02d %s", h12, tmv.tm_min,
                         tmv.tm_hour < 12 ? "AM" : "PM");
            } else {
                snprintf(tbuf, sizeof(tbuf), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
            }
            lv_label_set_text(lbl_title, tbuf);
        }
    }

    if (now - anim_msg_start >= ANIM_MSG_MS) {
        anim_msg_idx = (anim_msg_idx + 1) % ANIM_MSG_COUNT;
        anim_msg_start = now;
    }

    if (now - anim_last_ms < spinner_ms[anim_spinner_idx]) return;
    anim_last_ms = now;
    anim_phase = (anim_phase + 1) % SPINNER_PHASES;
    anim_spinner_idx = (anim_phase < SPINNER_COUNT) ? anim_phase
                                                    : (SPINNER_PHASES - anim_phase);

    // Status text by priority. Whimsical messages only when connected & settled.
    const char* text;
    if (!s_ble_connected) {
        text = "Waiting";              // advertising / waiting for a host connection
    } else if (view_state == 1) {      // idle — alternate so it reads as alive AND data-less
        text = (anim_msg_idx & 1) ? "No data" : "Listening";
    } else if (now - connected_at_ms < 5000) {
        text = "Connected";
    } else {
        text = anim_messages[anim_msg_idx];
    }

    // All states share the whimsical style: "<glyph> <Title-case word>…"
    static char buf[80];
    snprintf(buf, sizeof(buf), "%s %s\xE2\x80\xA6",
             spinner_frames[anim_spinner_idx], text);
    lv_label_set_text(lbl_anim, buf);
}

static screen_t prev_non_splash_screen = SCREEN_USAGE;
static void apply_battery_visibility(void) {
    if (!battery_img) return;
    if (current_screen == SCREEN_SPLASH || current_screen == SCREEN_ALARM)
        lv_obj_add_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_clear_flag(battery_img, LV_OBJ_FLAG_HIDDEN);
}

static void global_click_cb(lv_event_t* e) {
    (void)e;
    ui_next_screen();
}

void ui_show_screen(screen_t screen) {
    lv_obj_add_flag(usage_container, LV_OBJ_FLAG_HIDDEN);
    if (agenda_container) lv_obj_add_flag(agenda_container, LV_OBJ_FLAG_HIDDEN);
    if (alarm_container)  lv_obj_add_flag(alarm_container, LV_OBJ_FLAG_HIDDEN);
    splash_hide();

    switch (screen) {
    case SCREEN_SPLASH:  splash_show(); break;
    case SCREEN_USAGE:   lv_obj_clear_flag(usage_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_AGENDA:  lv_obj_clear_flag(agenda_container, LV_OBJ_FLAG_HIDDEN); break;
    case SCREEN_ALARM:   lv_obj_clear_flag(alarm_container, LV_OBJ_FLAG_HIDDEN); break;
    default: break;
    }

    // The corner mascot lives on the root screen, so it would sit on top of
    // the agenda's date line and the alarm's copy. It belongs to the usage
    // view only.
    splash_mascot_set_visible(screen == SCREEN_USAGE);
    if (logo_img) {
        if (screen == SCREEN_USAGE) lv_obj_clear_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
        else                         lv_obj_add_flag(logo_img, LV_OBJ_FLAG_HIDDEN);
    }

    if (screen != SCREEN_SPLASH && screen != SCREEN_ALARM) prev_non_splash_screen = screen;
    current_screen = screen;
    apply_battery_visibility();
}

void ui_toggle_splash(void) {
    if (current_screen == SCREEN_SPLASH) ui_show_screen(prev_non_splash_screen);
    else                                  ui_show_screen(SCREEN_SPLASH);
}

screen_t ui_get_current_screen(void) {
    return current_screen;
}

void ui_update_ble_status(ble_state_t state, const char* name, const char* mac) {
    (void)name; (void)mac;
    bool was_connected = s_ble_connected;
    s_ble_connected = (state == BLE_STATE_CONNECTED);

    if (s_ble_connected && !was_connected) connected_at_ms = lv_tick_get();
    // pair / idle / usage — picked from connection + data freshness.
    update_view_state();
}

void ui_update_battery(int percent, bool charging) {
    if (!battery_img) return;
    int idx;
    if (charging) {
        idx = 4;
    } else if (percent < 0) {
        idx = 0;
    } else if (percent <= 10) {
        idx = 0;
    } else if (percent <= 35) {
        idx = 1;
    } else if (percent <= 75) {
        idx = 2;
    } else {
        idx = 3;
    }
    lv_image_set_src(battery_img, &battery_dscs[idx]);
    apply_battery_visibility();
}


// ======== Agenda public API ========

// The wall clock the daemon last handed us, carried forward by the tick
// counter. Both screens read the same base, so they can never disagree.
static long now_epoch(void) {
    if (clock_base_epoch == 0) return 0;
    return clock_base_epoch + (long)((lv_tick_get() - clock_base_ms) / 1000);
}

// Lay out the hero card for whatever items[0] currently is. Split out because
// the per-second tick redraws only this part.
static void render_agenda_hero(void) {
    if (!agenda.valid || agenda.count == 0) return;
    const AgendaItem& it = agenda.items[0];
    const long now = now_epoch();
    char buf[64];

    const long end = it.start_epoch + (long)it.duration_min * 60;
    const bool running = (it.duration_min > 0 && now >= it.start_epoch && now < end);

    if (running) {
        const long left_min = (end - now + 59) / 60;
        snprintf(buf, sizeof(buf), "NOW - %ld MIN LEFT", left_min);
        lv_obj_set_style_text_color(ag_kicker, COL_GREEN, 0);
        lv_obj_set_style_bg_color(ag_stripe, COL_GREEN, 0);
        const long span = end - it.start_epoch;
        const int  pct  = span > 0 ? (int)((now - it.start_epoch) * 100 / span) : 0;
        lv_bar_set_value(ag_progress, pct, LV_ANIM_OFF);
        lv_obj_clear_flag(ag_progress, LV_OBJ_FLAG_HIDDEN);
    } else {
        format_lead_time(it.start_epoch - now, buf, sizeof(buf));
        lv_obj_set_style_text_color(ag_kicker, agenda_color(it.color), 0);
        lv_obj_set_style_bg_color(ag_stripe, agenda_color(it.color), 0);
        lv_obj_add_flag(ag_progress, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(ag_kicker, buf);

    lv_label_set_text(ag_title, it.title);

    char from[12];
    format_clock_time(it.start_epoch, from, sizeof(from));
    if (it.duration_min > 0) {
        char to[12];
        format_clock_time(end, to, sizeof(to));
        snprintf(buf, sizeof(buf), "%s - %s", from, to);
    } else {
        snprintf(buf, sizeof(buf), "%s - reminder", from);
    }
    lv_label_set_text(ag_meta, buf);
}

static void render_agenda(void) {
    if (!agenda_container) return;

    lv_label_set_text(ag_date, agenda.date);

    // Before the first payload the screen would otherwise show its bare
    // skeleton — a panel, three rules and three default-styled dots — which
    // reads as a broken screen rather than an empty one.
    lv_label_set_text(ag_empty_title,
                      agenda.valid ? "Nothing left today" : "No agenda yet");
    lv_label_set_text(ag_empty_sub,
                      agenda.valid ? "" : "Waiting for the companion daemon");

    const bool empty = (!agenda.valid || agenda.count == 0);
    if (empty) {
        lv_obj_add_flag(ag_hero, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ag_foot, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < AGENDA_MAX_ITEMS - 1; i++) {
            lv_obj_add_flag(agenda_rows[i].group, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(agenda_rows[i].rule, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_clear_flag(ag_empty_group, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_add_flag(ag_empty_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ag_hero, LV_OBJ_FLAG_HIDDEN);
    render_agenda_hero();

    for (int i = 0; i < AGENDA_MAX_ITEMS - 1; i++) {
        AgendaRow& r = agenda_rows[i];
        const int idx = i + 1;
        if (idx >= agenda.count || i >= L.ag_rows) {
            lv_obj_add_flag(r.group, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(r.rule, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const AgendaItem& it = agenda.items[idx];
        char t[12];
        format_clock_time(it.start_epoch, t, sizeof(t));
        lv_label_set_text(r.time, t);
        lv_label_set_text(r.name, it.title);
        lv_obj_set_style_bg_color(r.dot, agenda_color(it.color), 0);
        // Shape carries the kind, colour is already spoken for by the calendar.
        lv_obj_set_style_radius(r.dot, it.is_reminder ? 2 : LV_RADIUS_CIRCLE, 0);
        lv_obj_clear_flag(r.group, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(r.rule, LV_OBJ_FLAG_HIDDEN);
    }

    if (agenda.more > 0) {
        lv_label_set_text_fmt(ag_foot, "+%d later", agenda.more);
        lv_obj_clear_flag(ag_foot, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ag_foot, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_update_agenda(const AgendaData* data) {
    if (!data || !data->valid) return;
    agenda = *data;
    if (data->clock_epoch > 0) {
        clock_base_epoch = data->clock_epoch;
        clock_base_ms = lv_tick_get();
    }
    ag_last_kicker_sec = -1;
    render_agenda();
}

// Called every loop; relays the hero card once a second so the countdown stays
// honest without the daemon having to poll at that rate.
void ui_tick_agenda(void) {
    if (current_screen != SCREEN_AGENDA) return;
    if (!agenda.valid || agenda.count == 0) return;
    const int sec = (int)(lv_tick_get() / 1000);
    if (sec == ag_last_kicker_sec) return;
    ag_last_kicker_sec = sec;
    render_agenda_hero();
}

void ui_show_alarm(const AgendaItem* item) {
    if (!item || !alarm_container) return;
    alarm_item = *item;
    alarm_item_valid = true;
    lv_label_set_text(al_kicker, item->is_reminder ? "REMINDER" : "STARTING NOW");
    lv_label_set_text(al_title, item->title);
    char t[12];
    format_clock_time(item->start_epoch, t, sizeof(t));
    lv_label_set_text(al_time, t);
    ui_show_screen(SCREEN_ALARM);
}

void ui_set_alarm_action_cb(ui_alarm_action_cb cb) {
    alarm_cb = cb;
}

// Tap / swipe rotation. The alarm screen is deliberately outside it: it is
// dismissed by acting on it, not by browsing past it.
void ui_next_screen(void) {
    switch (current_screen) {
    case SCREEN_USAGE:  ui_show_screen(SCREEN_AGENDA); break;
    case SCREEN_AGENDA: ui_show_screen(SCREEN_SPLASH); break;
    case SCREEN_SPLASH: ui_show_screen(SCREEN_USAGE);  break;
    default: break;
    }
}
