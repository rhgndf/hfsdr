# GNU Radio Validation Checklist

Use this checklist after flashing firmware and setting OS driver bindings.

## 1) Host probe sanity

Run:

```bash
python3 client-sw/host/python/hfsdr_probe.py --duration-s 5 --report-s 1
```

Expected:

- Clock and PLL control requests return valid responses.
- Stream rate is stable and non-zero.
- First decoded IQ samples print.

## 2) GNU Radio source smoke test

Run:

```bash
python3 client-sw/flowgraphs/hfsdr_webusb_demo.py
```

Expected:

- Spectrum and time sinks update continuously.
- No repeated USB timeout/reset loops.
- Closing the window cleanly stops USB streaming.

## 3) Runtime control checks

From a Python flowgraph shell or custom control script:

- `set_lo_hz(...)` changes tuner LO and observed spectrum shifts.
- `set_gain_raw(...)` changes measured signal amplitude.
- `get_pll_locked()` returns `1` during stable operation.

## 4) Long run stability

Run for at least 10 minutes:

- No source freezes.
- No sustained growth in `read_errors` from `get_stats()`.
- `dropped_iq` remains low/acceptable for selected buffer size.

## 5) Cross-platform parity

Repeat sections 1-4 on:

- Windows (WinUSB on vendor interface).
- Linux (udev rule installed).

Record any differences in:

- average stream throughput,
- USB timeout frequency,
- startup reliability.
