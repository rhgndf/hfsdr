#!/usr/bin/env python3
"""
Treat serial CH1 / CH2 as I/Q (quadrature) samples for demodulation and spectrum.

Firmware line formats (same as i2s_serial_fft.py):
  ADC I2S CH1=-123 CH2=456
  CH1:-123,CH2:456

Complex sample: z = I + j*Q with I=CH1, Q=CH2 (use --swap-q for I=CH2, Q=CH1).

Plotting backends:
  matplotlib  — default; use --interval 16 for ~60 FPS
  pyqtgraph   — faster real-time; pip install pyqtgraph PyQt5

Console: prints latest CH1/CH2 at --print-hz (default 2/s); use --print-hz 0 to disable.

Dependencies:
  pip install pyserial numpy matplotlib
  pip install pyqtgraph PyQt5   # optional
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


def pair_to_complex(ch1, ch2, swap_q):
    """z = I + j*Q. Default I=CH1, Q=CH2; swap uses I=CH2, Q=CH1."""
    if swap_q:
        return complex(ch2, ch1)
    return complex(ch1, ch2)


def fm_discriminator(z):
    """Phase difference between consecutive samples (radians/sample). len = len(z)-1."""
    if z.size < 2:
        return np.array([], dtype=np.float64)
    return np.angle(z[1:] * np.conj(z[:-1]))


def one_pole_highpass(x, alpha):
    """y[n] = alpha * (y[n-1] + x[n] - x[n-1]); simple DC block."""
    if x.size == 0:
        return x
    y = np.zeros_like(x)
    y[0] = x[0]
    for n in range(1, x.size):
        y[n] = alpha * (y[n - 1] + x[n] - x[n - 1])
    return y


def run_matplotlib(args, nfft, freqs, hann):
    import matplotlib.pyplot as plt
    from matplotlib.animation import FuncAnimation

    buf = deque(maxlen=nfft)
    ser = serial.Serial(port=args.port, baudrate=args.baud, timeout=0.05)
    ser.reset_input_buffer()

    note_pair, tick_print = make_rate_limited_pair_printer(
        args.print_hz, "[%s]" % (args.port,)
    )

    def transform(ch1, ch2):
        note_pair(ch1, ch2)
        return pair_to_complex(ch1, ch2, args.swap_q)

    nrows = 3 if args.show_fm else 2
    fig, axes = plt.subplots(nrows, 1, figsize=(10, 8), squeeze=False)
    axes = axes.ravel()
    fig.suptitle(
        f"I/Q serial (matplotlib) — {args.port} @ {args.baud} — fs={args.fs:g} Hz — "
        f"I={'CH2' if args.swap_q else 'CH1'}, Q={'CH1' if args.swap_q else 'CH2'}"
    )

    ax_t = axes[0]
    (line_mag,) = ax_t.plot([], [], lw=0.8, label="|z|")
    ax_t.set_ylabel("|I+jQ|")
    ax_t.set_xlabel("Sample")
    ax_t.set_title("Envelope (AM magnitude)")
    ax_t.grid(True, alpha=0.3)
    ax_t.legend(loc="upper right", fontsize=8)

    idx_fm = 1
    line_fm = None
    ax_fm = None
    if args.show_fm:
        ax_fm = axes[idx_fm]
        (line_fm,) = ax_fm.plot([], [], lw=0.6, color="C1", label="FM dphi (rad/sample)")
        ax_fm.set_ylabel("Phase incr.")
        ax_fm.set_xlabel("Sample")
        ax_fm.set_title("FM discriminator + DC block")
        ax_fm.grid(True, alpha=0.3)
        ax_fm.legend(loc="upper right", fontsize=8)

    ax_sp = axes[-1]
    (line_spec,) = ax_sp.plot([], [], lw=0.8, color="C2")
    ax_sp.set_ylabel("|FFT|")
    ax_sp.set_xlabel("Frequency (Hz)")
    ax_sp.set_title("Complex FFT magnitude (Hann), fftshift")
    ax_sp.grid(True, alpha=0.3)

    def update(_frame):
        drain_serial_lines(ser, 2048, buf, transform)
        tick_print()
        if len(buf) < nfft:
            return (line_mag, line_spec) if line_fm is None else (line_mag, line_fm, line_spec)

        z = np.asarray(buf, dtype=np.complex128)

        mag = np.abs(z)
        line_mag.set_data(np.arange(nfft), mag)
        ax_t.relim()
        ax_t.autoscale_view()

        if args.show_fm and line_fm is not None and ax_fm is not None:
            dphi = fm_discriminator(z)
            if dphi.size > 0 and args.fm_hp > 0.0:
                dphi = one_pole_highpass(
                    dphi, float(np.clip(args.fm_hp, 0.0, 0.999999))
                )
            line_fm.set_data(np.arange(dphi.size), np.real(dphi))
            ax_fm.relim()
            ax_fm.autoscale_view()

        zw = z * hann
        spec = np.fft.fft(zw)
        spec_mag = np.abs(np.fft.fftshift(spec))
        line_spec.set_data(freqs, spec_mag)
        ax_sp.relim()
        ax_sp.autoscale_view()
        ax_sp.set_xlim(float(freqs.min()), float(freqs.max()))

        return (line_mag, line_spec) if line_fm is None else (line_mag, line_fm, line_spec)

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


def run_pyqtgraph(args, nfft, freqs, hann):
    import pyqtgraph as pg
    from pyqtgraph.Qt import QtCore

    buf = deque(maxlen=nfft)
    ser = serial.Serial(port=args.port, baudrate=args.baud, timeout=0.05)
    ser.reset_input_buffer()

    note_pair, tick_print = make_rate_limited_pair_printer(
        args.print_hz, "[%s]" % (args.port,)
    )

    def transform(ch1, ch2):
        note_pair(ch1, ch2)
        return pair_to_complex(ch1, ch2, args.swap_q)

    pg.setConfigOptions(antialias=True)
    app = pg.mkQApp("I/Q demod")

    glw = pg.GraphicsLayoutWidget(show=True)
    glw.resize(1000, 900)
    glw.setWindowTitle(
        f"I/Q — {args.port} — fs={args.fs:g} Hz (pyqtgraph)"
    )

    row = 0
    p_mag = glw.addPlot(row=row, col=0, title="Envelope |I+jQ|")
    p_mag.setLabel("left", "|z|")
    p_mag.setLabel("bottom", "Sample")
    p_mag.showGrid(x=True, y=True, alpha=0.3)
    curve_mag = p_mag.plot(pen=pg.mkPen("c", width=1))
    row += 1

    curve_fm = None
    p_fm = None
    if args.show_fm:
        p_fm = glw.addPlot(row=row, col=0, title="FM dphi (rad/sample) + DC block")
        p_fm.setLabel("left", "Phase incr.")
        p_fm.setLabel("bottom", "Sample")
        p_fm.showGrid(x=True, y=True, alpha=0.3)
        curve_fm = p_fm.plot(pen=pg.mkPen(255, 128, 0, width=1))
        row += 1

    p_sp = glw.addPlot(row=row, col=0, title="Complex FFT |·| (Hann), fftshift")
    p_sp.setLabel("left", "Magnitude")
    p_sp.setLabel("bottom", "Frequency (Hz)")
    p_sp.showGrid(x=True, y=True, alpha=0.3)
    p_sp.setXRange(float(freqs.min()), float(freqs.max()), padding=0)
    curve_sp = p_sp.plot(pen=pg.mkPen("g", width=1))

    def update():
        drain_serial_lines(ser, 2048, buf, transform)
        tick_print()
        if len(buf) < nfft:
            return
        z = np.asarray(buf, dtype=np.complex128)

        curve_mag.setData(np.arange(nfft), np.abs(z))

        if args.show_fm and curve_fm is not None:
            dphi = fm_discriminator(z)
            if dphi.size > 0 and args.fm_hp > 0.0:
                dphi = one_pole_highpass(
                    dphi, float(np.clip(args.fm_hp, 0.0, 0.999999))
                )
            curve_fm.setData(np.arange(dphi.size), np.real(dphi))

        zw = z * hann
        spec_mag = np.abs(np.fft.fftshift(np.fft.fft(zw)))
        curve_sp.setData(freqs, spec_mag)

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
    p = argparse.ArgumentParser(description="Serial CH1/CH2 as I/Q — FFT + FM demod")
    p.add_argument("--port", default="COM3", help="Serial port")
    p.add_argument("--baud", type=int, default=115200, help="Baud rate")
    p.add_argument(
        "--fs",
        type=float,
        default=96000.0,
        help="Complex sample rate (Hz)",
    )
    p.add_argument(
        "--block",
        type=int,
        default=4096,
        help="Complex samples per FFT frame (padded to next power of 2)",
    )
    p.add_argument("--swap-q", action="store_true", help="I=CH2, Q=CH1")
    p.add_argument("--show-fm", action="store_true", default=True)
    p.add_argument("--no-show-fm", action="store_false", dest="show_fm")
    p.add_argument(
        "--fm-hp",
        type=float,
        default=0.98,
        help="FM DC-block coefficient (0..1)",
    )
    p.add_argument(
        "--interval",
        type=int,
        default=16,
        help="Redraw period in ms (~16 ≈ 60 FPS)",
    )
    p.add_argument(
        "--backend",
        choices=("matplotlib", "pyqtgraph"),
        default="matplotlib",
        help="matplotlib (default) or pyqtgraph (faster real-time)",
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
    freqs = np.fft.fftshift(np.fft.fftfreq(nfft, d=1.0 / args.fs))
    hann = np.hanning(nfft)

    if args.backend == "matplotlib":
        run_matplotlib(args, nfft, freqs, hann)
    else:
        try:
            import pyqtgraph  # noqa: F401
        except ImportError:
            print(
                "pyqtgraph not installed. Try: pip install pyqtgraph PyQt5",
                file=sys.stderr,
            )
            raise
        run_pyqtgraph(args, nfft, freqs, hann)


if __name__ == "__main__":
    main()
