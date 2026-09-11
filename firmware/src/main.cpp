#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>

#include "data.h"
#include "ui.h"
#include "ble.h"
#include "splash.h"
#include "usage_rate.h"
#include "idle.h"
#include "idle_cfg.h"
#include "brightness.h"

#include "hal/board_caps.h"
#include "hal/display_hal.h"
#include "hal/touch_hal.h"
#include "hal/input_hal.h"
#include "hal/power_hal.h"
#include "hal/imu_hal.h"
#include "hal/sound_hal.h"

static UsageData usage = {};

// ---- LVGL draw buffers (partial render mode) ----
// PSRAM-equipped boards (S3) can comfortably hold larger strips. PSRAM-free
// boards (e.g. ESP32-C6) allocate from internal SRAM, so we shrink the strip
// — 480×20 RGB565 = 19 KB × 2 buffers = 38 KB, fits beside everything else.
#ifdef BOARD_HAS_PSRAM
#define BUF_LINES 40
#define LV_BUF_CAPS (MALLOC_CAP_SPIRAM)
#else
#define BUF_LINES 20
#define LV_BUF_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#endif
static uint16_t* buf1 = nullptr;
static uint16_t* buf2 = nullptr;

static uint32_t my_tick(void) { return millis(); }

static void my_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    display_hal_draw_bitmap(area->x1, area->y1, w, h, (uint16_t*)px_map);
    lv_display_flush_ready(disp);
}

static void rounder_cb(lv_event_t* e) {
    lv_area_t* area = (lv_area_t*)lv_event_get_param(e);
    display_hal_round_area(&area->x1, &area->y1, &area->x2, &area->y2);
}

// Touch policy is driven by IDLE_WAKE_ON_TOUCH:
//   true  → a press edge while asleep wakes the device and the first touch is
//           swallowed (mirrors the button wake-consumption); a press while
//           awake counts as activity.
//   false → touch never counts as activity and is fully swallowed while the
//           panel is dark, so pets/sleeves can't wake it overnight and LVGL
//           can't quietly toggle splash<->usage on a black panel.
// The panel image is rotated in software (display_hal_draw_bitmap on rotating
// boards); the touch controller knows nothing about it and always reports raw
// panel coordinates. Apply the inverse rotation here, or presses land where
// the unrotated layout used to be.
//
// This stayed hidden because every screen until the alarm reacted to a tap
// anywhere — the handler covers the full-screen container, so a press in the
// wrong place still toggled the view. The alarm screen was the first with
// small targets, and there the mismatch reads as "the buttons do nothing".
//
// Mapping mirrors rotate_strip(): 90° sends (x,y) to (S-1-y, x), so a panel
// point (px,py) came from (py, S-1-px). Square panels only, which is what
// every rotating board here is.
static void rotate_touch(uint16_t* x, uint16_t* y) {
    const BoardCaps& c = board_caps();
    if (!c.has_rotation || c.width != c.height) return;
    const uint16_t S = (uint16_t)(c.width - 1);
    const uint16_t px = *x, py = *y;
    switch (imu_hal_rotation_quadrant()) {
    case 1: *x = py;     *y = (uint16_t)(S - px); break;
    case 2: *x = (uint16_t)(S - px); *y = (uint16_t)(S - py); break;
    case 3: *x = (uint16_t)(S - py); *y = px;     break;
    default: break;   // 0° — panel and layout agree
    }
}

static void my_touch_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    uint16_t x, y;
    bool pressed;
    touch_hal_read(&x, &y, &pressed);
    rotate_touch(&x, &y);
    const bool raw_pressed = pressed;

    if (IDLE_WAKE_ON_TOUCH) {
        static bool touch_was = false;
        static bool touch_wake_swallowed = false;
        if (raw_pressed && !touch_was) {
            // Press edge — consume as wake if asleep.
            if (idle_consume_wake_press()) {
                touch_wake_swallowed = true;
                pressed = false;
            }
        } else if (!raw_pressed && touch_was) {
            // Release edge.
            if (touch_wake_swallowed) {
                touch_wake_swallowed = false;
                pressed = false;
            }
        } else if (raw_pressed && touch_wake_swallowed) {
            // Held finger through wake — keep hiding until release.
            pressed = false;
        }
        touch_was = raw_pressed;
    } else if (idle_is_asleep()) {
        pressed = false;
    }

    if (pressed) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// Parse a JSON line into UsageData.
static bool parse_json(const char* json, UsageData* out) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("JSON parse error: %s\n", err.c_str());
        return false;
    }

    out->session_pct = doc["s"] | 0.0f;
    out->session_reset_mins = doc["sr"] | -1;
    out->weekly_pct = doc["w"] | 0.0f;
    out->weekly_reset_mins = doc["wr"] | -1;
    strlcpy(out->status, doc["st"] | "unknown", sizeof(out->status));
    out->chime = doc["c"] | false;   // absent (old daemon / chime off) → stay silent
    const char* acct = doc["acct"] | "pro";
    out->enterprise = (strcmp(acct, "ent") == 0);
    out->time_pct = doc["tp"] | 0;
    out->period_days = doc["pd"] | 30;
    strlcpy(out->reset_date, doc["rd"] | "", sizeof(out->reset_date));
    out->clock_epoch = doc["t"] | 0L;
    out->clock_fmt = doc["tf"] | 24;
    out->ok = doc["ok"] | false;
    out->valid = true;
    return true;
}

// ---- Agenda (companion channel) ----

static AgendaData agenda;

// Alarms already answered, so a re-sent payload doesn't ring twice. Four slots
// is one per item: the daemon never has more than that in flight.
static char fired_handles[AGENDA_MAX_ITEMS][AGENDA_HANDLE_LEN];
static int  fired_next = 0;

static bool alarm_already_fired(const char* handle) {
    for (int i = 0; i < AGENDA_MAX_ITEMS; i++)
        if (strcmp(fired_handles[i], handle) == 0) return true;
    return false;
}

static void mark_alarm_fired(const char* handle) {
    strlcpy(fired_handles[fired_next], handle, AGENDA_HANDLE_LEN);
    fired_next = (fired_next + 1) % AGENDA_MAX_ITEMS;
}

// Wall clock carried forward between payloads, so alarms fire on time even
// while the link is down. Both the usage and the agenda payload feed it.
static long     wall_base_epoch = 0;
static uint32_t wall_base_ms = 0;

static void note_wall_clock(long epoch) {
    if (epoch <= 0) return;
    wall_base_epoch = epoch;
    wall_base_ms = millis();
}

static long wall_now(void) {
    if (wall_base_epoch == 0) return 0;
    return wall_base_epoch + (long)((millis() - wall_base_ms) / 1000);
}

static bool parse_agenda(const char* json, AgendaData* out) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("agenda parse error: %s\n", err.c_str());
        return false;
    }
    if (strcmp(doc["k"] | "", "agenda") != 0) {
        Serial.println("companion payload of unknown kind — ignored");
        return false;
    }

    out->clock_epoch = doc["t"] | 0L;
    out->more  = doc["m"] | 0;
    out->quiet = doc["q"] | false;
    strlcpy(out->date, doc["d"] | "", sizeof(out->date));

    out->count = 0;
    for (JsonObject e : doc["e"].as<JsonArray>()) {
        if (out->count >= AGENDA_MAX_ITEMS) break;
        AgendaItem& it = out->items[out->count];
        it.start_epoch  = e["s"] | 0L;
        it.alarm_epoch  = e["a"] | 0L;
        it.duration_min = e["dur"] | 0;
        it.color        = (unsigned char)(e["c"] | 0);
        it.is_reminder  = e["r"] | false;
        strlcpy(it.title, e["n"] | "", sizeof(it.title));
        strlcpy(it.handle, e["h"] | "", sizeof(it.handle));
        out->count++;
    }
    out->valid = true;
    return true;
}

// Ring for anything whose alarm has come due. The board owns this timing on
// purpose: a reminder that only fires when the mac is awake is not a reminder.
static void check_agenda_alarms(void) {
    if (!agenda.valid) return;
    const long now = wall_now();
    if (now == 0) return;
    if (ui_get_current_screen() == SCREEN_ALARM) return;

    for (int i = 0; i < agenda.count; i++) {
        const AgendaItem& it = agenda.items[i];
        if (it.alarm_epoch <= 0 || it.alarm_epoch > now) continue;
        if (it.handle[0] == '\0' || alarm_already_fired(it.handle)) continue;

        mark_alarm_fired(it.handle);
        idle_note_activity();            // wake the panel if it had gone dark
        if (!agenda.quiet) sound_hal_play_reset();
        ui_show_alarm(&it);
        Serial.printf("agenda alarm: %s%s\n", it.title, agenda.quiet ? " (quiet)" : "");
        return;                          // one at a time; the rest wait their turn
    }
}

// The user answered a ringing reminder. Snoozing is the daemon's job — it
// reschedules and sends a fresh payload — so the board only reports the press.
static void on_alarm_action(const char* handle, bool done) {
    char msg[64];
    snprintf(msg, sizeof(msg), "{\"a\":\"%s\",\"h\":\"%s\"}",
             done ? "done" : "snooze", handle);
    ble_companion_notify(msg);
    Serial.printf("agenda action: %s\n", msg);
}

// Feed the agenda screen a canned payload over serial, so the layout can be
// flashed and screenshotted before the companion daemon exists. Times are
// relative to now, so the countdown and the "running" state both animate.
static void inject_demo_agenda(void) {
    const long base = wall_now() ? wall_now() : 1757340000L;  // arbitrary but sane
    char json[512];
    snprintf(json, sizeof(json),
        "{\"k\":\"agenda\",\"t\":%ld,\"d\":\"Mon 8 Sep\",\"m\":2,\"e\":["
        "{\"s\":%ld,\"dur\":45,\"n\":\"Sync with Lisa\",\"h\":\"a1\",\"c\":0,\"a\":0},"
        "{\"s\":%ld,\"dur\":60,\"n\":\"Estimate review\",\"h\":\"a2\",\"c\":1,\"a\":0},"
        "{\"s\":%ld,\"dur\":0,\"n\":\"Collect the order\",\"h\":\"a3\",\"c\":0,\"r\":true,\"a\":%ld},"
        "{\"s\":%ld,\"dur\":90,\"n\":\"Dinner at Mark's\",\"h\":\"a4\",\"c\":2,\"a\":0}]}",
        base, base + 720, base + 5400, base + 9000, base + 30, base + 16200);
    if (parse_agenda(json, &agenda)) {
        note_wall_clock(agenda.clock_epoch);
        ui_update_agenda(&agenda);
        ui_show_screen(SCREEN_AGENDA);
        Serial.println("demo agenda loaded — the reminder rings in 30 s");
    }
}

// ---- Serial command buffer ----
#define CMD_BUF_SIZE 64
static char cmd_buf[CMD_BUF_SIZE];
static int cmd_pos = 0;

static void send_screenshot() {
#ifndef BOARD_HAS_PSRAM
    // A full RGB565 framebuffer doesn't fit in internal SRAM on PSRAM-free
    // boards (e.g. 480×480×2 = 460 KB). Capture is unsupported there.
    Serial.println("SCREENSHOT_UNSUPPORTED");
    return;
#else
    const uint32_t w = board_caps().width;
    const uint32_t h = board_caps().height;
    const uint32_t row_bytes = w * 2;
    const uint32_t buf_size = row_bytes * h;
    uint8_t* sbuf = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!sbuf) {
        Serial.println("SCREENSHOT_ERR");
        return;
    }

    lv_draw_buf_t draw_buf;
    lv_draw_buf_init(&draw_buf, w, h, LV_COLOR_FORMAT_RGB565, row_bytes, sbuf, buf_size);

    lv_result_t res = lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565, &draw_buf);
    if (res != LV_RESULT_OK) {
        heap_caps_free(sbuf);
        Serial.println("SCREENSHOT_ERR");
        return;
    }

    Serial.printf("SCREENSHOT_START %lu %lu %lu\n",
        (unsigned long)w, (unsigned long)h, (unsigned long)buf_size);
    Serial.flush();
    Serial.write(sbuf, buf_size);
    Serial.flush();
    Serial.println();
    Serial.println("SCREENSHOT_END");
    heap_caps_free(sbuf);
#endif
}

static void check_serial_cmd() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            cmd_buf[cmd_pos] = '\0';
            if (strcmp(cmd_buf, "screenshot") == 0) send_screenshot();
            else if (strcmp(cmd_buf, "buzz") == 0)  sound_hal_play_reset();
            else if (strcmp(cmd_buf, "agenda") == 0) inject_demo_agenda();
            cmd_pos = 0;
        } else if (cmd_pos < CMD_BUF_SIZE - 1) {
            cmd_buf[cmd_pos++] = c;
        }
    }
}

// Each board provides this. Must bring up the shared I2C bus (Wire.begin
// with the board's SDA/SCL pins) and any board-private hardware that has
// to settle before display/touch (e.g. an IO expander gating the LCD
// reset line). Called exactly once at the start of setup().
extern "C" void board_init(void);

void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("{\"ready\":true}");

    board_init();

    display_hal_init();
    display_hal_begin();
    idle_init();        // takes over panel brightness and starts the idle timer
    brightness_init();  // load the user's saved brightness level and apply via idle

    power_hal_init();
    imu_hal_init();
    sound_hal_init();
    touch_hal_init();

    // ---- LVGL ----
    const int W = board_caps().width;
    const int H = board_caps().height;

    lv_init();
    lv_tick_set_cb(my_tick);

    buf1 = (uint16_t*)heap_caps_malloc(W * BUF_LINES * 2, LV_BUF_CAPS);
    buf2 = (uint16_t*)heap_caps_malloc(W * BUF_LINES * 2, LV_BUF_CAPS);

    lv_display_t* disp = lv_display_create(W, H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, my_flush_cb);
    lv_display_set_buffers(disp, buf1, buf2, W * BUF_LINES * 2,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_add_event_cb(disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, my_touch_cb);

    ble_init();
    input_hal_init();

    ui_init();
    ui_update_ble_status(ble_get_state(), ble_get_device_name(), ble_get_mac_address());
    ui_update_battery(power_hal_battery_pct(), power_hal_is_charging());
    ui_set_alarm_action_cb(on_alarm_action);
    ui_show_screen(SCREEN_SPLASH);

    Serial.printf("Dashboard ready (%s, %dx%d), waiting for data on BLE...\n",
        board_caps().name, W, H);
}

static ble_state_t last_ble_state = BLE_STATE_INIT;

// Hold-to-pair gesture: hold the PWR button ~3s, then RELEASE → clear all BLE
// bonds and re-advertise. Clearing on *release* (not while held) is deliberate:
// holding to power the device OFF (AXP hardware shutdown at 8s) must not wipe
// the bond — a power-off hold never releases before shutdown. To stop a
// "chicken-out" release just before 8s from pairing, the gesture disarms at 6s.
//
//   ~1.5s long-press edge → PENDING
//   3.0s (+1500)          → ARMED   (release from here clears bonds)
//   6.0s (+4500)          → DISARMED (no clear; AXP powers off at 8s)
#define PAIR_ARM_AFTER_LONG_MS    1500   // 3.0s total
#define PAIR_DISARM_AFTER_LONG_MS 4500   // 6.0s total
enum pair_state_t { PAIR_IDLE, PAIR_PENDING, PAIR_ARMED };
static pair_state_t pair_state        = PAIR_IDLE;
static uint32_t     pair_long_seen_ms = 0;

static void pair_tick(void) {
    if (pair_state == PAIR_IDLE && power_hal_pwr_long_pressed()) {
        pair_state = PAIR_PENDING;
        pair_long_seen_ms = millis();
        (void)power_hal_pwr_released();  // drain any stale release edge
        Serial.println("PWR long-press: hold to ~3s then release to pair");
        return;
    }
    if (pair_state == PAIR_IDLE) return;

    if (power_hal_pwr_released()) {
        if (pair_state == PAIR_ARMED) {
            Serial.println("Pair: released in window — clearing bonds, advertising");
            ble_clear_bonds();
        } else {
            Serial.println("Pair: released too early — cancelled");
        }
        pair_state = PAIR_IDLE;
        return;
    }

    uint32_t held = millis() - pair_long_seen_ms;
    if (pair_state == PAIR_PENDING && held >= PAIR_ARM_AFTER_LONG_MS) {
        pair_state = PAIR_ARMED;
        Serial.println("Pair: armed — release to pair");
    } else if (pair_state == PAIR_ARMED && held >= PAIR_DISARM_AFTER_LONG_MS) {
        pair_state = PAIR_IDLE;  // power-off territory; don't pair
        Serial.println("Pair: disarmed (holding toward power-off)");
    }
}

void loop() {
    idle_tick();
    lv_timer_handler();
    ui_tick_anim();
    ble_tick();
    power_hal_tick();
    imu_hal_tick();
    sound_hal_tick();
    splash_tick();
    splash_mascot_tick();
    // Rotation transition (blank + ramp) would fight the idle fade — skip
    // ticks while the panel is dark. A rotation that happens during sleep
    // is detected by the next tick after wake and ramped in then.
    if (!idle_is_asleep()) display_hal_tick();

    // ---- Physical buttons ----
    //   PRIMARY   → HID Space  (Claude Code voice-mode PTT)
    //   SECONDARY → HID Shift+Tab  (mode toggle; only if the board has one)
    //   PWR       → on splash: cycle animations; on usage: cycle brightness;
    //               hold ~3s + release: pairing mode
    // First press from sleep is consumed as a wake-only event by
    // idle_consume_wake_press(); the normal action fires from the second
    // press. Activity bookkeeping happens inside idle_consume_wake_press
    // so no separate idle_note_activity() call is needed here.
    {
        static bool primary_was = false;
        static bool primary_wake_swallowed = false;
        bool primary_now = input_hal_is_held(INPUT_BTN_PRIMARY);
        if (primary_now != primary_was) {
            if (primary_now) {
                if (idle_consume_wake_press()) primary_wake_swallowed = true;
                else                            ble_keyboard_press(0x2C, 0);  // HID Space, no mods
            } else {
                if (primary_wake_swallowed) primary_wake_swallowed = false;
                else                        ble_keyboard_release();
            }
            primary_was = primary_now;
        }

        if (board_caps().button_count >= 2) {
            static bool secondary_was = false;
            static bool secondary_wake_swallowed = false;
            bool secondary_now = input_hal_is_held(INPUT_BTN_SECONDARY);
            if (secondary_now != secondary_was) {
                if (secondary_now) {
                    if (idle_consume_wake_press()) secondary_wake_swallowed = true;
                    else                            ble_keyboard_press(0x2B, 0x02);  // HID Tab + LEFT_SHIFT
                } else {
                    if (secondary_wake_swallowed) secondary_wake_swallowed = false;
                    else                          ble_keyboard_release();
                }
                secondary_was = secondary_now;
            }
        }

        if (power_hal_pwr_pressed()) {
            if (!idle_consume_wake_press()) {
                // On splash: cycle animations. On the usage view: cycle
                // screen brightness (single non-splash view, no more screens).
                if (ui_get_current_screen() == SCREEN_SPLASH) splash_next();
                else                                          brightness_cycle();
            }
        }

        pair_tick();
    }

    ble_state_t bs = ble_get_state();
    if (bs != last_ble_state) {
        last_ble_state = bs;
        ui_update_ble_status(bs, ble_get_device_name(), ble_get_mac_address());
    }

    static int  last_pct      = -2;
    static bool last_charging = false;
    int  pct      = power_hal_battery_pct();
    bool charging = power_hal_is_charging();
    if (pct != last_pct || charging != last_charging) {
        if (pct != last_pct) ble_set_battery_level(pct);
        last_pct = pct;
        last_charging = charging;
        ui_update_battery(pct, charging);
    }

    check_serial_cmd();

    if (ble_has_companion()) {
        if (parse_agenda(ble_get_companion(), &agenda)) {
            note_wall_clock(agenda.clock_epoch);
            ui_update_agenda(&agenda);
        }
    }

    ui_tick_agenda();
    check_agenda_alarms();

    if (ble_has_data()) {
        if (parse_json(ble_get_data(), &usage)) {
            note_wall_clock(usage.clock_epoch);
            int g_before = usage_rate_group();
            bool session_reset = usage_rate_sample(usage.session_pct);
            int g_after = usage_rate_group();
            // 5-hour session limit refilled → chime so the user knows they can
            // use Claude again (no-op on boards without a buzzer). Gated on the
            // daemon's opt-in `chime` config; the `buzz` serial cmd ignores it.
            if (session_reset && usage.chime) {
                Serial.println("session reset detected — chime");
                sound_hal_play_reset();
            }
            if (g_after != g_before) {
                Serial.printf("usage rate: group %d -> %d (s=%.2f%%)\n",
                    g_before, g_after, usage.session_pct);
                if (splash_is_active()) splash_pick_for_current_rate();
            }
            ui_update(&usage);
            ble_send_ack();
        } else {
            ble_send_nack();
        }
    }

    delay(5);
}
