# hfsdr

HFSDR: host software, firmware, and hardware design for the HFSDR USB device.

## Getting started

- **Step-by-step instructions:** [lain-sdr challenge / user flow](https://hackin7.github.io/lain-sdr-chall) (companion site with guided setup).
- **User manual (slides):** [HFSDR User Manual](https://docs.google.com/presentation/d/1T5zb-2KSN37Saw5E9nwcajVOSR3CzVYfAOpIypQLBcQ/edit?usp=sharing)
- **Host testing (Python probe, GNU Radio `.grc` examples, drivers, protocol):** [docs/host-guide.md](docs/host-guide.md)

## Troubleshooting

- **USB / stream flaky:** Unplug the board or reset it a few times. The host stack sometimes needs a power cycle before enumeration, WinUSB binding, or streaming stabilizes.
- **Deeper host bring-up:** See the driver and probe sections in [docs/host-guide.md](docs/host-guide.md).

## Repository layout

| Path | Contents |
|------|----------|
| `ch32v305/` | Firmware (CH32V305). Build, flash, toolchain: [ch32v305/README.md](ch32v305/README.md). |
| `client-sw/` | Host Python (`hfsdr_probe`, PyUSB helpers), GNU Radio blocks and examples (`gr-hfsdr-lib`, `.grc`). |
| `docs/` | [host-guide.md](docs/host-guide.md) — primary host-side documentation. |
| `hardware/` | KiCad project and related hardware files. |
| `scripts/` | Helper scripts (e.g. udev `99-hfsdr.rules`, Windows USB checks). |
| `ui/` | WebUSB Web UI (Svelte/Vite). Deployed build: `homepage` in [ui/package.json](ui/package.json). |
