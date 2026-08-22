"""
compute_ckpt_table.py -- A2 exit measurement (ZHR-92, 2026-08-21): the
segmented cosine + rel L2 table, hardware (int8, dequantized at each
checkpoint's own CALIBRATED scale -- no longer a uniform 1/127 placeholder,
see gen_shift_tables.py's scale_at_seq_index) vs. float32 ONNX reference,
all 7 checkpoints. Judged by SHAPE: smooth decay = expected quantization
error accumulation; a cliff = a real implementation defect at that stage.
Stem's anchor bar is ~0.9855 (closed 2026-08-21 with N=16 real-image batch
calibration + per-channel out_shift -- not 1.0, the residual gap traced to
one low-weight-scale channel, ZHR-92), not the uncalibrated placeholder's
~0.76.

用法:
  python compute_ckpt_table.py
"""
import numpy as np
import json
import os

REF_DIR = r"E:\codes\microzed\fastvit_hls\accuracy_test_imgs_256"

CHECKPOINTS = ["stem", "stage1", "stage2", "stage3", "stage4", "finaldw", "se"]
SHAPES = {
    "stem":    (48, 128, 128),
    "stage1":  (48, 64, 64),
    "stage2":  (96, 32, 32),
    "stage3":  (192, 16, 16),
    "stage4":  (384, 8, 8),
    "finaldw": (768, 8, 8),
    "se":      (768, 8, 8),
}
# seq_index each checkpoint's dequant scale comes from (gen_ckpt_harness.py's
# own checkpoint map -- see its printed "seq_index=" lines)
SEQ_INDEX = {"stage1": 16, "stage2": 31, "stage3": 58, "stage4": 73, "finaldw": 74, "se": 80}


def cosine_sim(a, b):
    a = a.ravel().astype(np.float64)
    b = b.ravel().astype(np.float64)
    denom = np.linalg.norm(a) * np.linalg.norm(b)
    if denom < 1e-12:
        return float("nan")
    return float(np.dot(a, b) / denom)


def rel_l2(a, b):
    a = a.ravel().astype(np.float64)
    b = b.ravel().astype(np.float64)
    denom = np.linalg.norm(b)
    if denom < 1e-12:
        return float("nan")
    return float(np.linalg.norm(a - b) / denom)


def main():
    with open(os.path.join(REF_DIR, "shift_table_meta.json")) as f:
        meta = json.load(f)

    scales = {"stem": meta["stem_output_scale"]}
    for tag, idx in SEQ_INDEX.items():
        scales[tag] = meta["scale_at_seq_index"][str(idx)]

    print(f"{'checkpoint':10s} {'cosine':>8s} {'rel_L2':>8s} {'scale':>10s}   shape")
    print("-" * 62)
    results = {}
    for tag in CHECKPOINTS:
        ref = np.load(os.path.join(REF_DIR, f"ckpt_ref_{tag}_0000.npy"))

        if tag == "stem":
            hw_int8 = np.fromfile(os.path.join(REF_DIR, "stem_output_0000.bin"), dtype=np.int8)
        else:
            hw_int8 = np.fromfile(os.path.join(REF_DIR, f"ckpt_hw_{tag}_0000.bin"), dtype=np.int8)

        expected_size = int(np.prod(SHAPES[tag]))
        assert hw_int8.size == expected_size, f"{tag}: hw dump size {hw_int8.size} != expected {expected_size}"
        assert ref.size == expected_size, f"{tag}: ref size {ref.size} != expected {expected_size}"

        scale = scales[tag]
        hw_float = hw_int8.astype(np.float64) * scale
        ref_flat = ref.ravel()

        cs = cosine_sim(hw_float, ref_flat)
        rl2 = rel_l2(hw_float, ref_flat)
        results[tag] = (cs, rl2)
        print(f"{tag:10s} {cs:8.4f} {rl2:8.4f} {scale:10.6f}   {SHAPES[tag]}")

    print()
    print(">>> judgment reminders:")
    print("    stem anchor bar ~0.9855 (calibrated, ZHR-92 2026-08-21) -- not 1.0, not the old ~0.76")
    print("    smooth decay after stem = expected quantization error accumulation (not a failure)")
    print("    a cliff at one stage = a real implementation defect there, worth chasing")
    print("    unexpectedly HIGH values anywhere = verify the reference isn't mismatched, not a win")


if __name__ == "__main__":
    main()
