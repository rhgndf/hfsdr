#!/usr/bin/env python3
"""Generate deterministic FM IQ test vectors and reference audio.

Outputs are written under scripts/fm_demod_proto/fm_proto_data by default:
  - <case>_iq_i32le.bin          interleaved I,Q int32 little-endian
  - <case>_ref_audio_f32le.bin   mono reference audio float32 @ 48 kHz
  - <case>_meta.json             per-case metadata
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

import numpy as np


FS_IQ = 192_000
FS_AUDIO = 48_000
DECIM = FS_IQ // FS_AUDIO


def _deemph_50us(x: np.ndarray, fs: int) -> np.ndarray:
    tau = 50e-6
    alpha = math.exp(-1.0 / (fs * tau))
    y = np.empty_like(x, dtype=np.float32)
    yp = 0.0
    for i, v in enumerate(x.astype(np.float32, copy=False)):
        yp = (1.0 - alpha) * float(v) + alpha * yp
        y[i] = yp
    return y


def _audio_case(case: str, t: np.ndarray) -> np.ndarray:
    if case == "single_tone":
        return 0.65 * np.sin(2.0 * np.pi * 1000.0 * t, dtype=np.float32)
    if case == "two_tone":
        a = 0.55 * np.sin(2.0 * np.pi * 1000.0 * t, dtype=np.float32)
        b = 0.25 * np.sin(2.0 * np.pi * 2300.0 * t, dtype=np.float32)
        return (a + b).astype(np.float32)
    if case == "noisy_two_tone":
        a = 0.50 * np.sin(2.0 * np.pi * 900.0 * t, dtype=np.float32)
        b = 0.20 * np.sin(2.0 * np.pi * 1800.0 * t, dtype=np.float32)
        return (a + b).astype(np.float32)
    raise ValueError(f"Unknown case: {case}")


def _fm_modulate(audio: np.ndarray, fs: int, fdev_hz: float) -> np.ndarray:
    phase_inc = (2.0 * np.pi * fdev_hz / float(fs)) * audio.astype(np.float64)
    phase = np.cumsum(phase_inc, dtype=np.float64)
    return np.exp(1j * phase).astype(np.complex64)


def _add_noise(z: np.ndarray, sigma: float, rng: np.random.Generator) -> np.ndarray:
    if sigma <= 0.0:
        return z
    n = (rng.standard_normal(z.size) + 1j * rng.standard_normal(z.size)).astype(np.complex64)
    return (z + sigma * n).astype(np.complex64)


def _complex_to_iq_i32(z: np.ndarray) -> np.ndarray:
    scale = float((1 << 30) - 1)
    i = np.clip(np.round(z.real * scale), -(1 << 30), (1 << 30) - 1).astype(np.int32)
    q = np.clip(np.round(z.imag * scale), -(1 << 30), (1 << 30) - 1).astype(np.int32)
    out = np.empty(i.size * 2, dtype=np.int32)
    out[0::2] = i
    out[1::2] = q
    return out


def _build_case(case: str, duration_s: float, fdev_hz: float, rng: np.random.Generator) -> tuple[np.ndarray, np.ndarray, dict]:
    n = int(FS_IQ * duration_s)
    t = np.arange(n, dtype=np.float32) / np.float32(FS_IQ)
    audio = _audio_case(case, t)
    z = _fm_modulate(audio, FS_IQ, fdev_hz)
    noise_sigma = 0.0 if case != "noisy_two_tone" else 0.03
    z = _add_noise(z, noise_sigma, rng)

    ref_audio = audio[::DECIM].astype(np.float32, copy=False)
    ref_audio = _deemph_50us(ref_audio, FS_AUDIO)

    meta = {
        "case": case,
        "duration_s": duration_s,
        "fs_iq": FS_IQ,
        "fs_audio": FS_AUDIO,
        "decim": DECIM,
        "fdev_hz": fdev_hz,
        "noise_sigma": noise_sigma,
        "ref_peak": float(np.max(np.abs(ref_audio)) + 1e-12),
        "ref_rms": float(np.sqrt(np.mean(ref_audio * ref_audio)) + 1e-12),
    }
    return _complex_to_iq_i32(z), ref_audio, meta


def main() -> None:
    p = argparse.ArgumentParser(description="Generate FM demod prototype test vectors")
    p.add_argument("--out-dir", default="scripts/fm_demod_proto/fm_proto_data", help="Output directory")
    p.add_argument("--duration", type=float, default=5.0, help="Duration per case (s)")
    p.add_argument("--fdev", type=float, default=15_000.0, help="Frequency deviation (Hz)")
    p.add_argument("--seed", type=int, default=1337, help="RNG seed")
    args = p.parse_args()

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    rng = np.random.default_rng(args.seed)
    cases = ["single_tone", "two_tone", "noisy_two_tone"]
    manifest = []

    for case in cases:
        iq_i32, ref_audio, meta = _build_case(case, args.duration, args.fdev, rng)
        iq_path = out_dir / f"{case}_iq_i32le.bin"
        ref_path = out_dir / f"{case}_ref_audio_f32le.bin"
        meta_path = out_dir / f"{case}_meta.json"

        iq_i32.astype("<i4", copy=False).tofile(iq_path)
        ref_audio.astype("<f4", copy=False).tofile(ref_path)
        meta_path.write_text(json.dumps(meta, indent=2), encoding="utf-8")

        manifest.append(
            {
                "case": case,
                "iq_path": str(iq_path.as_posix()),
                "ref_path": str(ref_path.as_posix()),
                "meta_path": str(meta_path.as_posix()),
            }
        )
        print(f"[testgen] wrote {case}: IQ={iq_path} REF={ref_path}")

    manifest_path = out_dir / "manifest.json"
    manifest_path.write_text(json.dumps({"cases": manifest}, indent=2), encoding="utf-8")
    print(f"[testgen] manifest: {manifest_path}")


if __name__ == "__main__":
    main()
