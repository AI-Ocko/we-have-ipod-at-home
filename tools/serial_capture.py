#!/usr/bin/env python3
"""Reset the ESP32 and print its serial output until a marker line or timeout.

Usage: serial_capture.py <port> <timeout_seconds> [marker]
Exit status: 0 if the marker was seen (or no marker was given), 1 on timeout.
"""
import sys
import time

import serial


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    port = sys.argv[1]
    timeout = float(sys.argv[2])
    marker = sys.argv[3].encode() if len(sys.argv) > 3 else None

    ser = serial.Serial(port, 115200, timeout=0.2)
    # The board's auto-reset circuit wires RTS to EN and DTR to GPIO0.
    # Keep DTR released so GPIO0 stays high (normal boot), then pulse EN low.
    ser.dtr = False
    ser.rts = True
    time.sleep(0.1)
    ser.rts = False

    deadline = time.time() + timeout
    seen = b""
    while time.time() < deadline:
        data = ser.read(4096)
        if not data:
            continue
        sys.stdout.write(data.decode("utf-8", "replace"))
        sys.stdout.flush()
        if marker:
            seen = (seen + data)[-65536:]
            if marker in seen:
                time.sleep(0.3)  # let the rest of the line arrive
                sys.stdout.write(ser.read(4096).decode("utf-8", "replace"))
                sys.stdout.flush()
                return 0
    return 1 if marker else 0


if __name__ == "__main__":
    sys.exit(main())
