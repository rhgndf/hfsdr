"""HFSDR USB helpers shared by probe and GNU Radio source."""

import struct
from dataclasses import dataclass
from typing import Iterable, Optional, List, Dict, Any

import numpy as np
import usb.backend.libusb1
import usb.core
import usb.util

VENDOR_REQUEST_WEBUSB = 1
VENDOR_REQUEST_MICROSOFT = 2
VENDOR_REQUEST_SET_CLK_FREQ = 3
VENDOR_REQUEST_GET_CLK_FREQ = 4
VENDOR_REQUEST_SET_TLV320_GAIN = 5
VENDOR_REQUEST_GET_PLL_LOCK = 6

DEFAULT_VID = 0xCAFE
DEFAULT_PID = 0x4031
DEFAULT_INTERFACE = 4
DEFAULT_EP_IN = 0x85
DEFAULT_EP_OUT = 0x05


def _get_backend():
    """
    Prefer libusb-package backend when available (Windows-friendly),
    then fall back to default libusb backend discovery.
    """
    try:
        import libusb_package  # type: ignore

        backend = usb.backend.libusb1.get_backend(
            find_library=libusb_package.find_library
        )
        if backend is not None:
            return backend
    except Exception:
        pass

    return usb.backend.libusb1.get_backend()


@dataclass(frozen=True)
class ClockState:
    status: int
    hz: int


@dataclass(frozen=True)
class PllState:
    status: int
    locked: int


class HfSdrUsb:
    """Minimal USB transport for the HFSDR vendor interface."""

    def __init__(
        self,
        vid: int = DEFAULT_VID,
        pid: int = DEFAULT_PID,
        interface_number: int = DEFAULT_INTERFACE,
        endpoint_in: int = DEFAULT_EP_IN,
        endpoint_out: int = DEFAULT_EP_OUT,
        timeout_ms: int = 200,
    ) -> None:
        self.vid = vid
        self.pid = pid
        self.interface_number = interface_number
        self.endpoint_in = endpoint_in
        self.endpoint_out = endpoint_out
        self.timeout_ms = timeout_ms
        self.dev: Optional[usb.core.Device] = None
        self._claimed = False

    def open(self) -> None:
        backend = _get_backend()
        self.dev = usb.core.find(idVendor=self.vid, idProduct=self.pid, backend=backend)
        if self.dev is None:
            raise RuntimeError(
                f"USB device {self.vid:#06x}:{self.pid:#06x} not found"
            )
        try:
            if self.dev.is_kernel_driver_active(self.interface_number):
                self.dev.detach_kernel_driver(self.interface_number)
        except (NotImplementedError, usb.core.USBError):
            # Windows/libusb backends may not support this call.
            pass

        try:
            self.dev.set_configuration()
        except usb.core.USBError:
            # Some composite devices are already configured by the OS.
            pass

        usb.util.claim_interface(self.dev, self.interface_number)
        self._claimed = True

    @staticmethod
    def enumerate_devices(vid: Optional[int] = None, pid: Optional[int] = None) -> List[Dict[str, Any]]:
        backend = _get_backend()
        devices = usb.core.find(
            find_all=True, idVendor=vid, idProduct=pid, backend=backend
        )
        out: List[Dict[str, Any]] = []
        for dev in devices or []:
            item: Dict[str, Any] = {
                "vid": int(dev.idVendor),
                "pid": int(dev.idProduct),
                "bus": getattr(dev, "bus", None),
                "address": getattr(dev, "address", None),
                "interfaces": [],
            }
            for cfg in dev:
                for intf in cfg:
                    eps = [int(ep.bEndpointAddress) for ep in intf.endpoints()]
                    item["interfaces"].append(
                        {
                            "number": int(intf.bInterfaceNumber),
                            "class": int(intf.bInterfaceClass),
                            "subclass": int(intf.bInterfaceSubClass),
                            "protocol": int(intf.bInterfaceProtocol),
                            "endpoints": eps,
                        }
                    )
            out.append(item)
        return out

    @staticmethod
    def guess_vendor_interface(
        vid: int,
        pid: int,
        endpoint_in: Optional[int] = None,
        endpoint_out: Optional[int] = None,
    ) -> Optional[int]:
        backend = _get_backend()
        dev = usb.core.find(idVendor=vid, idProduct=pid, backend=backend)
        if dev is None:
            return None

        for cfg in dev:
            for intf in cfg:
                if int(intf.bInterfaceClass) != 0xFF:
                    continue
                eps = {int(ep.bEndpointAddress) for ep in intf.endpoints()}
                if endpoint_in is not None and endpoint_in not in eps:
                    continue
                if endpoint_out is not None and endpoint_out not in eps:
                    continue
                return int(intf.bInterfaceNumber)
        return None

    def close(self) -> None:
        if self.dev is None:
            return
        if self._claimed:
            usb.util.release_interface(self.dev, self.interface_number)
        usb.util.dispose_resources(self.dev)
        self.dev = None
        self._claimed = False

    def read_stream(self, transfer_size: int) -> bytes:
        if self.dev is None:
            raise RuntimeError("Device is not open")
        raw = self.dev.read(self.endpoint_in, transfer_size, timeout=self.timeout_ms)
        return bytes(raw)

    def write_stream(self, payload: bytes) -> int:
        if self.dev is None:
            raise RuntimeError("Device is not open")
        return int(self.dev.write(self.endpoint_out, payload, timeout=self.timeout_ms))

    def set_clock_hz(self, hz: int) -> None:
        if self.dev is None:
            raise RuntimeError("Device is not open")
        payload = struct.pack("<Q", hz)
        self.dev.ctrl_transfer(
            bmRequestType=0x41,
            bRequest=VENDOR_REQUEST_SET_CLK_FREQ,
            wValue=0,
            wIndex=self.interface_number,
            data_or_wLength=payload,
            timeout=self.timeout_ms,
        )

    def get_clock_hz(self) -> ClockState:
        if self.dev is None:
            raise RuntimeError("Device is not open")
        response = bytes(
            self.dev.ctrl_transfer(
                bmRequestType=0xC1,
                bRequest=VENDOR_REQUEST_GET_CLK_FREQ,
                wValue=0,
                wIndex=self.interface_number,
                data_or_wLength=9,
                timeout=self.timeout_ms,
            )
        )
        if len(response) != 9:
            raise RuntimeError(f"Unexpected clock state response length: {len(response)}")
        return ClockState(status=response[0], hz=struct.unpack("<Q", response[1:])[0])

    def set_tlv320_gain_raw(self, gain_raw: int) -> None:
        if self.dev is None:
            raise RuntimeError("Device is not open")
        self.dev.ctrl_transfer(
            bmRequestType=0x41,
            bRequest=VENDOR_REQUEST_SET_TLV320_GAIN,
            wValue=0,
            wIndex=self.interface_number,
            data_or_wLength=bytes([gain_raw & 0xFF]),
            timeout=self.timeout_ms,
        )

    def get_pll_lock(self) -> PllState:
        if self.dev is None:
            raise RuntimeError("Device is not open")
        response = bytes(
            self.dev.ctrl_transfer(
                bmRequestType=0xC1,
                bRequest=VENDOR_REQUEST_GET_PLL_LOCK,
                wValue=0,
                wIndex=self.interface_number,
                data_or_wLength=2,
                timeout=self.timeout_ms,
            )
        )
        if len(response) != 2:
            raise RuntimeError(f"Unexpected PLL state response length: {len(response)}")
        return PllState(status=response[0], locked=response[1])

    @staticmethod
    def unpack_words(payload: bytes) -> np.ndarray:
        usable = len(payload) & ~1
        if usable == 0:
            return np.empty((0,), dtype=np.uint16)
        return np.frombuffer(payload[:usable], dtype="<u2")

    @staticmethod
    def words_to_iq(payload: bytes, scale: float = 1.0 / 2147483648.0) -> np.ndarray:
        words = HfSdrUsb.unpack_words(payload)
        if words.size < 4:
            return np.empty((0,), dtype=np.complex64)

        even_count = words.size & ~1
        words = words[:even_count]
        slots = (words[0::2].astype(np.uint32) << 16) | words[1::2].astype(np.uint32)
        signed = slots.view(np.int32).astype(np.float32) * np.float32(scale)
        pair_count = (signed.size // 2) * 2
        signed = signed[:pair_count]
        iq = signed[0::2] + 1j * signed[1::2]
        return iq.astype(np.complex64, copy=False)

    @staticmethod
    def iter_iq(chunks: Iterable[bytes], scale: float = 1.0 / 2147483648.0):
        for payload in chunks:
            iq = HfSdrUsb.words_to_iq(payload, scale=scale)
            if iq.size:
                yield iq
