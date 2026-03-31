#!/usr/bin/env python3
"""Shared line parser and serial drain for real-time plotting scripts."""

import re
import time
from datetime import datetime

LINE_RE = re.compile(r"CH1[=:](-?\d+)\D+CH2[=:](-?\d+)", re.IGNORECASE)


def parse_line(line):
    line = line.strip()
    m = LINE_RE.search(line)
    if not m:
        return None
    return int(m.group(1)), int(m.group(2))


def drain_serial_lines(ser, max_lines, buf, transform):
    """
    Read up to max_lines complete lines from ser; append transform(ch1, ch2) to buf.
    """
    for _ in range(max_lines):
        raw = ser.readline()
        if not raw:
            break
        try:
            text = raw.decode("utf-8", errors="replace")
        except Exception:
            continue
        pair = parse_line(text)
        if pair is None:
            continue
        ch1, ch2 = pair
        buf.append(transform(ch1, ch2))


def make_rate_limited_pair_printer(print_hz, label):
    """
    Return (note_pair, tick_print) callables.
    note_pair(ch1, ch2) records the latest parsed pair from the serial stream.
    tick_print() prints at most print_hz lines per second to stdout (no-op if print_hz <= 0).
    Each line starts with local wall time: YYYY-MM-DD HH:MM:SS.mmm
    """
    state = {"t0": 0.0, "c1": None, "c2": None}

    def note_pair(ch1, ch2):
        state["c1"] = ch1
        state["c2"] = ch2

    def tick_print():
        hz = float(print_hz)
        if hz <= 0:
            return
        if state["c1"] is None:
            return
        now = time.monotonic()
        if now - state["t0"] < 1.0 / hz:
            return
        state["t0"] = now
        stamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
        print(
            "%s %s CH1=%d CH2=%d"
            % (stamp, label, int(state["c1"]), int(state["c2"])),
            flush=True,
        )

    return note_pair, tick_print
