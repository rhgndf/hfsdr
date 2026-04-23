# gr-hfsdr-lib

Python-first GNU Radio source scaffold for HFSDR over the USB vendor endpoint.

## What this scaffold provides

- `python/hfsdr_source.py`: GNU Radio `sync_block` source outputting `gr_complex`.
- `python/hfsdr_usb.py`: PyUSB transport and vendor control requests.
- `grc/hfsdr_hfsdr_source.block.yml`: GRC block description for quick graph wiring.

## Dependencies

- GNU Radio 3.10+
- Python packages:
  - `numpy`
  - `pyusb`

Install:

```bash
py -3 -m pip install -r client-sw/host/python/requirements.txt
```

## Quick use from Python flowgraph

```python
import os
import sys
sys.path.insert(0, os.path.abspath("client-sw/gnuradio/gr-hfsdr-lib/python"))
from hfsdr_source import hfsdr_source

src = hfsdr_source(sample_rate=192000, vid=0xCAFE, pid=0x4031, interface=4, endpoint_in=0x85)
```

## Runtime controls

- `set_lo_hz(hz)`
- `get_lo_hz()`
- `set_gain_raw(gain_raw)`
- `get_pll_locked()`
- `get_stats()`

## Notes

- The source claims only the vendor interface, so CDC and UAC interfaces remain available.
- This is an initial scaffold intended for rapid bring-up. If needed, migrate to a full compiled OOT layout later.
