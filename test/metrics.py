import argparse
import json
import math
import sys

import numpy as np


def load_rgb_f32(path, width, height):
    data = np.fromfile(path, dtype="<f4")
    expected = width * height * 3
    if data.size != expected:
        raise ValueError(f"{path}: expected {expected} floats, got {data.size}")
    return data.reshape((height, width, 3))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ref", required=True)
    ap.add_argument("--test", required=True)
    ap.add_argument("--width", type=int, required=True)
    ap.add_argument("--height", type=int, required=True)
    ap.add_argument("--flip", action="store_true")
    args = ap.parse_args()

    ref = load_rgb_f32(args.ref, args.width, args.height)
    test = load_rgb_f32(args.test, args.width, args.height)
    diff = test - ref
    mse = float(np.mean(diff * diff))
    rmse = math.sqrt(mse)
    peak = float(max(np.max(ref), np.max(test), 1e-6))
    psnr = float(10.0 * math.log10((peak * peak) / max(mse, 1e-20)))
    rel_abs = float(np.mean(np.abs(diff) / np.maximum(np.abs(ref), 1e-3)))
    ref_mean = np.mean(ref, axis=(0, 1))
    test_mean = np.mean(test, axis=(0, 1))
    mean_delta = np.abs(test_mean - ref_mean) / np.maximum(np.abs(ref_mean), 1e-6)
    ref_rms = float(np.sqrt(np.mean(ref * ref)))

    out = {
        "hdrMse": mse,
        "hdrRmse": rmse,
        "hdrPsnrPeak": psnr,
        "hdrPeak": peak,
        "hdrRelativeRmse": float(rmse / max(ref_rms, 1e-6)),
        "meanAbsRelative": rel_abs,
        "referenceMeanRgb": [float(v) for v in ref_mean],
        "testMeanRgb": [float(v) for v in test_mean],
        "channelMeanRelativeDelta": [float(v) for v in mean_delta],
        "maxChannelMeanRelativeDelta": float(np.max(mean_delta)),
        "flip": None,
        "flipAvailable": False,
    }

    if args.flip:
        try:
            import flip_evaluator as flip

            _error_map, mean_flip, parameters = flip.evaluate(ref, test, "HDR")
            out["flip"] = float(mean_flip)
            out["flipAvailable"] = True
            out["flipParameters"] = {
                k: (float(v) if isinstance(v, float) else v) for k, v in parameters.items()
            }
        except Exception as exc:
            out["flipError"] = str(exc)

    json.dump(out, sys.stdout)


if __name__ == "__main__":
    main()
