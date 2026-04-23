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
python3 client-sw/gnuradio/examples/hfsdr_webusb_demo.py
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

## 6) UI-like spectrogram parity (GNU Radio example)

Use:

- `client-sw/gnuradio/examples/basic_spectrum_iq/basic_spectrum_iq.grc`

Recommended baseline parameters (matched to web UI intent):

- `samp_rate = 192000`
- `fft_size = 2048`
- `wf_update_s = 0.06`
- `wf_db_min = -110`
- `wf_db_max = -55`
- `dc_block_len = 64`
- `i_gain = 1.0`
- `q_gain = 1.0`
- `gain_const = 1.0`

Graph path used for display:

- embedded source -> complex-to-float (I/Q split) -> per-branch gain -> float-to-complex -> DC blocker -> GUI sinks

Side-by-side checklist against browser UI:

1. Set same LO frequency and ADC gain in both interfaces.
2. Verify DC-centered spectrum shape (carrier at expected offset from center).
3. Compare noise floor stability over 10-20 seconds.
4. Compare peak prominence for the same RF signal.
5. Verify retune response latency feels similar when changing LO.
