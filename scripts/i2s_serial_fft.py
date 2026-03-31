#!/usr/bin/env python3
"""
Read I2S ADC text lines from a serial port (default COM3), buffer int16 samples,
and display a live magnitude spectrum (FFT).

Firmware formats matched (either):
  ADC I2S CH1=-123 CH2=456
  CH1:-123,CH2:456

Default sample rate is 96000 Hz. Override with --fs.

Console: prints latest CH1/CH2 at --print-hz (default 2/s); use --print-hz 0 to disable.

Plotting backends:
  matplotlib  — default; FuncAnimation (~30–60 FPS with --interval 16)
  pyqtgraph   — faster real-time (QTimer + GPU-friendly curves); pip install pyqtgraph PyQt5

Dependencies:
  pip install pyserial numpy matplotlib
  pip install pyqtgraph PyQt5   # optional, for --backend pyqtgraph
"""

import argparse
import os
import sys
from collections import deque

import numpy as np

_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
if _SCRIPT_DIR not in sys.path:
    sys.path.insert(0, _SCRIPT_DIR)
from serial_plot_rt import drain_serial_lines, make_rate_limited_pair_printer

try:
    import serial
except ImportError:
    print("Install pyserial: pip install pyserial", file=sys.stderr)
    raise


def run_matplotlib(args, nfft, ch_idx, freqs, hann):
    import matplotlib.pyplot as plt
    from matplotlib.animation import FuncAnimation

    buf = deque(maxlen=nfft)
    ser = serial.Serial(
        port=args.port,
        baudrate=args.baud,
        timeout=0.05,
    )
    ser.reset_input_buffer()

    note_pair, tick_print = make_rate_limited_pair_printer(
        args.print_hz, "[%s]" % (args.port,)
    )

    def transform(ch1, ch2):
        note_pair(ch1, ch2)
        return ch1 if ch_idx == 0 else ch2

    fig, (ax_time, ax_spec) = plt.subplots(2, 1, figsize=(9, 6))
    fig.suptitle(
        f"I2S FFT — {args.port} @ {args.baud} — ch{ch_idx + 1} — fs={args.fs:g} Hz (matplotlib)"
    )

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

    def update(_frame):
        drain_serial_lines(ser, 2048, buf, transform)
        tick_print()
        if len(buf) < nfft:
            return line_time, line_spec

        x = np.asarray(buf, dtype=np.float64)
        xw = x * hann
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

    FuncAnimation(
        fig,
        update,
        interval=args.interval,
        blit=False,
        cache_frame_data=False,
    )
    plt.tight_layout()
    plt.show()
    ser.close()


def run_pyqtgraph(args, nfft, ch_idx, freqs, hann):
    import pyqtgraph as pg
    from pyqtgraph.Qt import QtCore

    buf = deque(maxlen=nfft)
    ser = serial.Serial(
        port=args.port,
        baudrate=args.baud,
        timeout=0.05,
    )
    ser.reset_input_buffer()

    note_pair, tick_print = make_rate_limited_pair_printer(
        args.print_hz, "[%s]" % (args.port,)
    )

    def transform(ch1, ch2):
        note_pair(ch1, ch2)
        return ch1 if ch_idx == 0 else ch2

    pg.setConfigOptions(antialias=True)
    app = pg.mkQApp("I2S FFT")

    glw = pg.GraphicsLayoutWidget(show=True)
    glw.resize(900, 650)
    glw.setWindowTitle(f"I2S FFT — {args.port} — fs={args.fs:g} Hz (pyqtgraph)")

    p_time = glw.addPlot(row=0, col=0, title="Last frame (time)")
    p_time.setLabel("left", "Amplitude")
    p_time.setLabel("bottom", "Sample")
    p_time.showGrid(x=True, y=True, alpha=0.3)

    p_spec = glw.addPlot(row=1, col=0, title="|FFT| (Hann)")
    p_spec.setLabel("left", "Magnitude")
    p_spec.setLabel("bottom", "Frequency (Hz)")
    p_spec.showGrid(x=True, y=True, alpha=0.3)
    p_spec.setXRange(0, max(args.fmax, 1.0), padding=0)

    curve_t = p_time.plot(pen=pg.mkPen("c", width=1))
    curve_s = p_spec.plot(pen=pg.mkPen("y", width=1))
    p_spec.addItem(
        pg.InfiniteLine(
            pos=args.fs / 2.0,
            angle=90,
            pen=pg.mkPen("orange", width=1.5, dash=[6, 4]),
        )
    )

    def update():
        drain_serial_lines(ser, 2048, buf, transform)
        tick_print()
        if len(buf) < nfft:
            return
        x = np.asarray(buf, dtype=np.float64)
        xw = x * hann
        mag = np.abs(np.fft.rfft(xw))
        curve_t.setData(np.arange(nfft), x)
        curve_s.setData(freqs, mag)

    timer = QtCore.QTimer()
    timer.timeout.connect(update)
    timer.start(args.interval)

    try:
        pg.exec()
    except AttributeError:
        from pyqtgraph.Qt import QtWidgets

        QtWidgets.QApplication.exec_()
    ser.close()


def main():
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
        default=16,
        help="Redraw period in ms (~16 ms ≈ 60 FPS; matplotlib or pyqtgraph timer)",
    )
    p.add_argument(
        "--backend",
        choices=("matplotlib", "pyqtgraph"),
        default="matplotlib",
        help="Plot library: matplotlib (default) or pyqtgraph (faster real-time)",
    )
    p.add_argument(
        "--print-hz",
        type=float,
        default=2.0,
        help="Print latest CH1/CH2 to stdout at most this many times per second (0=off)",
    )
    args = p.parse_args()

    block = max(256, int(args.block))
    nfft = 1 << (block - 1).bit_length()
    ch_idx = 0 if args.channel == "ch1" else 1

    freqs = np.fft.rfftfreq(nfft, d=1.0 / args.fs)
    hann = np.hanning(nfft)

    if args.backend == "matplotlib":
        run_matplotlib(args, nfft, ch_idx, freqs, hann)
    else:
        try:
            import pyqtgraph  # noqa: F401
        except ImportError:
            print(
                "pyqtgraph not installed. Try: pip install pyqtgraph PyQt5",
                file=sys.stderr,
            )
            raise
        run_pyqtgraph(args, nfft, ch_idx, freqs, hann)


if __name__ == "__main__":
    main()
