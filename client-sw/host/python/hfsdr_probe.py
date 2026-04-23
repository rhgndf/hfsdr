#!/usr/bin/env python3
"""Probe HFSDR vendor endpoint stream and control requests."""

import argparse
import platform
import signal
import sys
import time
import traceback

from hfsdr_usb import (
    DEFAULT_EP_IN,
    DEFAULT_EP_OUT,
    DEFAULT_INTERFACE,
    DEFAULT_PID,
    DEFAULT_VID,
    HfSdrUsb,
)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="HFSDR USB probe (bulk stream + vendor controls)"
    )
    parser.add_argument("--vid", type=lambda x: int(x, 0), default=DEFAULT_VID)
    parser.add_argument("--pid", type=lambda x: int(x, 0), default=DEFAULT_PID)
    parser.add_argument("--interface", type=int, default=DEFAULT_INTERFACE)
    parser.add_argument("--endpoint-in", type=lambda x: int(x, 0), default=DEFAULT_EP_IN)
    parser.add_argument(
        "--endpoint-out", type=lambda x: int(x, 0), default=DEFAULT_EP_OUT
    )
    parser.add_argument("--timeout-ms", type=int, default=200)
    parser.add_argument("--transfer-bytes", type=int, default=4096)
    parser.add_argument("--duration-s", type=float, default=10.0)
    parser.add_argument("--report-s", type=float, default=1.0)
    parser.add_argument("--set-lo-hz", type=int, default=None)
    parser.add_argument("--set-gain-raw", type=lambda x: int(x, 0), default=None)
    parser.add_argument(
        "--print-first-iq",
        type=int,
        default=8,
        help="Print first N decoded IQ samples from the stream",
    )
    parser.add_argument(
        "--auto-interface",
        action="store_true",
        help="Auto-detect vendor interface by class/endpoints before connecting",
    )
    parser.add_argument(
        "--list-devices",
        action="store_true",
        help="List matching USB devices/interfaces and exit",
    )
    parser.add_argument(
        "--doctor",
        action="store_true",
        help="Run diagnostics and exit",
    )
    parser.add_argument(
        "--debug",
        action="store_true",
        help="Print traceback on errors",
    )
    return parser


def list_devices(vid: int, pid: int) -> int:
    devices = HfSdrUsb.enumerate_devices(vid=vid, pid=pid)
    if not devices:
        print(f"no USB devices found for {vid:#06x}:{pid:#06x}")
        return 1
    for idx, dev in enumerate(devices, start=1):
        print(
            f"[{idx}] {dev['vid']:#06x}:{dev['pid']:#06x} "
            f"bus={dev['bus']} addr={dev['address']}"
        )
        for intf in dev["interfaces"]:
            eps = ", ".join(f"{ep:#04x}" for ep in intf["endpoints"]) or "none"
            print(
                "    itf={number} class=0x{cls:02x} subclass=0x{sub:02x} "
                "protocol=0x{proto:02x} eps=[{eps}]".format(
                    number=intf["number"],
                    cls=intf["class"],
                    sub=intf["subclass"],
                    proto=intf["protocol"],
                    eps=eps,
                )
            )
    return 0


def run_doctor(args) -> int:
    print("hfsdr_probe doctor")
    print(f"platform: {platform.platform()}")
    print(f"python: {sys.version.split()[0]}")

    try:
        _ = HfSdrUsb.enumerate_devices(vid=args.vid, pid=args.pid)
        print("pyusb backend: OK")
    except Exception as exc:  # pylint: disable=broad-except
        print(f"pyusb backend: ERROR ({exc})")
        print("hint: install pyusb and a libusb backend available to Python.")
        return 2

    rc = list_devices(args.vid, args.pid)
    if rc != 0:
        print("hint: check VID/PID and USB enumeration first.")
        return rc

    guessed = HfSdrUsb.guess_vendor_interface(
        args.vid,
        args.pid,
        endpoint_in=args.endpoint_in,
        endpoint_out=args.endpoint_out,
    )
    if guessed is None:
        print("vendor interface guess: failed")
        print(
            "hint: on Windows, bind the vendor interface to WinUSB "
            "(do not replace CDC/audio drivers)."
        )
        return 3

    print(f"vendor interface guess: {guessed}")
    print("doctor result: ready to run the probe")
    return 0


def main() -> int:
    args = build_parser().parse_args()
    running = True

    def _stop(_sig, _frame):
        nonlocal running
        running = False

    signal.signal(signal.SIGINT, _stop)
    signal.signal(signal.SIGTERM, _stop)

    if args.list_devices:
        return list_devices(args.vid, args.pid)

    if args.doctor:
        return run_doctor(args)

    interface_number = args.interface
    if args.auto_interface:
        guessed = HfSdrUsb.guess_vendor_interface(
            args.vid,
            args.pid,
            endpoint_in=args.endpoint_in,
            endpoint_out=args.endpoint_out,
        )
        if guessed is None:
            print(
                "probe error: could not auto-detect vendor interface for requested endpoints",
                file=sys.stderr,
            )
            return 2
        interface_number = guessed
        print(f"auto-detected vendor interface: {interface_number}")

    dev = HfSdrUsb(
        vid=args.vid,
        pid=args.pid,
        interface_number=interface_number,
        endpoint_in=args.endpoint_in,
        endpoint_out=args.endpoint_out,
        timeout_ms=args.timeout_ms,
    )

    try:
        dev.open()
        print(
            f"connected {args.vid:#06x}:{args.pid:#06x} "
            f"itf={interface_number} ep_in={args.endpoint_in:#04x}"
        )

        if args.set_lo_hz is not None:
            dev.set_clock_hz(args.set_lo_hz)
            print(f"set LO request sent: {args.set_lo_hz} Hz")

        if args.set_gain_raw is not None:
            dev.set_tlv320_gain_raw(args.set_gain_raw)
            print(f"set TLV320 gain request sent: 0x{args.set_gain_raw & 0xFF:02x}")

        clk = dev.get_clock_hz()
        pll = dev.get_pll_lock()
        print(f"clock state: status={clk.status} hz={clk.hz}")
        print(f"pll state: status={pll.status} locked={pll.locked}")

        started = time.monotonic()
        next_report = started + max(args.report_s, 0.2)
        total_bytes = 0
        total_iq = 0
        first_printed = False
        transfer_count = 0

        while running and (time.monotonic() - started) < args.duration_s:
            payload = dev.read_stream(args.transfer_bytes)
            transfer_count += 1
            total_bytes += len(payload)
            iq = dev.words_to_iq(payload)
            total_iq += int(iq.size)

            if not first_printed and iq.size:
                first_printed = True
                n = min(args.print_first_iq, int(iq.size))
                print(f"first {n} IQ samples:")
                for idx in range(n):
                    sample = iq[idx]
                    print(f"  {idx:03d}: {sample.real:+0.6f} {sample.imag:+0.6f}j")

            now = time.monotonic()
            if now >= next_report:
                elapsed = now - started
                mib_s = (total_bytes / (1024.0 * 1024.0)) / max(elapsed, 1e-6)
                print(
                    f"stream {elapsed:6.2f}s | {mib_s:7.3f} MiB/s | "
                    f"{total_iq:10d} IQ samples | {transfer_count:7d} transfers"
                )
                next_report = now + max(args.report_s, 0.2)

        elapsed = max(time.monotonic() - started, 1e-6)
        print(
            f"done {elapsed:.2f}s total={total_bytes} bytes "
            f"({(total_bytes / (1024.0 * 1024.0)) / elapsed:.3f} MiB/s), "
            f"iq={total_iq}, transfers={transfer_count}"
        )
        return 0
    except Exception as exc:  # pylint: disable=broad-except
        print(f"probe error: {exc}", file=sys.stderr)
        msg = str(exc).lower()
        if "no backend available" in msg:
            print(
                "hint: pyusb has no libusb backend. Install libusb and restart the shell.",
                file=sys.stderr,
            )
        elif "access is denied" in msg or "permission" in msg:
            print(
                "hint: driver/permission issue. On Windows bind vendor interface to WinUSB.",
                file=sys.stderr,
            )
        elif "entity not found" in msg or "not found" in msg:
            print(
                "hint: try --doctor or --auto-interface to verify interface/endpoints.",
                file=sys.stderr,
            )
        if args.debug:
            traceback.print_exc()
        return 1
    finally:
        dev.close()


if __name__ == "__main__":
    raise SystemExit(main())
