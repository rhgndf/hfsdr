#!/usr/bin/env python3
"""
Read I2S ADC text lines from a serial port (default COM3), buffer int16 samples,
and display a live magnitude spectrum (FFT).

Firmware formats matched (either):
  ADC I2S CH1=-123 CH2=456
  CH1:-123,CH2:456

Default sample rate is 96000 Hz (match I2S at 96 ksps). Override with --fs.

The spectrum axis defaults to 0–96000 Hz. For a real-valued signal, |FFT| is only meaningful
up to Nyquist (fs/2 = 48 kHz at 96 ksps); a dashed line marks Nyquist.

Dependencies:
  pip install pyserial numpy matplotlib
"""

from __future__ import annotations

import argparse
import re
import sys
from collections import deque

import numpy as np

try:
    import serial
except ImportError:
    print("Install pyserial: pip install pyserial", file=sys.stderr)
    raise

try:
    import matplotlib.pyplot as plt
    from matplotlib.animation import FuncAnimation
except ImportError:
    print("Install matplotlib: pip install matplotlib", file=sys.stderr)
    raise

# CH1=... CH2=... or CH1:... CH2:...
LINE_RE = re.compile(r"CH1[=:](-?\d+)\D+CH2[=:](-?\d+)", re.IGNORECASE)


def parse_line(line: str) -> tuple[int, int] | None:
    line = line.strip()
    m = LINE_RE.search(line)
    if not m:
        return None
    return int(m.group(1)), int(m.group(2))


def main() -> None:
    p = argparse.ArgumentParser(description="Serial I2S text -> live FFT")
    p.add_argument("--port", default="COM3", help="Serial port (default COM3)")
    p.add_argument("--baud", type=int, default=115200, help="Baud rate")
    p.add_argument(
        "--fs",
        type=float,
        default=96000.0,
        help="Assumed sample rate in Hz (must match firmware I2S rate)",
    )
    p.add_argument(
        "--fmax",
        type=float,
        default=96000.0,
        help="Frequency axis upper limit (Hz); default 96000",
    )
    p.add_argument(
        "--block",
        type=int,
        default=8192,
        help="FFT length (real samples); zero-padded to next power of 2 if needed",
    )
    p.add_argument(
        "--channel",
        choices=("ch1", "ch2"),
        default="ch1",
        help="Which channel to FFT",
    )
    p.add_argument(
        "--interval",
        type=int,
        default=50,
        help="Matplotlib animation interval in ms",
    )
    args = p.parse_args()

    block = max(256, int(args.block))
    # Use power-of-2 FFT for speed
    nfft = 1 << (block - 1).bit_length()
    ch_idx = 0 if args.channel == "ch1" else 1

    buf: deque[int] = deque(maxlen=nfft)

    ser = serial.Serial(
        port=args.port,
        baudrate=args.baud,
        timeout=0.05,
    )
    ser.reset_input_buffer()

    fig, (ax_time, ax_spec) = plt.subplots(2, 1, figsize=(9, 6))
    fig.suptitle(f"I2S FFT — {args.port} @ {args.baud} — {args.channel} — fs={args.fs:g} Hz")

    (line_time,) = ax_time.plot([], [], lw=0.8)
    ax_time.set_xlabel("Sample")
    ax_time.set_ylabel("Amplitude")
    ax_time.set_title("Last frame (time)")
    ax_time.grid(True, alpha=0.3)

    (line_spec,) = ax_spec.plot([], [], lw=0.8)
    ax_spec.set_xlabel("Frequency (Hz)")
    ax_spec.set_ylabel("Magnitude (linear)")
    ax_spec.set_title("|FFT| (Hann window)")
    ax_spec.set_xlim(0, max(args.fmax, 1.0))
    ax_spec.grid(True, alpha=0.3)
    ax_spec.axvline(
        args.fs / 2.0,
        color="orange",
        linestyle="--",
        linewidth=1,
        alpha=0.85,
        label="Nyquist (fs/2)",
    )
    ax_spec.legend(loc="upper right", fontsize=8)

    freqs = np.fft.rfftfreq(nfft, d=1.0 / args.fs)
    win = np.hanning(nfft)

    def update(_frame: int):
        # Read as many complete lines as are available this frame (bounded).
        for _ in range(2048):
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
            buf.append(pair[ch_idx])

        if len(buf) < nfft:
            return line_time, line_spec

        x = np.asarray(buf, dtype=np.float64)
        xw = x * win
        spec = np.fft.rfft(xw)
        mag = np.abs(spec)

        line_time.set_data(np.arange(nfft), x)
        ax_time.relim()
        ax_time.autoscale_view()

        line_spec.set_data(freqs, mag)
        ax_spec.relim()
        ax_spec.autoscale_view()
        ax_spec.set_xlim(0, max(args.fmax, 1.0))

        return line_time, line_spec

    ani = FuncAnimation(
        fig,
        update,
        interval=args.interval,
        blit=False,
        cache_frame_data=False,
    )
    plt.tight_layout()
    plt.show()
    ser.close()


if __name__ == "__main__":
    main()
