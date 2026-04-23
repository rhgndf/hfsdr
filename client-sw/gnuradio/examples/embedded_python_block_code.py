"""
GNU Radio Embedded Python Block source for HFSDR USB stream.

Paste class `blk` into an Embedded Python Block, or import this file directly.
"""

import os
import sys
import time
import threading
from collections import deque

import numpy as np
from gnuradio import gr


class blk(gr.sync_block):
    def __init__(
        self,
        sample_rate=192000.0,
        vid=0xCAFE,
        pid=0x4031,
        interface=4,
        endpoint_in=0x85,
        transfer_bytes=65536,
        usb_timeout_ms=200,
        buffer_ms=1000,
        scale=(1.0 / 2147483648.0),
        lo_hz=7067333,
        apply_lo_on_start=True,
        gain_raw=0x00,
        apply_gain_on_start=False,
        stats_report_s=1.0,
        stats_to_console=True,
        module_root="",  # optional absolute path to repo root
    ):
        gr.sync_block.__init__(
            self,
            name="HFSDR Embedded Source",
            in_sig=None,
            out_sig=[np.complex64],
        )

        repo_root = module_root if module_root else os.getcwd()
        host_py = os.path.abspath(os.path.join(repo_root, "client-sw", "host", "python"))
        if host_py not in sys.path:
            sys.path.insert(0, host_py)

        from hfsdr_usb import HfSdrUsb  # import after sys.path update

        self._HfSdrUsb = HfSdrUsb
        self.sample_rate = float(sample_rate)
        self.transfer_bytes = int(transfer_bytes)
        self.scale = float(scale)
        self.lo_hz = int(lo_hz)
        self.apply_lo_on_start = bool(apply_lo_on_start)
        self.gain_raw = int(gain_raw) & 0xFF
        self.apply_gain_on_start = bool(apply_gain_on_start)
        self.stats_report_s = max(float(stats_report_s), 0.1)
        self.stats_to_console = bool(stats_to_console)

        self._max_iq_buffer = max(
            int((self.sample_rate * float(buffer_ms)) / 1000.0),
            self.transfer_bytes,
        )

        self._dev = self._HfSdrUsb(
            vid=int(vid),
            pid=int(pid),
            interface_number=int(interface),
            endpoint_in=int(endpoint_in),
            timeout_ms=int(usb_timeout_ms),
        )

        self._lock = threading.Lock()
        self._queue_cv = threading.Condition(self._lock)
        self._queue = deque()
        self._queued_iq = 0
        self._dropped_iq = 0
        self._read_errors = 0
        self._work_underruns = 0
        self._last_control_error = ""

        self._stop_evt = threading.Event()
        self._reader = None
        self._running = False
        self._started_at = 0.0
        self._last_stats_at = 0.0
        self._bytes_total = 0
        self._bytes_since_report = 0
        self._iq_total = 0
        self._iq_since_report = 0
        self._xfers_total = 0
        self._xfers_since_report = 0
        self._last_report = {}

    def start(self):
        self._stop_evt.clear()
        self._dev.open()
        self._running = True
        now = time.monotonic()
        self._started_at = now
        self._last_stats_at = now

        if self.apply_lo_on_start:
            self._set_lo_internal(self.lo_hz)
        if self.apply_gain_on_start:
            self._set_gain_internal(self.gain_raw)

        self._reader = threading.Thread(target=self._reader_loop, daemon=True)
        self._reader.start()
        return super().start()

    def stop(self):
        self._stop_evt.set()
        self._running = False
        if self._reader is not None:
            self._reader.join(timeout=1.0)
            self._reader = None
        self._dev.close()
        return super().stop()

    def _reader_loop(self):
        while not self._stop_evt.is_set():
            try:
                payload = self._dev.read_stream(self.transfer_bytes)
                iq = self._dev.words_to_iq(payload, scale=self.scale)
                if iq.size == 0:
                    continue
                with self._lock:
                    self._queue.append(iq)
                    self._queued_iq += int(iq.size)
                    self._bytes_total += len(payload)
                    self._bytes_since_report += len(payload)
                    self._iq_total += int(iq.size)
                    self._iq_since_report += int(iq.size)
                    self._xfers_total += 1
                    self._xfers_since_report += 1
                    while self._queued_iq > self._max_iq_buffer and self._queue:
                        old = self._queue.popleft()
                        self._queued_iq -= int(old.size)
                        self._dropped_iq += int(old.size)
                    self._queue_cv.notify()
                self._maybe_report_stats()
            except Exception:
                self._read_errors += 1
                time.sleep(0.02)

    def _maybe_report_stats(self):
        now = time.monotonic()
        elapsed = now - self._last_stats_at
        if elapsed < self.stats_report_s:
            return

        with self._lock:
            bytes_since = self._bytes_since_report
            iq_since = self._iq_since_report
            xfers_since = self._xfers_since_report
            queued_iq = int(self._queued_iq)
            dropped_iq = int(self._dropped_iq)
            read_errors = int(self._read_errors)
            total_bytes = int(self._bytes_total)
            total_iq = int(self._iq_total)
            total_xfers = int(self._xfers_total)
            self._bytes_since_report = 0
            self._iq_since_report = 0
            self._xfers_since_report = 0

        mbps = (bytes_since / max(elapsed, 1e-9)) / (1024.0 * 1024.0)
        iqps = iq_since / max(elapsed, 1e-9)
        xfersps = xfers_since / max(elapsed, 1e-9)

        self._last_report = {
            "mbps": mbps,
            "iqps": iqps,
            "xfersps": xfersps,
            "queued_iq": queued_iq,
            "dropped_iq": dropped_iq,
            "read_errors": read_errors,
            "total_bytes": total_bytes,
            "total_iq": total_iq,
            "total_xfers": total_xfers,
            "uptime_s": now - self._started_at,
        }
        self._last_stats_at = now

        if self.stats_to_console:
            print(
                "[HFSDR] {mbps:.3f} MiB/s | {iqps:.0f} iq/s | "
                "{xfersps:.1f} xfer/s | q={queued_iq} drop={dropped_iq} err={read_errors}".format(
                    **self._last_report
                )
            )

    def work(self, input_items, output_items):
        out = output_items[0]
        produced = 0

        with self._queue_cv:
            if not self._queue:
                self._queue_cv.wait(timeout=0.003)
            while produced < len(out) and self._queue:
                chunk = self._queue[0]
                take = min(len(out) - produced, len(chunk))
                out[produced : produced + take] = chunk[:take]
                produced += take

                if take == len(chunk):
                    self._queue.popleft()
                else:
                    self._queue[0] = chunk[take:]

                self._queued_iq -= take

        if produced == 0:
            self._work_underruns += 1
        return produced

    def _set_lo_internal(self, hz):
        try:
            self._dev.set_clock_hz(int(hz))
            self._last_control_error = ""
        except Exception as exc:  # pylint: disable=broad-except
            self._last_control_error = str(exc)

    def _set_gain_internal(self, gain_raw):
        try:
            self._dev.set_tlv320_gain_raw(int(gain_raw) & 0xFF)
            self._last_control_error = ""
        except Exception as exc:  # pylint: disable=broad-except
            self._last_control_error = str(exc)

    def set_lo_hz(self, lo_hz):
        """
        GRC can call this setter when a connected variable changes.
        """
        self.lo_hz = int(lo_hz)
        if self._running:
            self._set_lo_internal(self.lo_hz)

    def set_gain_raw(self, gain_raw):
        """
        GRC can call this setter when a connected variable changes.
        """
        self.gain_raw = int(gain_raw) & 0xFF
        if self._running:
            self._set_gain_internal(self.gain_raw)

    def get_stats(self):
        with self._lock:
            return {
                "queued_iq": int(self._queued_iq),
                "queue_chunks": len(self._queue),
                "dropped_iq": int(self._dropped_iq),
                "read_errors": int(self._read_errors),
                "work_underruns": int(self._work_underruns),
                "lo_hz": int(self.lo_hz),
                "gain_raw": int(self.gain_raw),
                "last_control_error": self._last_control_error,
                "bytes_total": int(self._bytes_total),
                "iq_total": int(self._iq_total),
                "xfers_total": int(self._xfers_total),
                "stats_report_s": float(self.stats_report_s),
                "last_report": dict(self._last_report),
            }
