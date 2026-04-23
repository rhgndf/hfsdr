"""USB transport helper for gr-hfsdr blocks."""

import struct

import numpy as np
import usb.core
import usb.util

VENDOR_REQUEST_SET_CLK_FREQ = 3
VENDOR_REQUEST_GET_CLK_FREQ = 4
VENDOR_REQUEST_SET_TLV320_GAIN = 5
VENDOR_REQUEST_GET_PLL_LOCK = 6

DEFAULT_VID = 0xCAFE
DEFAULT_PID = 0x4031
DEFAULT_INTERFACE = 4
DEFAULT_EP_IN = 0x85


class HfSdrUsb:
    def __init__(
        self,
        vid=DEFAULT_VID,
        pid=DEFAULT_PID,
        interface_number=DEFAULT_INTERFACE,
        endpoint_in=DEFAULT_EP_IN,
        timeout_ms=200,
    ):
        self.vid = int(vid)
        self.pid = int(pid)
        self.interface_number = int(interface_number)
        self.endpoint_in = int(endpoint_in)
        self.timeout_ms = int(timeout_ms)
        self.dev = None
        self._claimed = False

    def open(self):
        self.dev = usb.core.find(idVendor=self.vid, idProduct=self.pid)
        if self.dev is None:
            raise RuntimeError(
                "USB device %#06x:%#06x not found" % (self.vid, self.pid)
            )
        try:
            if self.dev.is_kernel_driver_active(self.interface_number):
                self.dev.detach_kernel_driver(self.interface_number)
        except (NotImplementedError, usb.core.USBError):
            pass
        usb.util.claim_interface(self.dev, self.interface_number)
        self._claimed = True

    def close(self):
        if self.dev is None:
            return
        if self._claimed:
            usb.util.release_interface(self.dev, self.interface_number)
        usb.util.dispose_resources(self.dev)
        self._claimed = False
        self.dev = None

    def read_stream(self, transfer_size):
        if self.dev is None:
            raise RuntimeError("Device is not open")
        data = self.dev.read(self.endpoint_in, int(transfer_size), timeout=self.timeout_ms)
        return bytes(data)

    def set_clock_hz(self, hz):
        self.dev.ctrl_transfer(
            0x41,
            VENDOR_REQUEST_SET_CLK_FREQ,
            0,
            self.interface_number,
            struct.pack("<Q", int(hz)),
            timeout=self.timeout_ms,
        )

    def get_clock_hz(self):
        data = bytes(
            self.dev.ctrl_transfer(
                0xC1,
                VENDOR_REQUEST_GET_CLK_FREQ,
                0,
                self.interface_number,
                9,
                timeout=self.timeout_ms,
            )
        )
        return struct.unpack("<Q", data[1:])[0]

    def set_tlv320_gain_raw(self, gain_raw):
        self.dev.ctrl_transfer(
            0x41,
            VENDOR_REQUEST_SET_TLV320_GAIN,
            0,
            self.interface_number,
            bytes([int(gain_raw) & 0xFF]),
            timeout=self.timeout_ms,
        )

    def get_pll_lock(self):
        data = bytes(
            self.dev.ctrl_transfer(
                0xC1,
                VENDOR_REQUEST_GET_PLL_LOCK,
                0,
                self.interface_number,
                2,
                timeout=self.timeout_ms,
            )
        )
        return int(data[1])

    @staticmethod
    def words_to_iq(payload, scale=(1.0 / 2147483648.0)):
        usable = len(payload) & ~1
        if usable == 0:
            return np.empty((0,), dtype=np.complex64)
        words = np.frombuffer(payload[:usable], dtype="<u2")
        even_count = words.size & ~1
        words = words[:even_count]
        if words.size < 4:
            return np.empty((0,), dtype=np.complex64)
        slots = (words[0::2].astype(np.uint32) << 16) | words[1::2].astype(np.uint32)
        signed = slots.view(np.int32).astype(np.float32) * np.float32(scale)
        pair_count = (signed.size // 2) * 2
        signed = signed[:pair_count]
        return (signed[0::2] + 1j * signed[1::2]).astype(np.complex64, copy=False)
