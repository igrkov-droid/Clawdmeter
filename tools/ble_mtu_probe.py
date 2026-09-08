#!/usr/bin/env python3
"""Measure the largest JSON payload the Clawdmeter accepts in one GATT write.

The shipped daemon writes without response (claude_usage_daemon.py), which is
hard-capped at MTU-3 — about 182 bytes on macOS, since CoreBluetooth offers an
MTU of 185 and the negotiated value is the minimum of the two. The firmware's
receive buffer is 512 bytes (BLE_BUF_SIZE in firmware/src/ble.cpp), so the only
question is whether a write *with* response reaches it via ATT long writes.

The probe writes valid JSON of increasing length and watches the TX
characteristic: the firmware notifies {"ack":true} when the payload parsed and
{"err":true} when it did not (ble_send_ack / ble_send_nack). A payload cut off
at the MTU always breaks inside a string literal, so the size where ack turns
into err is the real ceiling.

Stop the usage daemon first — its own writes every 60 s would produce acks the
probe would credit to itself.

Usage: python3 tools/ble_mtu_probe.py [address]
"""

import asyncio
import json
import sys
from pathlib import Path

from bleak import BleakClient, BleakScanner

DEVICE_NAME = "Clawdmeter"
RX_CHAR_UUID = "4c41555a-4465-7669-6365-000000000002"
TX_CHAR_UUID = "4c41555a-4465-7669-6365-000000000003"
SAVED_ADDR_FILE = Path.home() / ".config" / "claude-usage-monitor" / "ble-address"

SIZES = [120, 160, 182, 200, 240, 300, 400, 500]
REPLY_TIMEOUT = 4.0


def build_payload(size: int) -> bytes:
    """A valid usage payload padded to exactly `size` bytes.

    The padding sits in a string that is not the last key, so any truncation
    lands inside a quoted value and the JSON fails to parse.
    """
    def render(pad: str) -> bytes:
        body = {"s": 0.0, "sr": -1, "w": 0.0, "wr": -1,
                "st": "probe", "ok": False, "pad": pad, "z": 1}
        return json.dumps(body, separators=(",", ":")).encode()

    overhead = len(render(""))
    if size < overhead:
        raise ValueError(f"{size} bytes is below the {overhead}-byte envelope")
    return render("x" * (size - overhead))


async def resolve_address(argv: list[str]) -> str:
    if len(argv) > 1:
        return argv[1]
    if SAVED_ADDR_FILE.exists():
        cached = SAVED_ADDR_FILE.read_text().strip()
        if cached:
            print(f"using cached address {cached}")
            return cached
    print(f"scanning for {DEVICE_NAME}...")
    device = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=15.0)
    if device is None:
        sys.exit(f"{DEVICE_NAME} not found — is it advertising?")
    return device.address


async def probe(client: BleakClient, size: int, with_response: bool) -> str:
    reply: asyncio.Queue[str] = asyncio.Queue()

    def on_notify(_, data: bytearray) -> None:
        reply.put_nowait(data.decode(errors="replace"))

    await client.start_notify(TX_CHAR_UUID, on_notify)
    try:
        await client.write_gatt_char(RX_CHAR_UUID, build_payload(size),
                                     response=with_response)
    except Exception as e:                     # noqa: BLE001 — report, don't crash
        await client.stop_notify(TX_CHAR_UUID)
        return f"write failed: {e}"
    try:
        answer = await asyncio.wait_for(reply.get(), REPLY_TIMEOUT)
    except asyncio.TimeoutError:
        answer = "no reply"
    finally:
        await client.stop_notify(TX_CHAR_UUID)
    return answer


async def main() -> None:
    address = await resolve_address(sys.argv)
    async with BleakClient(address, timeout=20.0) as client:
        print(f"connected to {address}\n")
        for with_response in (True, False):
            label = "with response" if with_response else "without response"
            print(f"--- write {label} ---")
            for size in SIZES:
                answer = await probe(client, size, with_response)
                verdict = {'{"ack":true}': "ok",
                           '{"err":true}': "TRUNCATED"}.get(answer, answer)
                print(f"  {size:4d} B  {verdict}")
                await asyncio.sleep(0.4)   # let the 5 ms firmware loop drain rx_buf
            print()


if __name__ == "__main__":
    asyncio.run(main())
