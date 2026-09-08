#!/usr/bin/env python3
"""Clawdmeter companion daemon — calendar events and reminders over BLE.

Runs alongside claude_usage_daemon.py rather than inside it. The two talk to
the same board over separate GATT characteristics, so a hung IMAP-style source
or a revoked EventKit permission here can never take the usage display down
with it. CoreBluetooth multiplexes one physical link between processes, so the
second connection costs no extra pairing.

Everything the board cannot cheaply do happens here: recurrences are expanded
by EventKit, items are merged and sorted, titles are truncated to what a row
can show, and the date is formatted. The board is left with the one job it
must own — counting down — so a reminder still rings when this mac is asleep.

Permissions: full access to Calendars and to Reminders. Under launchd the TCC
prompt is attributed to the python binary itself, and that grant is keyed to
its path, so a Homebrew python upgrade silently revokes it. If the daemon
starts logging "no access", re-grant in System Settings → Privacy & Security.
"""

import asyncio
import json
import re
import sys
import time
from datetime import datetime, timedelta
from pathlib import Path

from bleak import BleakClient, BleakScanner

import objc
from Foundation import (
    NSCalendarUnitDay,
    NSCalendarUnitHour,
    NSCalendarUnitMinute,
    NSCalendarUnitMonth,
    NSCalendarUnitYear,
    NSDate,
    NSCalendar,
    NSDefaultRunLoopMode,
    NSNotificationCenter,
    NSObject,
    NSRunLoop,
)
from EventKit import EKEventStore, EKAlarm

try:
    from EventKit import EKEntityTypeEvent, EKEntityTypeReminder
except ImportError:      # older pyobjc-framework-EventKit exposes only the values
    EKEntityTypeEvent, EKEntityTypeReminder = 0, 1

DEVICE_NAME = "Clawdmeter"
CMP_CHAR_UUID = "4c41555a-4465-7669-6365-000000000005"
SAVED_ADDR_FILE = Path.home() / ".config" / "claude-usage-monitor" / "ble-address"
CONFIG_FILE = Path.home() / ".config" / "claude-usage-monitor" / "config"

POLL_INTERVAL = 30          # seconds between full EventKit sweeps
TICK = 1.0                  # loop period; also how often store-change events are pumped
CONNECT_TIMEOUT = 20.0
HORIZON_HOURS = 36          # how far ahead to look for the "next up" list

MAX_ITEMS = 4               # must match AGENDA_MAX_ITEMS in firmware/src/data.h
TITLE_CHARS = 24            # what one row shows at styrene_20 on a 480px panel
SNOOZE_MINUTES = 10

# One ATT write without response is capped at MTU-3, which is 182 bytes on
# macOS. Two of those bytes carry the fragment header, so this is what is left
# for payload. Fragmenting always works; whether a single long write would fit
# is what tools/ble_mtu_probe.py exists to answer.
FRAGMENT_BYTES = 176


def log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


# ---------------------------------------------------------------- config

def read_config() -> dict:
    """Read the shared config file. Same format as the usage daemon: key = value,
    `#` starts a comment, re-read every cycle so edits land without a restart."""
    cfg = {}
    try:
        if CONFIG_FILE.exists():
            for line in CONFIG_FILE.read_text().splitlines():
                line = line.split("#", 1)[0].strip()
                if "=" in line:
                    key, val = line.split("=", 1)
                    cfg[key.strip().lower()] = val.strip()
    except OSError:
        pass
    return cfg


def parse_quiet_hours(raw: str | None) -> tuple[int, int] | None:
    """`22:00-08:00` → (1320, 480), both as minutes past midnight. A window that
    wraps past midnight is normal and handled at comparison time."""
    if not raw:
        return None
    m = re.match(r"^\s*(\d{1,2}):(\d{2})\s*-\s*(\d{1,2}):(\d{2})\s*$", raw)
    if not m:
        log(f"ignoring malformed agenda_quiet value: {raw!r}")
        return None
    a = int(m.group(1)) * 60 + int(m.group(2))
    b = int(m.group(3)) * 60 + int(m.group(4))
    return (a, b)


def in_quiet_hours(window: tuple[int, int] | None, when: datetime) -> bool:
    if window is None:
        return False
    start, end = window
    now = when.hour * 60 + when.minute
    if start > end:            # window crosses midnight, e.g. 22:00-08:00
        return now >= start or now < end
    return start <= now < end


# ---------------------------------------------------------------- EventKit

class StoreObserver(NSObject):
    """Flips a flag when anything in the calendar database changes, so an edit
    in Calendar.app reaches the board immediately instead of on the next sweep."""

    def initWithCallback_(self, callback):
        self = objc.super(StoreObserver, self).init()
        if self is None:
            return None
        self._callback = callback
        return self

    def storeChanged_(self, note):
        self._callback()


class Agenda:
    """Wraps EKEventStore: access, fetching, and the two write-backs the board
    can trigger. Handles are short opaque tokens ("e0".."e3") because an
    EventKit identifier is a 36-character UUID and the payload has no room for
    four of them; this object owns the mapping back."""

    def __init__(self):
        self.store = EKEventStore.alloc().init()
        self.handles: dict[str, object] = {}
        self.dirty = True
        self._observer = StoreObserver.alloc().initWithCallback_(self._mark_dirty)
        NSNotificationCenter.defaultCenter().addObserver_selector_name_object_(
            self._observer, "storeChanged:", "EKEventStoreChangedNotification", None
        )

    def _mark_dirty(self):
        self.dirty = True

    def pump(self):
        """Give the run loop a moment so EventKit's change notification is
        delivered. Without this the observer above never fires in an asyncio
        process, since nothing else drives a CFRunLoop."""
        NSRunLoop.currentRunLoop().runMode_beforeDate_(
            NSDefaultRunLoopMode, NSDate.dateWithTimeIntervalSinceNow_(0.01)
        )

    def request_access(self) -> bool:
        done = {"granted": False, "finished": False}

        def handler(granted, err):
            done["granted"] = bool(granted)
            done["finished"] = True

        # macOS 14 split full from write-only access; older systems only have
        # the entity-type call. Ask for whichever this system offers.
        if hasattr(self.store, "requestFullAccessToEventsWithCompletion_"):
            self.store.requestFullAccessToEventsWithCompletion_(handler)
            self._wait(done)
            events_ok = done["granted"]
            done = {"granted": False, "finished": False}
            self.store.requestFullAccessToRemindersWithCompletion_(handler)
            self._wait(done)
            return events_ok and done["granted"]

        self.store.requestAccessToEntityType_completion_(EKEntityTypeEvent, handler)
        self._wait(done)
        events_ok = done["granted"]
        done = {"granted": False, "finished": False}
        self.store.requestAccessToEntityType_completion_(EKEntityTypeReminder, handler)
        self._wait(done)
        return events_ok and done["granted"]

    def _wait(self, done: dict, timeout: float = 30.0):
        deadline = time.time() + timeout
        while not done["finished"] and time.time() < deadline:
            self.pump()

    # -- reading ---------------------------------------------------------

    def _events(self, start: NSDate, end: NSDate) -> list[dict]:
        pred = self.store.predicateForEventsWithStartDate_endDate_calendars_(
            start, end, None
        )
        out = []
        for ev in self.store.eventsMatchingPredicate_(pred) or []:
            if ev.isAllDay():
                continue          # an all-day banner has no time to count down to
            begin = ev.startDate().timeIntervalSince1970()
            finish = ev.endDate().timeIntervalSince1970()
            out.append({
                "obj": ev,
                "start": begin,
                "duration": max(0, int((finish - begin) / 60)),
                "title": ev.title() or "(no title)",
                "alarm": self._first_alarm(ev, begin),
                "reminder": False,
                "color": calendar_color_index(ev.calendar()),
            })
        return out

    def _reminders(self, start: NSDate, end: NSDate) -> list[dict]:
        pred = self.store.predicateForIncompleteRemindersWithDueDateStarting_ending_calendars_(
            start, end, None
        )
        box = {"items": None}

        def handler(items):
            box["items"] = items or []

        self.store.fetchRemindersMatchingPredicate_completion_(pred, handler)
        deadline = time.time() + 10.0
        while box["items"] is None and time.time() < deadline:
            self.pump()

        out = []
        cal = NSCalendar.currentCalendar()
        for rem in box["items"] or []:
            comps = rem.dueDateComponents()
            if comps is None:
                continue
            due = cal.dateFromComponents_(comps)
            if due is None:
                continue
            when = due.timeIntervalSince1970()
            out.append({
                "obj": rem,
                "start": when,
                "duration": 0,
                "title": rem.title() or "(no title)",
                # A reminder without an explicit alarm still rings at its due
                # time — that is what a due time means to the person who set it.
                "alarm": self._first_alarm(rem, when) or when,
                "reminder": True,
                "color": calendar_color_index(rem.calendar()),
            })
        return out

    @staticmethod
    def _first_alarm(obj, anchor: float) -> float | None:
        """Earliest alarm as an absolute epoch. EventKit stores them either
        absolutely or as an offset from the item's own date."""
        best = None
        for alarm in obj.alarms() or []:
            absolute = alarm.absoluteDate()
            when = absolute.timeIntervalSince1970() if absolute else anchor + alarm.relativeOffset()
            best = when if best is None else min(best, when)
        return best

    def collect(self) -> tuple[list[dict], int]:
        """Everything worth showing, soonest first, with the count that didn't fit."""
        now = NSDate.date()
        end = NSDate.dateWithTimeIntervalSinceNow_(HORIZON_HOURS * 3600)
        items = self._events(now, end) + self._reminders(now, end)
        items.sort(key=lambda i: i["start"])

        self.handles = {}
        for idx, item in enumerate(items[:MAX_ITEMS]):
            item["handle"] = f"e{idx}"
            self.handles[item["handle"]] = item["obj"]
        return items[:MAX_ITEMS], max(0, len(items) - MAX_ITEMS)

    # -- writing back ----------------------------------------------------

    def complete(self, handle: str) -> bool:
        obj = self.handles.get(handle)
        if obj is None or not hasattr(obj, "setCompleted_"):
            return False
        obj.setCompleted_(True)
        ok, err = self.store.saveReminder_commit_error_(obj, True, None)
        if not ok:
            log(f"could not complete {handle}: {err}")
        return bool(ok)

    def snooze(self, handle: str) -> bool:
        """Push the item out by SNOOZE_MINUTES. Reminders move their due date;
        an event keeps its time and just gets a fresh alarm, since moving a
        meeting because you dismissed a warning would be wrong."""
        obj = self.handles.get(handle)
        if obj is None:
            return False
        when = NSDate.dateWithTimeIntervalSinceNow_(SNOOZE_MINUTES * 60)

        if hasattr(obj, "setCompleted_"):
            cal = NSCalendar.currentCalendar()
            units = (NSCalendarUnitYear | NSCalendarUnitMonth | NSCalendarUnitDay
                     | NSCalendarUnitHour | NSCalendarUnitMinute)
            obj.setDueDateComponents_(cal.components_fromDate_(units, when))
            for alarm in list(obj.alarms() or []):
                obj.removeAlarm_(alarm)
            obj.addAlarm_(EKAlarm.alarmWithAbsoluteDate_(when))
            ok, err = self.store.saveReminder_commit_error_(obj, True, None)
        else:
            for alarm in list(obj.alarms() or []):
                obj.removeAlarm_(alarm)
            obj.addAlarm_(EKAlarm.alarmWithAbsoluteDate_(when))
            ok, err = self.store.saveEvent_span_commit_error_(obj, 0, True, None)

        if not ok:
            log(f"could not snooze {handle}: {err}")
        return bool(ok)


def calendar_color_index(cal) -> int:
    """Map a calendar's colour onto the three the board can draw: 0 terracotta,
    1 green, 2 grey. Anything desaturated becomes grey, anything green-dominant
    becomes green, everything else takes the accent."""
    try:
        components = cal.CGColor().components()
        r, g, b = float(components[0]), float(components[1]), float(components[2])
    except Exception:
        return 0
    hi, lo = max(r, g, b), min(r, g, b)
    if hi - lo < 0.15:
        return 2
    if g >= r and g >= b:
        return 1
    return 0


# ---------------------------------------------------------------- payload

def truncate(title: str) -> str:
    title = " ".join(title.split())
    if len(title) <= TITLE_CHARS:
        return title
    return title[: TITLE_CHARS - 1].rstrip() + "…"


def build_payload(items: list[dict], more: int, quiet: bool) -> dict:
    """Local wall-clock epochs throughout — the firmware does plain division on
    them rather than carrying a timezone database."""
    offset = -time.timezone if time.localtime().tm_isdst == 0 else -time.altzone
    now = time.time()

    payload = {
        "k": "agenda",
        "t": int(now + offset),
        "d": datetime.now().strftime("%a %-d %b"),
        "m": more,
        "e": [],
    }
    if quiet:
        payload["q"] = True

    for item in items:
        entry = {
            "s": int(item["start"] + offset),
            "dur": item["duration"],
            "n": truncate(item["title"]),
            "h": item["handle"],
            "c": item["color"],
        }
        if item["reminder"]:
            entry["r"] = True
        # Alarms already past are not re-rung on the board; the daemon simply
        # stops offering them, which is what "missed" should look like.
        if item["alarm"] and item["alarm"] > now:
            entry["a"] = int(item["alarm"] + offset)
        payload["e"].append(entry)

    return payload


def frame(payload: dict) -> list[bytes]:
    """Split a payload into the board's two-byte-header fragments."""
    body = json.dumps(payload, separators=(",", ":")).encode()
    chunks = [body[i:i + FRAGMENT_BYTES] for i in range(0, len(body), FRAGMENT_BYTES)] or [b""]
    total = len(chunks)
    return [bytes([idx, total]) + chunk for idx, chunk in enumerate(chunks)]


# ---------------------------------------------------------------- BLE

async def resolve_address() -> str:
    if SAVED_ADDR_FILE.exists():
        cached = SAVED_ADDR_FILE.read_text().strip()
        if cached:
            return cached
    log(f"scanning for {DEVICE_NAME}...")
    device = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=15.0)
    if device is None:
        raise RuntimeError(f"{DEVICE_NAME} not found")
    return device.address


async def run_session(address: str, agenda: Agenda, quiet_window) -> None:
    async with BleakClient(address, timeout=CONNECT_TIMEOUT) as client:
        log(f"connected to {address}")

        def on_action(_, data: bytearray):
            try:
                msg = json.loads(data.decode())
            except (UnicodeDecodeError, json.JSONDecodeError):
                log(f"unreadable action from the board: {data!r}")
                return
            handle, action = msg.get("h", ""), msg.get("a", "")
            if action == "done":
                ok = agenda.complete(handle)
            elif action == "snooze":
                ok = agenda.snooze(handle)
            else:
                log(f"unknown action {action!r}")
                return
            log(f"{action} {handle}: {'applied' if ok else 'failed'}")
            agenda.dirty = True     # push the changed list straight back

        await client.start_notify(CMP_CHAR_UUID, on_action)

        last_sweep = 0.0
        while client.is_connected:
            agenda.pump()
            now = time.time()
            if agenda.dirty or now - last_sweep >= POLL_INTERVAL:
                agenda.dirty = False
                last_sweep = now
                items, more = agenda.collect()
                quiet = in_quiet_hours(quiet_window, datetime.now())
                for fragment in frame(build_payload(items, more, quiet)):
                    await client.write_gatt_char(CMP_CHAR_UUID, fragment, response=False)
                    await asyncio.sleep(0.02)   # let the 5 ms firmware loop keep up
                log(f"sent {len(items)} item(s), {more} more{' (quiet)' if quiet else ''}")
            await asyncio.sleep(TICK)


async def main() -> None:
    agenda = Agenda()
    if not agenda.request_access():
        sys.exit("no access to Calendars or Reminders — grant it in "
                 "System Settings → Privacy & Security, then restart the daemon")

    while True:
        quiet_window = parse_quiet_hours(read_config().get("agenda_quiet"))
        try:
            address = await resolve_address()
            await run_session(address, agenda, quiet_window)
            log("link dropped; reconnecting")
        except Exception as e:                  # noqa: BLE001 — a daemon never dies on one bad cycle
            log(f"session ended: {e}")
        await asyncio.sleep(5)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
