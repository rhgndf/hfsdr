#!/usr/bin/env python3
"""Convert recorded GNU Radio complex64 IQ to i32 IQ and run fm_demod_proto.

Example:
  py -3 scripts/fm_demod_proto/decode_recorded_iq.py \
    --in-c64 scripts/fm_demod_proto/fm_proto_data/capture_94p4_iq_c64.bin \
    --out-wav scripts/fm_demod_proto/fm_proto_data/capture_94p4_demod.wav
"""

from __future__ import annotations

import argparse
import subprocess
from pathlib import Path

import numpy as np


def c64_to_i32le(in_path: Path, out_path: Path) -> int:
    z = np.fromfile(in_path, dtype=np.complex64)
    if z.size == 0:
        raise RuntimeError(f"No IQ samples read from {in_path}")
    scale = float((1 << 30) - 1)
    i = np.clip(np.round(z.real * scale), -(1 << 30), (1 << 30) - 1).astype(np.int32)
    q = np.clip(np.round(z.imag * scale), -(1 << 30), (1 << 30) - 1).astype(np.int32)
    iq = np.empty(i.size * 2, dtype=np.int32)
    iq[0::2] = i
    iq[1::2] = q
    iq.astype("<i4", copy=False).tofile(out_path)
    return int(z.size)


def ensure_proto_exe(proto_dir: Path) -> Path:
    exe = proto_dir / "fm_demod_proto.exe"
    src = proto_dir / "fm_demod_proto.c"
    if exe.exists():
        return exe
    if not src.exists():
        raise RuntimeError(f"Missing C source: {src}")
    cmd = ["gcc", str(src), "-O2", "-std=c11", "-o", str(exe)]
    subprocess.run(cmd, check=True)
    return exe


def main() -> None:
    p = argparse.ArgumentParser(description="Decode recorded IQ capture with fm_demod_proto")
    p.add_argument("--in-c64", required=True, help="Input complex64 IQ file from GNU Radio file sink")
    p.add_argument("--out-wav", required=True, help="Output demodulated WAV file")
    p.add_argument("--out-i32", default="", help="Optional explicit path for converted i32 IQ file")
    p.add_argument("--report", default="", help="Optional report output path")
    args = p.parse_args()

    in_c64 = Path(args.in_c64)
    out_wav = Path(args.out_wav)
    proto_dir = Path(__file__).resolve().parent

    if not in_c64.exists():
        raise RuntimeError(f"Input file not found: {in_c64}")
    out_wav.parent.mkdir(parents=True, exist_ok=True)

    out_i32 = Path(args.out_i32) if args.out_i32 else out_wav.with_suffix(".i32le.bin")
    report = Path(args.report) if args.report else out_wav.with_suffix(".report.txt")

    n = c64_to_i32le(in_c64, out_i32)
    print(f"[decode] converted {n} complex samples -> {out_i32}")

    exe = ensure_proto_exe(proto_dir)
    cmd = [str(exe), str(out_i32), str(out_wav), str(report)]
    subprocess.run(cmd, check=True)
    print(f"[decode] wav: {out_wav}")
    print(f"[decode] report: {report}")


if __name__ == "__main__":
    main()
