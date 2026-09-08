#!/usr/bin/env python3
"""Tests for the companion daemon's pure payload logic.

EventKit and CoreBluetooth only exist on macOS, so those modules are stubbed
before import: everything covered here is plain data shaping — framing, title
truncation, quiet hours — which is exactly the part that has to agree with the
firmware byte for byte.

Run: python -m pytest daemon/tests/test_companion_payload.py -x -q
"""
import json
import sys
import types
from datetime import datetime
from unittest.mock import MagicMock


def _install_macos_stubs():
    """Stand in for the frameworks the module imports at load time."""
    for name in ("objc", "Foundation", "EventKit", "bleak"):
        if name not in sys.modules:
            sys.modules[name] = types.ModuleType(name)
    sys.modules["objc"].super = super
    for attr in ("NSDate", "NSCalendar", "NSNotificationCenter", "NSObject",
                 "NSRunLoop", "NSDefaultRunLoopMode", "NSCalendarUnitYear",
                 "NSCalendarUnitMonth", "NSCalendarUnitDay", "NSCalendarUnitHour",
                 "NSCalendarUnitMinute"):
        setattr(sys.modules["Foundation"], attr, MagicMock())
    # NSObject must be subclassable for the notification observer.
    sys.modules["Foundation"].NSObject = type("NSObject", (), {})
    for attr in ("EKEventStore", "EKAlarm", "EKEntityTypeEvent", "EKEntityTypeReminder"):
        setattr(sys.modules["EventKit"], attr, MagicMock())
    sys.modules["bleak"].BleakClient = MagicMock()
    sys.modules["bleak"].BleakScanner = MagicMock()


_install_macos_stubs()

from daemon.clawdmeter_companion import (  # noqa: E402
    FRAGMENT_BYTES,
    MAX_ITEMS,
    TITLE_CHARS,
    build_payload,
    frame,
    in_quiet_hours,
    parse_quiet_hours,
    truncate,
)


# --- framing: has to match the reassembler in firmware/src/ble.cpp -----------

def reassemble(fragments):
    """What CmpCallbacks::onWrite does: check the header, concatenate the rest."""
    body, expected = b"", 0
    for f in fragments:
        idx, total = f[0], f[1]
        assert idx == expected, f"fragment {idx} arrived out of order"
        assert idx < total
        body += f[2:]
        expected += 1
    assert expected == fragments[0][1], "sequence ended before the last fragment"
    return body


def test_single_fragment_round_trips():
    payload = {"k": "agenda", "t": 1, "e": []}
    fragments = frame(payload)
    assert len(fragments) == 1
    assert fragments[0][:2] == bytes([0, 1])
    assert json.loads(reassemble(fragments)) == payload


def test_long_payload_splits_and_reassembles():
    """A full four-item agenda outgrows one ATT write; the pieces must still
    rebuild into exactly the same JSON."""
    items = [{
        "start": 1_700_000_000 + i * 3600,
        "duration": 45,
        "title": f"Event number {i} with a long name",
        "handle": f"e{i}",
        "color": i % 3,
        "reminder": False,
        "alarm": None,
    } for i in range(MAX_ITEMS)]
    payload = build_payload(items, more=2, quiet=False)

    fragments = frame(payload)
    assert len(fragments) > 1, "this payload is supposed to exercise splitting"
    assert all(len(f) <= FRAGMENT_BYTES + 2 for f in fragments)
    assert json.loads(reassemble(fragments)) == payload


def test_every_fragment_carries_the_same_total():
    payload = build_payload([{
        "start": 1_700_000_000, "duration": 30, "title": "x" * 20,
        "handle": "e0", "color": 0, "reminder": True, "alarm": None,
    }] * MAX_ITEMS, more=0, quiet=False)
    fragments = frame(payload)
    assert len({f[1] for f in fragments}) == 1


# --- payload shaping --------------------------------------------------------

def test_reminder_and_alarm_fields_are_omitted_when_they_do_not_apply():
    """Absent keys are how the firmware's `| default` reads "not set", so an
    event must not carry a false `r` or a zero `a` — they cost bytes we need."""
    payload = build_payload([{
        "start": 1_700_000_000, "duration": 60, "title": "Standup",
        "handle": "e0", "color": 1, "reminder": False, "alarm": None,
    }], more=0, quiet=False)
    entry = payload["e"][0]
    assert "r" not in entry
    assert "a" not in entry
    assert "q" not in payload


def test_past_alarms_are_dropped():
    """A missed alarm is simply not offered — the board must never ring for
    something that came due while it was asleep."""
    payload = build_payload([{
        "start": 1_700_000_000, "duration": 0, "title": "Old",
        "handle": "e0", "color": 0, "reminder": True, "alarm": 1_700_000_000,
    }], more=0, quiet=False)
    assert "a" not in payload["e"][0]


def test_quiet_flag_rides_on_the_payload():
    payload = build_payload([], more=0, quiet=True)
    assert payload["q"] is True


# --- titles -----------------------------------------------------------------

def test_short_titles_pass_through():
    assert truncate("Sync with Lisa") == "Sync with Lisa"


def test_long_titles_are_cut_to_what_a_row_shows():
    out = truncate("A remarkably long meeting title that will never fit")
    assert len(out) == TITLE_CHARS
    assert out.endswith("…")


def test_whitespace_is_collapsed():
    assert truncate("  two\n\tspaces  ") == "two spaces"


# --- quiet hours ------------------------------------------------------------

def test_parse_quiet_hours():
    assert parse_quiet_hours("22:00-08:00") == (1320, 480)
    assert parse_quiet_hours(" 9:30 - 17:00 ") == (570, 1020)
    assert parse_quiet_hours("nonsense") is None
    assert parse_quiet_hours(None) is None


def test_window_across_midnight_covers_both_sides():
    window = parse_quiet_hours("22:00-08:00")
    assert in_quiet_hours(window, datetime(2026, 9, 8, 23, 30))
    assert in_quiet_hours(window, datetime(2026, 9, 8, 3, 0))
    assert not in_quiet_hours(window, datetime(2026, 9, 8, 12, 0))


def test_window_inside_one_day():
    window = parse_quiet_hours("09:00-17:00")
    assert in_quiet_hours(window, datetime(2026, 9, 8, 12, 0))
    assert not in_quiet_hours(window, datetime(2026, 9, 8, 22, 0))


def test_no_window_never_silences():
    assert not in_quiet_hours(None, datetime(2026, 9, 8, 3, 0))
