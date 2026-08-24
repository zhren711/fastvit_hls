#!/usr/bin/env python3
"""
Compare fresh board checkpoint dumps from mac_array_full_network_test against
both the established csim reference (ckpt_hw_*) and the ONNX float32 reference
(ckpt_ref_*.npy), using the per-checkpoint dequant scales in shift_table_meta.json.

Usage: python tools/compare_board_full_network_ckpts.py [board_dump_dir]
"""
import sys
import json
import numpy as np

REF_DIR = "accuracy_test_imgs_256"
BOARD_DIR = sys.argv[1] if len(sys.argv) > 1 else f"{REF_DIR}/board_test_full_network"

SEQ_INDEX = {
    "stage1": 16, "stage2": 31, "stage3": 58,
    "stage4": 73, "finaldw": 74, "se": 80,
}
TAGS = ["stage1", "stage2", "stage3", "stage4", "finaldw", "se"]


def cosine_sim(a, b):
    a = a.ravel().astype(np.float64)
    b = b.ravel().astype(np.float64)
    denom = np.linalg.norm(a) * np.linalg.norm(b)
    return float(np.dot(a, b) / denom) if denom > 1e-12 else float("nan")


def main():
    with open(f"{REF_DIR}/shift_table_meta.json") as f:
        meta = json.load(f)

    print(f"{'tag':10s} {'csim_mism':>10s} {'csim_cos':>10s} {'onnx_cos':>10s}")
    for t in TAGS:
        board = np.fromfile(f"{BOARD_DIR}/ckpt_board_{t}.bin", dtype=np.int8)

        # vs csim (fixed-point, exact match expected)
        hw_ref = np.fromfile(f"{REF_DIR}/ckpt_hw_{t}_0000.bin", dtype=np.int8)
        mism = int(np.sum(board != hw_ref)) if board.size == hw_ref.size else -1
        csim_cos = cosine_sim(board, hw_ref) if board.size == hw_ref.size else float("nan")

        # vs ONNX float32 reference (dequantized via per-checkpoint scale)
        onnx_ref = np.load(f"{REF_DIR}/ckpt_ref_{t}_0000.npy")
        scale = meta["scale_at_seq_index"][str(SEQ_INDEX[t])]
        board_f = board.astype(np.float64) * scale
        onnx_cos = cosine_sim(board_f, onnx_ref.ravel())

        print(f"{t:10s} {mism:10d} {csim_cos:10.6f} {onnx_cos:10.4f}")


if __name__ == "__main__":
    main()
