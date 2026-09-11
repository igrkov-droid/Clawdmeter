#pragma once
#include <Arduino.h>

struct UsageData {
    float session_pct;       // utilization 0-100 (5h window Pro/Max; spending % Enterprise)
    int session_reset_mins;  // minutes until reset
    float weekly_pct;        // 7-day utilization (Pro/Max only; 0 for Enterprise)
    int weekly_reset_mins;   // minutes until weekly reset (Pro/Max only)
    char status[16];         // "allowed", "limited", etc.
    bool chime;              // play the session-reset chime; false unless daemon opts in
    bool enterprise;         // true = Enterprise spending-limit account
    int time_pct;            // 0-100: fraction of billing period elapsed (Enterprise)
    int period_days;         // total billing period length in days (Enterprise)
    char reset_date[12];     // formatted reset date e.g. "Jul 1" (Enterprise)
    long clock_epoch;        // local wall-clock epoch (s) from daemon; 0 = not provided
    int  clock_fmt;          // 12 or 24 (hour format from daemon); defaults to 24
    bool ok;                 // data parse succeeded
    bool valid;              // false until first successful parse
};

// ---- Agenda (calendar events + reminders from the companion daemon) ----
//
// The mac does all the shaping: it expands recurrences, sorts, truncates
// titles to what a row can show, and picks which handful is worth sending.
// The board only counts time, so "in 12 min" stays right between polls.

#define AGENDA_MAX_ITEMS   4
// 64 bytes = 24 Cyrillic glyphs plus the ellipsis, or ~60 Latin ones. The old
// 40 fit 24 Latin glyphs but only 19 Cyrillic, so Russian titles arrived cut.
#define AGENDA_TITLE_LEN   64
#define AGENDA_HANDLE_LEN  9    // opaque token; the daemon maps it back to EventKit

struct AgendaItem {
    long start_epoch;    // local wall-clock epoch (s), same basis as UsageData::clock_epoch
    long alarm_epoch;    // when to ring; 0 = never
    int  duration_min;   // 0 = a reminder, which has no span
    char title[AGENDA_TITLE_LEN];
    char handle[AGENDA_HANDLE_LEN];
    unsigned char color; // 0 = accent, 1 = green, 2 = dim — the calendar's colour
    bool is_reminder;    // square marker instead of a round one
};

struct AgendaData {
    AgendaItem items[AGENDA_MAX_ITEMS];
    int  count;
    int  more;           // how many further items didn't fit — "+N later"
    long clock_epoch;    // the daemon's wall clock when this was sent
    char date[16];       // formatted by the daemon, e.g. "Mon 8 Sep"
    bool quiet;          // inside the daemon's quiet hours — show, don't ring
    bool valid;          // false until the first payload parses
};
