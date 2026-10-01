#!/usr/bin/env python3
"""Run a scripted conversation with the ESP32 over its serial port.

Usage: serial_script.py <port> [--no-reset] <step>...

Steps run in order:
  expect:<text>[:<timeout_s>]  wait until <text> appears in the output
                               (default timeout 10 s)
  send:<line>                  write <line> and a newline to the board
  sleep:<seconds>              wait, still echoing what arrives

The board is reset when the port is opened unless --no-reset is given.
Everything received is echoed to stdout. An expect only looks at output that
arrived after the previous expect's match, so the steps match in order; output
that arrives during a send or a sleep is kept for the next expect.

If <text> itself ends in a colon and a number, give the timeout explicitly:
"expect:rate:44100" means text "rate", timeout 44100 s.

Exit status: 0 if every expect matched, 1 on the first timeout (the step is
named on stderr), 2 on a usage error.
"""
import sys
import time

import serial

DEFAULT_TIMEOUT_S = 10.0


def parse_step(arg: str):
    """Turn one command-line step into (kind, value, timeout)."""
    kind, sep, rest = arg.partition(":")
    if not sep or kind not in ("expect", "send", "sleep"):
        raise ValueError(f"unknown step '{arg}'")
    if kind == "send":
        return kind, rest, None
    if kind == "sleep":
        return kind, float(rest), None
    # A trailing ":<number>" is the timeout; anything else is part of the text.
    text, timeout = rest, DEFAULT_TIMEOUT_S
    head, sep, tail = rest.rpartition(":")
    if sep and head:
        try:
            text, timeout = head, float(tail)
        except ValueError:
            pass
    if not text:
        raise ValueError(f"empty text in '{arg}'")
    return kind, text, timeout


def main() -> int:
    args = sys.argv[1:]
    reset = "--no-reset" not in args
    args = [a for a in args if a != "--no-reset"]
    if len(args) < 2 or args[0].startswith("-"):
        print(__doc__, file=sys.stderr)
        return 2
    try:
        steps = [parse_step(a) for a in args[1:]]
    except ValueError as e:
        print(f"serial_script: {e}\n\n{__doc__}", file=sys.stderr)
        return 2

    ser = serial.Serial(args[0], 115200, timeout=0.1)
    if reset:
        # The board's auto-reset circuit wires RTS to EN and DTR to GPIO0.
        # Keep DTR released so GPIO0 stays high (normal boot), then pulse EN low.
        ser.dtr = False
        ser.rts = True
        time.sleep(0.1)
        ser.rts = False
    # Without a reset the lines stay as the open left them (both asserted),
    # which the circuit treats as idle. Releasing them one at a time would pass
    # through "RTS only", and that is a reset.

    pending = b""  # received, not yet consumed by an expect

    def pump(seconds: float, want: bytes = b"") -> bool:
        """Read and echo for up to `seconds`; stop early once `want` is seen."""
        nonlocal pending
        deadline = time.monotonic() + seconds
        while True:
            if want:
                at = pending.find(want)
                if at >= 0:
                    pending = pending[at + len(want):]
                    return True
            if time.monotonic() >= deadline:
                return False
            data = ser.read(4096)
            if data:
                sys.stdout.write(data.decode("utf-8", "replace"))
                sys.stdout.flush()
                pending += data

    for number, (kind, value, timeout) in enumerate(steps, start=1):
        if kind == "send":
            ser.write(value.encode() + b"\n")
            ser.flush()
        elif kind == "sleep":
            pump(value)
        elif not pump(timeout, value.encode()):
            sys.stdout.write("\n")
            sys.stdout.flush()
            print(f"serial_script: TIMEOUT at step {number} ({args[number]}): "
                  f"'{value}' not seen within {timeout:g} s", file=sys.stderr)
            return 1
    pump(0.3)  # let the rest of the last line arrive
    return 0


if __name__ == "__main__":
    sys.exit(main())
