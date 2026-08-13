"""
compare_accuracy_results.py - Phase 0.5 final comparison.

Reads board outputs (out_XXXX.bin, raw int8 [768,4,4]) + ONNX float32
references (ref_se_mul_XXXX.npy / ref_final_XXXX.npy) from
accuracy_test_imgs/, dequantizes the board output (x * output_scale,
output_scale=1/127 per quant_config.json, uniform across all layers),
and reports cosine similarity + relative L2 error against both
reference points.

用法: python compare_accuracy_results.py [--dir accuracy_test_imgs] [--n 8]
"""
import numpy as np
import argparse
import os


def cosine_sim(a, b):
    a = a.ravel().astype(np.float64)
    b = b.ravel().astype(np.float64)
    denom = np.linalg.norm(a) * np.linalg.norm(b)
    if denom < 1e-12:
        return float("nan")
    return float(np.dot(a, b) / denom)


def rel_l2_error(a, b):
    a = a.ravel().astype(np.float64)
    b = b.ravel().astype(np.float64)
    denom = np.linalg.norm(a)
    if denom < 1e-12:
        return float("nan")
    return float(np.linalg.norm(a - b) / denom)


def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("--dir", default=os.path.join(os.path.dirname(__file__), "..", "accuracy_test_imgs"))
    p.add_argument("--n", type=int, default=8)
    p.add_argument("--output_scale", type=float, default=1.0 / 127.0)
    return p.parse_args()


def main():
    args = parse_args()
    cos_se, l2_se, cos_final, l2_final, nonzero_frac = [], [], [], [], []

    for i in range(args.n):
        tag = f"{i:04d}"
        out_path = os.path.join(args.dir, f"out_{tag}.bin")
        if not os.path.exists(out_path):
            print(f"  img {i}: MISSING {out_path}, skipping")
            continue
        board_int8 = np.fromfile(out_path, dtype=np.int8).reshape(1, 768, 4, 4)
        board_float = board_int8.astype(np.float64) * args.output_scale

        ref_se = np.load(os.path.join(args.dir, f"ref_se_mul_{tag}.npy"))
        ref_final = np.load(os.path.join(args.dir, f"ref_final_{tag}.npy"))

        cs_se = cosine_sim(board_float, ref_se)
        re_se = rel_l2_error(board_float, ref_se)
        cs_fi = cosine_sim(board_float, ref_final)
        re_fi = rel_l2_error(board_float, ref_final)
        nz = float(np.count_nonzero(board_int8)) / board_int8.size

        cos_se.append(cs_se); l2_se.append(re_se)
        cos_final.append(cs_fi); l2_final.append(re_fi)
        nonzero_frac.append(nz)

        print(f"  img {i}: nonzero={nz*100:5.1f}%  "
              f"vs se_mul(no final GELU): cos={cs_se:6.3f} rel_l2={re_se:6.3f}  |  "
              f"vs true_final(with GELU): cos={cs_fi:6.3f} rel_l2={re_fi:6.3f}")

    print()
    print(f">>> mean nonzero fraction:                {np.mean(nonzero_frac)*100:.1f}%")
    print(f">>> mean cosine_sim vs se_mul (no GELU):   {np.mean(cos_se):.4f}   (matches what fastvit_infer.c actually computes)")
    print(f">>> mean rel_l2_error vs se_mul (no GELU): {np.mean(l2_se):.4f}")
    print(f">>> mean cosine_sim vs true final (+GELU): {np.mean(cos_final):.4f}   (true ONNX graph output)")
    print(f">>> mean rel_l2_error vs true final (+GELU): {np.mean(l2_final):.4f}")
    print()
    print(">>> Reference thresholds from this project's own DEPLOY_PLAN.md 5.3: cosine >= 0.99 target.")
    print(">>> NOTE: synthetic test images (no real photos in repo), held-out seed from calibration/W8A4 checks.")


if __name__ == "__main__":
    main()
