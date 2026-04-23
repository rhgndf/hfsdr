"""GNU Radio source block for HFSDR over USB vendor interface."""

import threading
import time
from collections import deque

import numpy as np
from gnuradio import gr

from hfsdr_usb import (
    DEFAULT_EP_IN,
    DEFAULT_INTERFACE,
    DEFAULT_PID,
    DEFAULT_VID,
    HfSdrUsb,
)


class hfsdr_source(gr.sync_block):
    """
    Stream HFSDR IQ samples from USB vendor endpoint into GNU Radio.
    """

    def __init__(
        self,
        sample_rate=192000.0,
        vid=DEFAULT_VID,
        pid=DEFAULT_PID,
        interface=DEFAULT_INTERFACE,
        endpoint_in=DEFAULT_EP_IN,
        transfer_bytes=4096,
        usb_timeout_ms=200,
        buffer_ms=200,
        scale=(1.0 / 2147483648.0),
    ):
        gr.sync_block.__init__(
            self,
            name="hfsdr_source",
            in_sig=None,
            out_sig=[np.complex64],
        )

        self.sample_rate = float(sample_rate)
        self.transfer_bytes = int(transfer_bytes)
        self.usb_timeout_ms = int(usb_timeout_ms)
        self.scale = float(scale)
        self._max_iq_buffer = max(
            int((self.sample_rate * float(buffer_ms)) / 1000.0), self.transfer_bytes
        )

        self._dev = HfSdrUsb(
            vid=int(vid),
            pid=int(pid),
            interface_number=int(interface),
            endpoint_in=int(endpoint_in),
            timeout_ms=int(usb_timeout_ms),
        )

        self._lock = threading.Lock()
        self._stop_evt = threading.Event()
        self._reader = None
        self._queue = deque()
        self._queued_iq = 0
        self._read_errors = 0
        self._dropped_iq = 0

    def start(self):
        self._stop_evt.clear()
        self._dev.open()
        self._reader = threading.Thread(target=self._reader_loop, daemon=True)
        self._reader.start()
        return super().start()

    def stop(self):
        self._stop_evt.set()
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
                    while self._queued_iq > self._max_iq_buffer and self._queue:
                        old = self._queue.popleft()
                        self._queued_iq -= int(old.size)
                        self._dropped_iq += int(old.size)
            except Exception:
                self._read_errors += 1
                time.sleep(0.02)

    def work(self, input_items, output_items):
        out = output_items[0]
        produced = 0

        with self._lock:
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

        if produced < len(out):
            out[produced:] = 0.0 + 0.0j
            produced = len(out)
        return produced

    # Runtime controls mapped to existing vendor requests.
    def set_lo_hz(self, hz):
        self._dev.set_clock_hz(int(hz))

    def get_lo_hz(self):
        return int(self._dev.get_clock_hz().hz)

    def set_gain_raw(self, gain_raw):
        self._dev.set_tlv320_gain_raw(int(gain_raw) & 0xFF)

    def get_pll_locked(self):
        return int(self._dev.get_pll_lock().locked)

    def get_stats(self):
        with self._lock:
            return {
                "queued_iq": int(self._queued_iq),
                "queue_chunks": len(self._queue),
                "dropped_iq": int(self._dropped_iq),
                "read_errors": int(self._read_errors),
            }
