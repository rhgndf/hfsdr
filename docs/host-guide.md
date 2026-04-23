# HFSDR host guide

This guide covers verifying the USB/IQ path with Python, running example GNU Radio Companion (`.grc`) flowgraphs, and reference material for drivers and the vendor protocol.

**If `hfsdr_probe` cannot see the device or claim the interface,** configure [USB driver setup](#hfsdr-usb-driver-setup-windows-and-linux) first, then return here.

---

## Test the USB connection with Python

Use the probe script to confirm the OS stack, WinUSB/udev, and vendor bulk stream before GNU Radio.

### Dependencies

- Python 3
- Packages from the repo: `client-sw/host/python/requirements.txt` (includes `pyusb` and a working **libusb** backend; on Windows use a libusb-compatible driver on the vendor interface—see driver section below).

Install from the repository root:

```bash
py -3 -m pip install -r client-sw/host/python/requirements.txt
```

(Linux/macOS: `python3 -m pip install ...` as appropriate.)

### Run the probe

From the **repository root**:

```bash
python3 client-sw/host/python/hfsdr_probe.py --duration-s 5 --report-s 1
```

On Windows you can use `py -3` instead of `python3`.

### What “good” looks like

- Clock and PLL control requests return valid responses.
- Stream rate is stable and non-zero.
- First decoded IQ samples print.

### Extra probe modes (especially on Windows)

```powershell
py -3 client-sw/host/python/hfsdr_probe.py --doctor
py -3 client-sw/host/python/hfsdr_probe.py --list-devices
py -3 client-sw/host/python/hfsdr_probe.py --auto-interface --duration-s 3
```

If the doctor finds the device but cannot claim the interface, the vendor interface is usually not bound to WinUSB yet.

---

## GNU Radio: example `.grc` flowgraphs

Use **GNU Radio Companion** (3.10+) with the `gr-hfsdr-lib` blocks so the flowgraph can open the HFSDR source.

### 1. Dependencies and block path

- Install GNU Radio 3.10+.
- Install Python deps: `py -3 -m pip install -r client-sw/host/python/requirements.txt` (see [gr-hfsdr-lib README](../client-sw/gnuradio/gr-hfsdr-lib/README.md)).

GNU Radio must find `gr-hfsdr-lib` Python modules and the block YAML. Typical options:

- Add the library `python` folder to `PYTHONPATH` before starting GRC, **or**
- Install/copy the OOT into a layout GNU Radio discovers (project uses a scaffold layout; see `gr-hfsdr-lib` README).

The block definition lives at `client-sw/gnuradio/gr-hfsdr-lib/grc/hfsdr_hfsdr_source.block.yml`.

### 2. Open an example flowgraph

In GNU Radio Companion: **File → Open** and choose one of:

| Example | Path |
|--------|------|
| Spectrum / waterfall style | `client-sw/gnuradio/examples/basic_spectrum_iq/basic_spectrum_iq.grc` |
| IQ record to file | `client-sw/gnuradio/examples/fm_iq_record_to_file.grc` |

Use **Generate** then **Execute**. You should see live plots updating while the device streams.

### 3. Suggested parameters (`basic_spectrum_iq`)

Baseline values aligned with the web UI intent:

- `samp_rate = 192000`
- `fft_size = 2048`
- `wf_update_s = 0.06`
- `wf_db_min = -110`
- `wf_db_max = -55`
- `dc_block_len = 64`
- `i_gain = 1.0`, `q_gain = 1.0`, `gain_const = 1.0`

Display path: embedded source → complex-to-float → per-branch gain → float-to-complex → DC blocker → GUI sinks.

### 4. Optional: Python demo (no `.grc`)

```bash
python3 client-sw/gnuradio/examples/hfsdr_webusb_demo.py
```

Expect spectrum and time sinks to update continuously, no repeated USB timeout loops, and clean stop when the window closes.

---

## HFSDR USB driver setup (Windows and Linux)

This setup keeps a single host code path (`pyusb` / libusb) and only changes device driver binding per OS.

### Interface strategy

- Claim only vendor interface `4` (bulk IN `0x85`, OUT `0x05`) from host tools.
- Leave CDC and UAC interfaces untouched so serial/audio features still work.

### Windows (WinUSB)

The firmware already includes:

- BOS WebUSB capability descriptor.
- Microsoft OS 2.0 descriptor with compatible ID `WINUSB` for vendor interface.

Expected behavior: interface 4 binds to WinUSB automatically on modern Windows.

#### Verify binding

1. Plug device and open Device Manager.
2. Find interface under `Universal Serial Bus devices`.
3. Confirm driver provider is Microsoft and driver is WinUSB.

#### If auto-bind fails (fallback)

1. Use [Zadig](https://zadig.akeo.ie/).
2. Select HFSDR vendor interface (not audio/CDC interfaces).
3. Install `WinUSB` driver only for that interface.

### Linux (libusb + udev)

Install udev rule so non-root users can access device:

```bash
sudo cp scripts/99-hfsdr.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger
```

Unplug/replug device after rule reload.

#### Verify

```bash
lsusb | grep -i cafe
```

Then run:

```bash
python3 client-sw/host/python/hfsdr_probe.py --duration-s 3
```

### Coexistence checks

On each OS:

1. `hfsdr_probe.py` can stream data and read PLL state.
2. CDC serial port still enumerates.
3. USB audio interface still enumerates.
4. GNU Radio `hfsdr_source` can run while CDC/audio are visible.

---

## HFSDR WebUSB vendor protocol

This section defines the host-visible protocol for the HFSDR USB vendor interface so tools and GNU Radio blocks can interoperate across Windows/Linux.

### USB identity and interface map

- Vendor ID: `0xCAFE`
- Product ID: dynamic from TinyUSB PID bitmap (`0x4031` with current class set)
- Vendor interface number: `4` (`ITF_NUM_VENDOR`)
- Vendor bulk endpoints:
  - IN: `0x85` (device → host stream)
  - OUT: `0x05` (host → device, currently echo/debug path)
- Transport: USB Bulk transfers (not isochronous)

### Stream payload contract

The stream emitted on endpoint `0x85` is a byte sequence of little-endian 16-bit words copied directly from the I2S RX DMA half-buffer.

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

Current firmware sends raw sample stream without sequence counter or frame marker. Drop detection should use:

- host USB timeout/read-gap monitoring,
- firmware drop counters (`usb_hw_vendor_dropped_words()` telemetry),
- optional future stream header framing if needed.

### Vendor control request contract

Requests use bmRequestType `vendor`. Recipient can be `device` (`wIndex=0`) or `interface` (`wIndex=ITF_NUM_VENDOR`); host tools should use interface recipient.

#### Request `1`: `VENDOR_REQUEST_WEBUSB`

- Direction: IN
- Purpose: fetch WebUSB landing page descriptor URL.

#### Request `2`: `VENDOR_REQUEST_MICROSOFT`

- Direction: IN
- Purpose: fetch Microsoft OS 2.0 descriptor set (`wIndex=7`) for WinUSB binding.

#### Request `3`: `VENDOR_REQUEST_SET_CLK_FREQ`

- Direction: OUT
- `wValue = 0`
- `wLength = 8`
- Payload: little-endian `uint64` frequency in Hz.
- Result: status stage success if Si5351 LO programming succeeds.

#### Request `4`: `VENDOR_REQUEST_GET_CLK_FREQ`

- Direction: IN
- `wValue = 0`
- Response length: 9 bytes:
  - byte `[0]`: `ErrorStatus` from firmware (`READY`/`NoREADY` enum value)
  - bytes `[1..8]`: little-endian `uint64` frequency in Hz.

#### Request `5`: `VENDOR_REQUEST_SET_TLV320_GAIN`

- Direction: OUT
- `wValue = 0`
- `wLength = 1`
- Payload: raw TLV320 gain register byte.
- Result: status stage success if TLV320 write succeeds.

#### Request `6`: `VENDOR_REQUEST_GET_PLL_LOCK`

- Direction: IN
- `wValue = 0`
- Response length: 2 bytes:
  - byte `[0]`: `ErrorStatus`
  - byte `[1]`: lock state (`0` unlocked, `1` locked)

### Host recommendations

- Read endpoint `0x85` with transfer sizes that are multiples of 512 bytes.
- Use multiple in-flight transfers or a dedicated reader thread.
- Keep USB read and decode separate via a ring buffer for GNU Radio integration.
- Claim only vendor interface `4` so CDC/UAC interfaces remain available.

---

## Extended validation checklist

Use after firmware flash and OS driver binding are correct.

### Runtime control checks

From a Python flowgraph shell or custom control script:

- `set_lo_hz(...)` changes tuner LO and observed spectrum shifts.
- `set_gain_raw(...)` changes measured signal amplitude.
- `get_pll_locked()` returns `1` during stable operation.

### Long run stability

Run for at least 10 minutes:

- No source freezes.
- No sustained growth in `read_errors` from `get_stats()`.
- `dropped_iq` remains low/acceptable for selected buffer size.

### Cross-platform parity

Repeat probe, GNU Radio smoke test, runtime checks, and long-run tests on:

- Windows (WinUSB on vendor interface).
- Linux (udev rule installed).

Record any differences in average stream throughput, USB timeout frequency, and startup reliability.

### UI-like spectrogram parity (GNU Radio vs browser)

Use `client-sw/gnuradio/examples/basic_spectrum_iq/basic_spectrum_iq.grc` and compare to the Web UI if applicable:

1. Set same LO frequency and ADC gain in both interfaces.
2. Verify DC-centered spectrum shape (carrier at expected offset from center).
3. Compare noise floor stability over 10–20 seconds.
4. Compare peak prominence for the same RF signal.
5. Verify retune response latency feels similar when changing LO.
