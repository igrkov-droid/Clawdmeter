#!/bin/bash
# Take a screenshot from the Waveshare AMOLED display via LVGL snapshot.
# Usage: ./screenshot.sh [output.png] [port]
# Default port: /dev/cu.usbmodem101 on macOS, /dev/ttyACM0 on Linux.

OUTPUT="${1:-screenshot.png}"
if [ -z "$2" ]; then
    case "$(uname -s)" in
        Darwin) PORT="/dev/cu.usbmodem101" ;;
        *)      PORT="/dev/ttyACM0" ;;
    esac
else
    PORT="$2"
fi

# Use pio's bundled python if pyserial isn't on the system python.
PY="python3"
if ! python3 -c "import serial" 2>/dev/null; then
    if [ -x "$HOME/.platformio/penv/bin/python" ]; then
        PY="$HOME/.platformio/penv/bin/python"
    fi
fi

echo "Taking screenshot from $PORT..."

"$PY" - "$PORT" "$OUTPUT" << 'PYEOF'
import serial, sys, struct, zlib

port_path, out_path = sys.argv[1], sys.argv[2]

port = serial.Serial(port_path, 115200, timeout=10)
port.reset_input_buffer()
port.write(b"screenshot\n")
port.flush()

while True:
    line = port.readline().decode("utf-8", errors="replace").strip()
    if line.startswith("SCREENSHOT_START"):
        parts = line.split()
        w, h, raw_size = int(parts[1]), int(parts[2]), int(parts[3])
        break
    if line == "SCREENSHOT_ERR":
        print("Device reported screenshot error", file=sys.stderr)
        sys.exit(1)
    if line == "SCREENSHOT_UNSUPPORTED":
        print("Board has no PSRAM; snapshot capture is unavailable", file=sys.stderr)
        sys.exit(1)

data = b""
while len(data) < raw_size:
    chunk = port.read(min(4096, raw_size - len(data)))
    if not chunk:
        print(f"Timeout: got {len(data)} of {raw_size} bytes", file=sys.stderr)
        sys.exit(1)
    data += chunk

for _ in range(10):
    line = port.readline().decode("utf-8", errors="replace").strip()
    if line == "SCREENSHOT_END":
        break

port.close()

# RGB565LE -> PNG with the standard library, so the script needs no ffmpeg or
# Pillow (neither ships on a stock macOS).
rows = bytearray()
for y in range(h):
    rows.append(0)                                  # PNG per-scanline filter: none
    row = data[y * w * 2:(y + 1) * w * 2]
    for px in struct.unpack(f"<{w}H", row):
        r = (px >> 11) & 0x1F
        g = (px >> 5) & 0x3F
        b = px & 0x1F
        rows += bytes(((r * 255 + 15) // 31, (g * 255 + 31) // 63, (b * 255 + 15) // 31))

def chunk(tag, payload):
    return (struct.pack(">I", len(payload)) + tag + payload
            + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

with open(out_path, "wb") as f:
    f.write(b"\x89PNG\r\n\x1a\n")
    f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
    f.write(chunk(b"IDAT", zlib.compress(bytes(rows), 6)))
    f.write(chunk(b"IEND", b""))

print(f"Captured {w}x{h} ({len(data)} bytes)")
PYEOF

if [ $? -ne 0 ]; then
    echo "Screenshot capture failed"
    exit 1
fi

echo "Saved: $OUTPUT"
