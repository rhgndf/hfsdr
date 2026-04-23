# HFSDR USB Driver Setup (Windows and Linux)

This setup keeps a single host code path (`pyusb/libusb`) and only changes
device driver binding per OS.

## Interface strategy

- Claim only vendor interface `4` (bulk IN `0x85`, OUT `0x05`) from host tools.
- Leave CDC and UAC interfaces untouched so serial/audio features still work.

## Windows (WinUSB)

The firmware already includes:

- BOS WebUSB capability descriptor.
- Microsoft OS 2.0 descriptor with compatible ID `WINUSB` for vendor interface.

Expected behavior: interface 4 binds to WinUSB automatically on modern Windows.

### Verify binding

1. Plug device and open Device Manager.
2. Find interface under `Universal Serial Bus devices`.
3. Confirm driver provider is Microsoft and driver is WinUSB.

### If auto-bind fails (fallback)

1. Use [Zadig](https://zadig.akeo.ie/).
2. Select HFSDR vendor interface (not audio/CDC interfaces).
3. Install `WinUSB` driver only for that interface.

## Linux (libusb + udev)

Install udev rule so non-root users can access device:

```bash
sudo cp scripts/99-hfsdr.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger
```

Unplug/replug device after rule reload.

### Verify

```bash
lsusb | grep -i cafe
```

Then run:

```bash
python3 client-sw/host/python/hfsdr_probe.py --duration-s 3
```

## Coexistence checks

Run these checks on each OS:

1. `hfsdr_probe.py` can stream data and read PLL state.
2. CDC serial port still enumerates.
3. USB audio interface still enumerates.
4. GNU Radio `hfsdr_source` can run while CDC/audio are visible.

## Fast Windows troubleshooting

Use the Python doctor mode first:

```powershell
py -3 client-sw/host/python/hfsdr_probe.py --doctor
```

Useful helper modes:

```powershell
py -3 client-sw/host/python/hfsdr_probe.py --list-devices
py -3 client-sw/host/python/hfsdr_probe.py --auto-interface --duration-s 3
```

If doctor finds the device but cannot claim interface, this almost always means
the vendor interface is not bound to WinUSB yet.
