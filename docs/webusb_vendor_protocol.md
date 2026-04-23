# HFSDR WebUSB Vendor Protocol

This document defines the host-visible protocol for the HFSDR USB vendor
interface so tools and GNU Radio blocks can interoperate across Windows/Linux.

## USB identity and interface map

- Vendor ID: `0xCAFE`
- Product ID: dynamic from TinyUSB PID bitmap (`0x4031` with current class set)
- Vendor interface number: `4` (`ITF_NUM_VENDOR`)
- Vendor bulk endpoints:
  - IN: `0x85` (device -> host stream)
  - OUT: `0x05` (host -> device, currently echo/debug path)
- Transport: USB Bulk transfers (not isochronous)

## Stream payload contract

The stream emitted on endpoint `0x85` is a byte sequence of little-endian
16-bit words copied directly from the I2S RX DMA half-buffer.

- Source buffer type in firmware: `volatile uint16_t[]`
- Words per DMA half-buffer push: `256`
- Bytes per DMA half-buffer push: `512`
- Sample structure:
  - 24-bit audio from TLV320 is captured in 32-bit I2S slots.
  - Each 32-bit slot appears as two 16-bit words in transfer order.
  - A stereo frame is 4 words (2 slots), i.e. 8 bytes.

Host-side reconstruction:

1. Group stream bytes into little-endian `uint16_t` words.
2. Pair words into 32-bit slots:
   - `slot = (word_hi << 16) | word_lo`
3. Reinterpret `slot` as signed 32-bit (`int32`), then scale to float:
   - `float = slot / 2147483648.0`
4. Pair adjacent slots into IQ:
   - `I = slot[0]`, `Q = slot[1]`, repeated.

Current firmware sends raw sample stream without sequence counter or frame marker.
Drop detection should use:

- host USB timeout/read-gap monitoring,
- firmware drop counters (`usb_hw_vendor_dropped_words()` telemetry),
- optional future stream header framing if needed.

## Vendor control request contract

Requests use bmRequestType `vendor`. Recipient can be `device` (`wIndex=0`) or
`interface` (`wIndex=ITF_NUM_VENDOR`); host tools should use interface recipient.

### Request `1`: `VENDOR_REQUEST_WEBUSB`

- Direction: IN
- Purpose: fetch WebUSB landing page descriptor URL.

### Request `2`: `VENDOR_REQUEST_MICROSOFT`

- Direction: IN
- Purpose: fetch Microsoft OS 2.0 descriptor set (`wIndex=7`) for WinUSB binding.

### Request `3`: `VENDOR_REQUEST_SET_CLK_FREQ`

- Direction: OUT
- `wValue = 0`
- `wLength = 8`
- Payload: little-endian `uint64` frequency in Hz.
- Result: status stage success if Si5351 LO programming succeeds.

### Request `4`: `VENDOR_REQUEST_GET_CLK_FREQ`

- Direction: IN
- `wValue = 0`
- Response length: 9 bytes:
  - byte `[0]`: `ErrorStatus` from firmware (`READY`/`NoREADY` enum value)
  - bytes `[1..8]`: little-endian `uint64` frequency in Hz.

### Request `5`: `VENDOR_REQUEST_SET_TLV320_GAIN`

- Direction: OUT
- `wValue = 0`
- `wLength = 1`
- Payload: raw TLV320 gain register byte.
- Result: status stage success if TLV320 write succeeds.

### Request `6`: `VENDOR_REQUEST_GET_PLL_LOCK`

- Direction: IN
- `wValue = 0`
- Response length: 2 bytes:
  - byte `[0]`: `ErrorStatus`
  - byte `[1]`: lock state (`0` unlocked, `1` locked)

## Host recommendations

- Read endpoint `0x85` with transfer sizes that are multiples of 512 bytes.
- Use multiple in-flight transfers or a dedicated reader thread.
- Keep USB read and decode separate via a ring buffer for GNU Radio integration.
- Claim only vendor interface `4` so CDC/UAC interfaces remain available.
