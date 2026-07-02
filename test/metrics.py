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

    out = {
        "hdrMse": mse,
        "hdrRmse": rmse,
        "hdrPsnrPeak": psnr,
        "hdrPeak": peak,
        "meanAbsRelative": rel_abs,
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
