#!/usr/bin/env python3
"""Validate fm_demod_proto outputs against generated references.

Expected files under --data-dir:
  <case>_ref_audio_f32le.bin
  <case>_out.wav
  <case>_report.txt
"""

from __future__ import annotations

import argparse
import json
import wave
from pathlib import Path

import numpy as np


def _read_wav_i16(path: Path) -> np.ndarray:
    with wave.open(str(path), "rb") as wf:
        if wf.getnchannels() != 1 or wf.getsampwidth() != 2:
            raise ValueError(f"Unexpected WAV format in {path}")
        pcm = wf.readframes(wf.getnframes())
    return np.frombuffer(pcm, dtype="<i2").astype(np.float32) / 32768.0


def _norm_corr(a: np.ndarray, b: np.ndarray) -> float:
    n = min(a.size, b.size)
    if n < 16:
        return 0.0
    a = a[:n].astype(np.float64, copy=False)
    b = b[:n].astype(np.float64, copy=False)
    a -= np.mean(a)
    b -= np.mean(b)
    da = np.linalg.norm(a)
    db = np.linalg.norm(b)
    if da < 1e-12 or db < 1e-12:
        return 0.0
    return float(np.dot(a, b) / (da * db))


def _read_report(path: Path) -> dict[str, int]:
    out = {}
    if not path.exists():
        return out
    for line in path.read_text(encoding="utf-8").splitlines():
        if "=" not in line:
            continue
        k, v = line.split("=", 1)
        try:
            out[k.strip()] = int(v.strip())
        except ValueError:
            continue
    return out


def main() -> None:
    p = argparse.ArgumentParser(description="Validate fixed-point C FM demod outputs")
    p.add_argument("--data-dir", default="scripts/fm_demod_proto/fm_proto_data")
    p.add_argument("--corr-clean-threshold", type=float, default=0.90)
    p.add_argument("--corr-noisy-threshold", type=float, default=0.80)
    args = p.parse_args()

    data_dir = Path(args.data_dir)
    cases = ["single_tone", "two_tone", "noisy_two_tone"]
    results = []

    overall_pass = True
    for case in cases:
        ref_path = data_dir / f"{case}_ref_audio_f32le.bin"
        wav_path = data_dir / f"{case}_out.wav"
        rep_path = data_dir / f"{case}_report.txt"

        if not ref_path.exists() or not wav_path.exists():
            results.append({"case": case, "pass": False, "error": "missing_ref_or_wav"})
            overall_pass = False
            continue

        ref = np.fromfile(ref_path, dtype="<f4")
        out = _read_wav_i16(wav_path)
        corr = _norm_corr(ref, out)
        report = _read_report(rep_path)

        sat_count = int(report.get("sat_count", 0))
        den_guard = int(report.get("den_guard_count", 0))
        out_samples = int(report.get("output_samples", out.size))

        corr_thresh = args.corr_noisy_threshold if case == "noisy_two_tone" else args.corr_clean_threshold
        case_pass = corr >= corr_thresh and sat_count < max(8, out_samples // 2000)
        overall_pass = overall_pass and case_pass

        results.append(
            {
                "case": case,
                "pass": bool(case_pass),
                "corr": corr,
                "corr_threshold": corr_thresh,
                "sat_count": sat_count,
                "den_guard_count": den_guard,
                "samples_ref": int(ref.size),
                "samples_out": int(out.size),
            }
        )
        print(
            f"[validate] {case}: pass={case_pass} corr={corr:.4f} "
            f"sat={sat_count} den_guard={den_guard}"
        )

    summary = {"overall_pass": overall_pass, "results": results}
    out_json = data_dir / "validation_summary.json"
    out_json.write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(f"[validate] summary: {out_json} overall_pass={overall_pass}")

    raise SystemExit(0 if overall_pass else 1)


if __name__ == "__main__":
    main()
