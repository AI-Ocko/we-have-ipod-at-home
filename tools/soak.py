#!/usr/bin/env python3
"""A long run of continuous play, driven through the debug console.

Usage: soak.py <port> <minutes> <log file> [--no-reset] [--no-setup]
               [--keep-playlists] [--every=<seconds between actions>]

Resets the board (unless --no-reset), makes two playlists with the console's
`pl new` (SOAK: every format on the card, and SOAKEDIT: one to edit), plays
SOAK through the keys (Playlists, `#`, Play), turns loop all on and opens
About at once (the first free-space read after a boot). Then, until <minutes>
have passed since the first track started, it does one action every
ACTION_EVERY_S seconds, in turn: volume up and down, pause and resume,
fast-forward and rewind by holding 6 and 4, a console seek, skips with 6 and
4, adding an artist's tracks to the queue with `#`, editing SOAKEDIT with the
`pl` commands, opening About, and turning shuffle on (at a third of the run)
and off (at two thirds). The backlight is left to time out; before each
action that uses keys the board is asked (`stat`) whether it is lit, and if
it is not a key is pressed to wake it, which must do nothing else.
A `stat` is taken every STAT_EVERY_S seconds as well.

At the end: a last `stat` and `queue`, the playlists deleted (unless
--keep-playlists), `stop`, and `state clear` as the last command.

Every line from the board goes to <log file>, prefixed with the seconds since
the script started; the script's own notes are lines starting with `#`.
--no-setup skips everything before the actions (the board must already be
playing a looping queue).

Exit status: 0 if the run reached its end, 1 if the board stopped answering.
"""
import re
import sys
import threading
import time

import serial

ACTION_EVERY_S = 150
STAT_EVERY_S = 60

# The queue, by path as the index stores it: both music tracks, every audio
# file in /test/ (t07 and t08 are not audio and fail, t09 is cut short) and
# some of /lib/'s very short tracks, so that tracks change often.
SOAK_PATHS = [
    "music/Good Time.mp3",
    "test/t01_44k_16_st.flac",
    "test/t04_44k_cbr192_st.mp3",
    "lib/Bjork/Homogenic/01 Hunter.flac",
    "test/t02_96k_24_st.flac",
    "lib/Radiohead/OK Computer/01 Airbag.mp3",
    "test/t03_48k_16_mono.flac",
    "test/t07_empty.flac",
    "test/t05_48k_vbr_mono.mp3",
    "lib/Various/Hits/01 a.flac",
    "music/16 - Kanye West - Hey Mama.flac",
    "test/t06_tone_30s.flac",
    "test/t08_not_audio.flac",
    "lib/Queen/Hits/01 Bohemian.mp3",
    "test/t09_truncated.flac",
    "lib/Radiohead/Kid A/01 Everything.flac",
    "test/t10_loud_quiet_44k_16_st.flac",
    "lib/Radiohead/OK Computer/02 Paranoid.mp3",
]
# Artists whose tracks `#` > Add to queue appends, in turn.
ADD_ARTISTS = ["Bjork", "Queen", "Radiohead"]


class Board:
    def __init__(self, port, log_path, reset):
        self.ser = serial.Serial(port, 115200, timeout=0.1)
        if reset:
            self.ser.dtr = False
            self.ser.rts = True
            time.sleep(0.1)
            self.ser.rts = False
        self.t0 = time.monotonic()
        self.log = open(log_path, "a", buffering=1)
        self.lines = []  # every complete line received
        self.cond = threading.Condition()
        self.alive = True
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def now(self):
        return time.monotonic() - self.t0

    def note(self, text):
        self.log.write(f"{self.now():9.2f} # {text}\n")
        print(f"# {text}", flush=True)

    def _read(self):
        partial = b""
        while self.alive:
            try:
                data = self.ser.read(4096)
            except serial.SerialException as e:
                self.note(f"serial error: {e}")
                self.alive = False
                with self.cond:
                    self.cond.notify_all()
                return
            if not data:
                continue
            partial += data
            *done, partial = partial.split(b"\n")
            if done:
                with self.cond:
                    for raw in done:
                        line = raw.decode("utf-8", "replace").rstrip("\r")
                        self.log.write(f"{self.now():9.2f} {line}\n")
                        self.lines.append(line)
                    self.cond.notify_all()

    def mark(self):
        with self.cond:
            return len(self.lines)

    def wait_for(self, pattern, timeout, since=None):
        """The first line from `since` on matching the regex, or None."""
        rx = re.compile(pattern)
        deadline = time.monotonic() + timeout
        at = self.mark() if since is None else since
        with self.cond:
            while True:
                while at < len(self.lines):
                    m = rx.search(self.lines[at])
                    at += 1
                    if m:
                        return m
                left = deadline - time.monotonic()
                if left <= 0 or not self.alive:
                    return None
                self.cond.wait(left)

    def send(self, line):
        self.log.write(f"{self.now():9.2f} > {line}\n")
        self.ser.write(line.encode() + b"\n")
        self.ser.flush()

    def cmd(self, line, timeout=20):
        """Send a console command; return its answer line (ok or err)."""
        since = self.mark()
        self.send(line)
        word = re.escape(line.split()[0])
        m = self.wait_for(rf"dbg: (ok|err) {word}", timeout, since)
        if m is None:
            raise TimeoutError(f"no answer to '{line}'")
        return m.string

    def collect(self, line, pattern, timeout=20):
        """Send a command; return the lines matching `pattern` before its
        answer, and the answer."""
        since = self.mark()
        answer = self.cmd(line, timeout)
        with self.cond:
            got = [l for l in self.lines[since:] if re.search(pattern, l)]
        return got, answer

    def screen(self):
        got, _ = self.collect("screen", r'dbg: screen "')
        m = re.search(r'screen "(.{16})" "(.{16})"', got[-1])
        return m.group(1), m.group(2)

    def stat(self):
        got, _ = self.collect("stat", r"dbg: stat ")
        return dict(re.findall(r"(\w+)=(\S+)", got[-1].split("dbg: stat ")[1]))

    def key(self, k, settle=0.15):
        self.cmd(f"key {k}")
        time.sleep(settle)


def is_menu(top, bottom):
    return re.match(r">\S", bottom) is not None


def wake(b):
    if b.stat().get("ui_light") == "off":
        since = b.mark()
        b.key("0")
        if b.wait_for(r"woke the backlight and does nothing else", 3, since):
            b.note("backlight was off: woken by key 0")
        else:
            b.note("WARNING: key 0 did not report waking the backlight")


def go_main(b):
    for _ in range(12):
        top, bottom = b.screen()
        if top.startswith("Main"):
            return
        if top.startswith("Free") or "tracks" in top and not is_menu(top, bottom):
            b.key("0")  # About: any key leaves it, to Settings
        elif is_menu(top, bottom):
            b.key("4")
        else:
            b.key("C")
    raise RuntimeError("cannot reach the main menu")


def select_row(b, name, limit=40):
    for _ in range(limit):
        top, bottom = b.screen()
        if bottom[1:].strip() == name:
            return
        b.key("8")
    raise RuntimeError(f"row '{name}' not found")


def go_now_playing(b):
    top, bottom = b.screen()
    if not is_menu(top, bottom):
        if top.startswith("Free") or re.match(r"\d+ tracks", top):
            b.key("0")
        else:
            return
    b.key("C")
    top, bottom = b.screen()
    if is_menu(top, bottom):
        raise RuntimeError("C did not bring up Now Playing")


def open_about(b):
    wake(b)
    go_main(b)
    select_row(b, "Settings")
    b.key("5")
    select_row(b, "About")
    b.key("5", settle=2.5)
    top, bottom = b.screen()
    b.note(f"About: '{top}' '{bottom}'")
    b.key("0")  # leaves to Settings
    go_now_playing(b)


def pl_number(b, name):
    got, _ = b.collect("pl ls", r'dbg: pl \d+ "')
    for line in got:
        m = re.search(r'pl (\d+) "([^"]*)"', line)
        if m and m.group(2) == name:
            return int(m.group(1))
    return None


def track_numbers(b, paths):
    out = []
    for p in paths:
        got, ans = b.collect(f"lib find {p}", r"dbg: track \d+")
        if not got:
            raise RuntimeError(f"not in the library: {p} ({ans})")
        out.append(int(re.search(r"track (\d+)", got[-1]).group(1)))
    return out


def setup(b):
    if not b.wait_for(r"library: loaded \d+ tracks", 90, since=0):
        raise RuntimeError("the library did not load")
    time.sleep(1)
    for name in ("SOAK", "SOAKEDIT"):
        n = pl_number(b, name)
        if n is not None:
            b.cmd(f"pl del {n}")
    soak = track_numbers(b, SOAK_PATHS)
    b.note(f"SOAK tracks: {soak}")
    b.note(b.cmd("pl new SOAK " + " ".join(map(str, soak))))
    b.note(b.cmd("pl new SOAKEDIT " + str(soak[3])))
    # Play SOAK with the keys: Playlists, the playlist's row, `#`, Play.
    wake(b)
    go_main(b)
    select_row(b, "Playlists")
    b.key("5", settle=1.0)
    select_row(b, "SOAK")
    b.key("#")
    top, bottom = b.screen()
    if not bottom.startswith(">Play"):
        raise RuntimeError(f"options menu not as expected: '{top}' '{bottom}'")
    since = b.mark()
    b.key("5")
    if not b.wait_for(r"player: STARTED", 10, since):
        raise RuntimeError("SOAK did not start")
    go_now_playing(b)
    b.key("3")  # loop: off -> all
    got, _ = b.collect("queue 0 1", r"dbg: queue count=")
    b.note(got[-1])
    if "loop=1" not in got[-1]:
        raise RuntimeError("loop all is not on")
    # About while playing, right after the boot: the first free-space read.
    open_about(b)


def act_volume(b):
    wake(b)
    go_now_playing(b)
    for k in "AABB":
        b.key(k, 0.4)


def act_pause(b):
    wake(b)
    go_now_playing(b)
    b.key("D")
    time.sleep(5)
    b.key("D")


def act_scrub(b):
    wake(b)
    go_now_playing(b)
    b.cmd("hold 6 12")
    time.sleep(3)
    b.cmd("hold 4 6")


def act_console_seek(b):
    b.cmd("seek 8000")
    time.sleep(3)
    b.cmd("seek -4000")


def act_skip(b):
    wake(b)
    go_now_playing(b)
    b.key("6", 3)
    b.key("6", 3)
    b.key("4", 1)


add_turn = [0]


def act_add(b):
    artist = ADD_ARTISTS[add_turn[0] % len(ADD_ARTISTS)]
    add_turn[0] += 1
    wake(b)
    go_main(b)
    select_row(b, "Artists")
    b.key("5")
    select_row(b, artist)
    b.key("#")
    select_row(b, "Add to queue", limit=5)
    b.key("5", 0.5)
    top, bottom = b.screen()
    b.note(f"add {artist}: '{top}' '{bottom}'")
    go_now_playing(b)


edit_turn = [0]


def act_playlist(b):
    n = pl_number(b, "SOAKEDIT")
    if n is None:
        b.note("WARNING: SOAKEDIT is gone")
        return
    t = edit_turn[0]
    edit_turn[0] += 1
    b.note(b.cmd(f"pl add {n} {1000 + t % 3} {1013 + t % 10}"))
    b.note(b.cmd(f"pl up {n} 1"))
    if t % 2:
        b.note(b.cmd(f"pl rm {n} 0"))
    b.note(b.cmd(f"pl mv {n} SOAKEDIT"))


ACTIONS = [act_volume, act_pause, act_scrub, act_skip, act_add,
           act_console_seek, act_playlist, open_about]


def run(b, minutes, setup_wanted):
    if setup_wanted:
        setup(b)
    start = b.now()
    b.note(f"run starts; {minutes} minutes")
    end = start + minutes * 60
    next_action = start + ACTION_EVERY_S
    next_stat = start + STAT_EVERY_S
    turn = 0
    shuffle_on = False
    while b.now() < end:
        if not b.alive:
            return 1
        t = b.now()
        if t >= next_stat:
            next_stat += STAT_EVERY_S
            b.stat()
        if t >= next_action:
            next_action += ACTION_EVERY_S
            elapsed = t - start
            if not shuffle_on and elapsed >= minutes * 20 and elapsed < minutes * 40:
                wake(b)
                go_now_playing(b)
                b.key("1")
                shuffle_on = True
                b.note("shuffle on")
            elif shuffle_on and elapsed >= minutes * 40:
                wake(b)
                go_now_playing(b)
                b.key("1")
                shuffle_on = False
                b.note("shuffle off")
            action = ACTIONS[turn % len(ACTIONS)]
            turn += 1
            b.note(f"action {action.__name__} at {elapsed / 60:.1f} min")
            try:
                action(b)
            except (RuntimeError, TimeoutError) as e:
                b.note(f"WARNING: {action.__name__} failed: {e}")
        time.sleep(0.5)
    b.note(f"run ends after {(b.now() - start) / 60:.1f} minutes")
    b.stat()
    got, _ = b.collect("queue 0 1", r"dbg: queue count=")
    b.note(got[-1] if got else "no queue line")
    return 0


def finish(b, keep):
    if not keep:
        for name in ("SOAKEDIT", "SOAK"):
            n = pl_number(b, name)
            if n is not None:
                b.note(b.cmd(f"pl del {n}"))
    b.note(b.cmd("stop"))
    time.sleep(3)
    b.note(b.cmd("state clear"))


def main():
    args = sys.argv[1:]
    global ACTION_EVERY_S
    flags = {a for a in args if a.startswith("--")}
    for f in flags:
        if f.startswith("--every="):
            ACTION_EVERY_S = float(f.split("=", 1)[1])
    args = [a for a in args if not a.startswith("--")]
    if len(args) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    b = Board(args[0], args[2], reset="--no-reset" not in flags)
    status = run(b, float(args[1]), "--no-setup" not in flags)
    if status == 0:
        finish(b, "--keep-playlists" in flags)
    time.sleep(1)
    b.alive = False
    return status


if __name__ == "__main__":
    sys.exit(main())
