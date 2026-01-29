#!/usr/bin/env python3

import argparse
import math
import os
from dataclasses import dataclass

import numpy as np
from PIL import Image


@dataclass
class Metrics:
    mse: float
    psnr: float
    mae: float
    max_abs: int
    hist_l1_r: float
    hist_l1_g: float
    hist_l1_b: float
    mean1: tuple[float, float, float]
    mean2: tuple[float, float, float]
    std1: tuple[float, float, float]
    std2: tuple[float, float, float]


def _to_rgb_arr(path: str) -> np.ndarray:
    im = Image.open(path)
    im = im.convert("RGB")
    return np.asarray(im, dtype=np.uint8)


def _crop_to_common(a: np.ndarray, b: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    h = min(a.shape[0], b.shape[0])
    w = min(a.shape[1], b.shape[1])
    return a[:h, :w, :], b[:h, :w, :]


def _hist_l1(u8a: np.ndarray, u8b: np.ndarray) -> float:
    ha = np.bincount(u8a.reshape(-1), minlength=256).astype(np.float64)
    hb = np.bincount(u8b.reshape(-1), minlength=256).astype(np.float64)
    denom = max(1.0, float(ha.sum()))
    return float(np.abs(ha - hb).sum() / denom)


def compare_rgb(a: np.ndarray, b: np.ndarray) -> Metrics:
    a, b = _crop_to_common(a, b)

    a_i = a.astype(np.int16)
    b_i = b.astype(np.int16)
    d = a_i - b_i

    mse = float(np.mean(d.astype(np.float64) ** 2))
    mae = float(np.mean(np.abs(d).astype(np.float64)))
    max_abs = int(np.max(np.abs(d)))
    psnr = float("inf") if mse == 0.0 else 10.0 * math.log10((255.0 * 255.0) / mse)

    hr = _hist_l1(a[:, :, 0], b[:, :, 0])
    hg = _hist_l1(a[:, :, 1], b[:, :, 1])
    hb = _hist_l1(a[:, :, 2], b[:, :, 2])

    mean1 = tuple(float(x) for x in a.reshape(-1, 3).mean(axis=0))
    mean2 = tuple(float(x) for x in b.reshape(-1, 3).mean(axis=0))
    std1 = tuple(float(x) for x in a.reshape(-1, 3).std(axis=0))
    std2 = tuple(float(x) for x in b.reshape(-1, 3).std(axis=0))

    return Metrics(
        mse=mse,
        psnr=psnr,
        mae=mae,
        max_abs=max_abs,
        hist_l1_r=hr,
        hist_l1_g=hg,
        hist_l1_b=hb,
        mean1=mean1,
        mean2=mean2,
        std1=std1,
        std2=std2,
    )


def _apply_wb(rgb: np.ndarray, r_gain: float, b_gain: float) -> np.ndarray:
    out = rgb.astype(np.float32)
    out[:, :, 0] *= r_gain
    out[:, :, 2] *= b_gain
    out = np.clip(out, 0, 255).astype(np.uint8)
    return out


def _grey_world_gains(rgb: np.ndarray) -> tuple[float, float]:
    # Compute gains to match R and B means to G mean
    means = rgb.reshape(-1, 3).mean(axis=0)
    mean_r, mean_g, mean_b = float(means[0]), float(means[1]), float(means[2])
    if mean_r <= 0.0:
        r_gain = 1.0
    else:
        r_gain = mean_g / mean_r
    if mean_b <= 0.0:
        b_gain = 1.0
    else:
        b_gain = mean_g / mean_b
    return r_gain, b_gain


def _print_metrics(label: str, m: Metrics) -> None:
    print(f"{label}:")
    print(f"  mse={m.mse:.3f}  psnr={m.psnr:.2f} dB  mae={m.mae:.3f}  max_abs={m.max_abs}")
    print(
        "  hist_l1(R,G,B)=({:.4f},{:.4f},{:.4f})".format(m.hist_l1_r, m.hist_l1_g, m.hist_l1_b)
    )
    print(
        "  mean1(R,G,B)=({:.2f},{:.2f},{:.2f})  mean2(R,G,B)=({:.2f},{:.2f},{:.2f})".format(
            *m.mean1, *m.mean2
        )
    )
    print(
        "  std1(R,G,B)=({:.2f},{:.2f},{:.2f})   std2(R,G,B)=({:.2f},{:.2f},{:.2f})".format(
            *m.std1, *m.std2
        )
    )


def _swap_rb(rgb: np.ndarray) -> np.ndarray:
    out = rgb.copy()
    out[:, :, 0], out[:, :, 2] = out[:, :, 2], out[:, :, 0].copy()
    return out


def _swap_cbcr_via_ycc(path: str) -> np.ndarray:
    im = Image.open(path).convert("YCbCr")
    arr = np.asarray(im, dtype=np.uint8)
    swapped = arr.copy()
    swapped[:, :, 1], swapped[:, :, 2] = swapped[:, :, 2], swapped[:, :, 1].copy()
    return np.asarray(Image.fromarray(swapped, mode="YCbCr").convert("RGB"), dtype=np.uint8)


def save_hist_csv(rgb: np.ndarray, out_prefix: str) -> None:
    os.makedirs(os.path.dirname(out_prefix) or ".", exist_ok=True)
    for ch, name in [(0, "R"), (1, "G"), (2, "B")]:
        h = np.bincount(rgb[:, :, ch].reshape(-1), minlength=256)
        np.savetxt(f"{out_prefix}_{name}.csv", h, fmt="%d")


def main() -> int:
    ap = argparse.ArgumentParser(description="Compare JPEG outputs via RGB histograms and pixel metrics")
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--dump-hist", action="store_true", help="Write per-channel histogram CSVs")
    ap.add_argument("--out", default="hist", help="Output prefix directory/name for CSVs")
    ap.add_argument("--tune-wb", action="store_true", help="Tune R/B gains to match image a to target b")
    ap.add_argument("--auto-wb", action="store_true", help="Auto white balance image a using grey-world gains")
    ap.add_argument("--r-min", type=float, default=0.8, help="Min red gain (for --tune-wb)")
    ap.add_argument("--r-max", type=float, default=1.4, help="Max red gain (for --tune-wb)")
    ap.add_argument("--b-min", type=float, default=0.8, help="Min blue gain (for --tune-wb)")
    ap.add_argument("--b-max", type=float, default=1.6, help="Max blue gain (for --tune-wb)")
    ap.add_argument("--step", type=float, default=0.02, help="Gain step size (for --tune-wb)")
    ap.add_argument("--save-best", default="", help="Optional path to save best-matched image")
    args = ap.parse_args()

    a = _to_rgb_arr(args.a)
    b = _to_rgb_arr(args.b)

    if args.auto_wb:
        r_gain, b_gain = _grey_world_gains(a)
        adj = _apply_wb(a, r_gain, b_gain)
        print(f"auto_wb_r_gain={r_gain:.4f} auto_wb_b_gain={b_gain:.4f}")
        if args.save_best:
            Image.fromarray(adj, mode="RGB").save(args.save_best)
        return 0

    if args.tune_wb:
        a, b = _crop_to_common(a, b)
        best = None
        best_img = None
        r = args.r_min
        while r <= args.r_max + 1e-9:
            b_gain = args.b_min
            while b_gain <= args.b_max + 1e-9:
                adj = _apply_wb(a, r, b_gain)
                m = compare_rgb(adj, b)
                if best is None or m.mse < best[0].mse:
                    best = (m, r, b_gain)
                    best_img = adj
                b_gain += args.step
            r += args.step

        if best is None:
            print("No candidates evaluated.")
            return 1

        m, r_best, b_best = best
        print(f"best_r_gain={r_best:.4f} best_b_gain={b_best:.4f}")
        _print_metrics("best_match", m)
        if args.save_best:
            Image.fromarray(best_img, mode="RGB").save(args.save_best)
        return 0

    m0 = compare_rgb(a, b)
    _print_metrics("identity", m0)

    # quick diagnostics
    m_rb = compare_rgb(a, _swap_rb(b))
    _print_metrics("b: swap R<->B", m_rb)

    b_cbcr = _swap_cbcr_via_ycc(args.b)
    m_cbcr = compare_rgb(a, b_cbcr)
    _print_metrics("b: swap Cb<->Cr (via YCbCr)", m_cbcr)

    if args.dump_hist:
        save_hist_csv(a, os.path.join(args.out, "a"))
        save_hist_csv(b, os.path.join(args.out, "b"))

    best = min(
        [("identity", m0), ("swap_rb", m_rb), ("swap_cbcr", m_cbcr)],
        key=lambda t: t[1].mse,
    )
    print(f"best_by_mse={best[0]} mse={best[1].mse:.3f} psnr={best[1].psnr:.2f}dB")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
