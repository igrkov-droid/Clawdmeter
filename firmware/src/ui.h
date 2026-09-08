#pragma once
#include "data.h"
#include "ble.h"

enum screen_t {
    SCREEN_SPLASH,
    SCREEN_USAGE,
    SCREEN_AGENDA,
    SCREEN_ALARM,    // outside the browse rotation: dismissed by acting on it
    SCREEN_COUNT,
};

// Fired when the user answers a ringing reminder. `done` distinguishes the
// two buttons; the handle is the daemon's opaque token for the item.
typedef void (*ui_alarm_action_cb)(const char* handle, bool done);

void ui_init(void);
void ui_update(const UsageData* data);
void ui_tick_anim(void);
void ui_show_screen(screen_t screen);
void ui_toggle_splash(void);
screen_t ui_get_current_screen(void);
void ui_update_ble_status(ble_state_t state, const char* name, const char* mac);
void ui_update_battery(int percent, bool charging);

void ui_update_agenda(const AgendaData* data);
void ui_tick_agenda(void);
void ui_show_alarm(const AgendaItem* item);
void ui_set_alarm_action_cb(ui_alarm_action_cb cb);
void ui_next_screen(void);
