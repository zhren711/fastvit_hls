"""
diagnose_entry_by_entry.py -- A2/Phase C bisection (ZHR-92, 2026-08-21):
bit-exact Python re-implementation of mac_array.cpp's actual algorithm
(not an idealized approximation -- a FAITHFUL translation: per-channel
out_shift, truncating right-shift exactly matching clip_shift, GELU's
real crude placeholder with shift=7, Add with shift=0, same as
mac_array.cpp literally does), run on the SAME real weights/shift-tables/
image as the real hardware, entry by entry (0-16, stem.1 through
stage1), compared against real per-entry dumps (mac_array_ckpt_dump.cpp,
extended this round to dump entries 0-16, not just the 6 checkpoints).

Vectorized with numpy (same im2col-style technique validated in
compute_stem_arm.py, cross-checked there against a naive triple loop,
35/35 exact) -- exact int64 arithmetic throughout, not floating point,
just avoiding a slow pure-Python nested loop.

Also tracks each entry's accumulator range BEFORE the shift, checked
against acc_t's real ap_int<32> range -- directly answers suspect #1
(int32 accumulator overflow) without guessing.

用法:
  python diagnose_entry_by_entry.py
"""
import json
import numpy as np
import os

FASTVIT_DIR = r"E:\codes\microzed\fastvit_hls"
REF_DIR = os.path.join(FASTVIT_DIR, "accuracy_test_imgs_256")
FASTVIT_IP_DIR = os.path.join(FASTVIT_DIR, "fastvit_ip_v2")

ACC_MIN, ACC_MAX = -(2**31), 2**31 - 1  # ap_int<32> range


def clip_shift_vec(acc, shift_per_elem):
    """acc: int64 ndarray: shift_per_elem: int64 ndarray, same shape
    (broadcastable). Exact match to C++ ap_int's signed arithmetic right
    shift (numpy's >> on signed integer dtypes performs the same
    sign-extending floor shift as C++ for int64)."""
    v = acc >> shift_per_elem
    return np.clip(v, -128, 127)


def dwconv(act, in_off, out_off, cin, fpg, h_in, w_in, K, S, P, w_flat, w_off, b_flat, b_off, shift_table, sh_off):
    h_out = (h_in + 2 * P - K) // S + 1
    w_out = h_out
    cout = cin * fpg
    x = act[in_off:in_off + cin * h_in * w_in].reshape(cin, h_in, w_in)
    x_pad = np.zeros((cin, h_in + 2 * P, w_in + 2 * P), dtype=np.int64)
    x_pad[:, P:P + h_in, P:P + w_in] = x

    out = np.zeros((cout, h_out, w_out), dtype=np.int64)
    acc_all = np.zeros((cout, h_out, w_out), dtype=np.int64)
    for f in range(fpg):
        oc_list = np.arange(cin) * fpg + f  # oc per input channel c
        w_this = w_flat[w_off:w_off + cin * fpg * K * K].astype(np.int64).reshape(cin * fpg, K, K)[oc_list]
        bias_this = b_flat[b_off:b_off + cin * fpg].astype(np.int64)[oc_list]
        acc = np.zeros((cin, h_out, w_out), dtype=np.int64)
        for kh in range(K):
            for kw in range(K):
                patch = x_pad[:, kh:kh + S * h_out:S, kw:kw + S * w_out:S]
                acc += patch * w_this[:, kh, kw][:, None, None]
        acc += bias_this[:, None, None]
        acc_all[oc_list] = acc
        shift = shift_table[sh_off:sh_off + cout].astype(np.int64)[oc_list]
        out[oc_list] = clip_shift_vec(acc, shift[:, None, None])
    return out.reshape(-1), acc_all.reshape(-1)


def pwconv(act, in_off, out_off, cin, cout, h_in, w_in, w_flat, w_off, b_flat, b_off, shift_table, sh_off):
    x = act[in_off:in_off + cin * h_in * w_in].reshape(cin, h_in, w_in)
    w = w_flat[w_off:w_off + cout * cin].astype(np.int64).reshape(cout, cin)
    bias = b_flat[b_off:b_off + cout].astype(np.int64)
    acc = np.einsum('ihw,oi->ohw', x, w) + bias[:, None, None]
    shift = shift_table[sh_off:sh_off + cout].astype(np.int64)
    out = clip_shift_vec(acc, shift[:, None, None])
    return out.reshape(-1), acc.reshape(-1)


def quantized_sigmoid_vec(x):
    v = x + 64
    return np.clip(v, 0, 127)


def main():
    with open(os.path.join(REF_DIR, "shift_table_meta.json")) as f:
        shift_meta = json.load(f)
    with open(os.path.join(FASTVIT_DIR, "tools", "layer_hw_sequence_256.json")) as f:
        hwdata = json.load(f)
    seq = hwdata["sequence"]
    with open(os.path.join(FASTVIT_DIR, "tools", "layer_descriptor_256.json")) as f:
        descriptor = json.load(f)
    by_layer_idx = {e["layer_idx"]: e for e in descriptor}
    with open(os.path.join(FASTVIT_DIR, "weights_t8_gamma_folded", "quant_config.json")) as f:
        qcfg = json.load(f)

    w_flat = np.fromfile(os.path.join(FASTVIT_IP_DIR, "ckpt_weights_flat.bin"), dtype=np.int8)
    b_flat = np.fromfile(os.path.join(FASTVIT_IP_DIR, "ckpt_bias_flat.bin"), dtype=np.int32)

    w_off_by_layer, b_off_by_layer = {}, {}
    w_cursor, b_cursor = 0, 0
    for e in descriptor:
        if e["layer_idx"] == 0:
            continue
        tag = e["tag"]
        qc = qcfg[tag]
        w_off_by_layer[e["layer_idx"]] = w_cursor
        w_cursor += os.path.getsize(os.path.join(FASTVIT_DIR, "weights_t8_gamma_folded", qc["weight_file"]))
        b_off_by_layer[e["layer_idx"]] = b_cursor
        b_cursor += os.path.getsize(os.path.join(FASTVIT_DIR, "weights_t8_gamma_folded", qc["bias_file"])) // 4

    shift_off_by_layer = shift_meta["shift_off_by_layer"]
    shift_table = np.fromfile(os.path.join(REF_DIR, "shift_table_flat.bin"), dtype=np.int8)

    TOTAL_BYTES = 1819392
    act = np.zeros(TOTAL_BYTES, dtype=np.int64)
    stem_out = np.fromfile(os.path.join(REF_DIR, "stem_output_0000.bin"), dtype=np.int8)
    act[:stem_out.size] = stem_out.astype(np.int64)

    print(f"{'entry':6s} {'kind':8s} {'max_mismatch':>13s} {'n_mismatch':>12s} {'acc_range':>24s}")
    print("-" * 75)

    first_mismatch = None
    for i, e in enumerate(seq):
        if i > 16:
            break
        kind = e["kind"]
        acc_min_seen, acc_max_seen = None, None

        if kind == "conv":
            d = by_layer_idx[e["layer_idx"]]
            op_type = d["op_type"]
            cin, cout = d["cin"], d["cout"]
            h_in, w_in = d["h_in"], d["w_in"]
            K, S, P = d["k"], d["stride"], d["pad"]
            fpg = d["fpg"]
            w_off = w_off_by_layer[e["layer_idx"]]
            b_off = b_off_by_layer[e["layer_idx"]]
            sh_off = shift_off_by_layer[str(e["layer_idx"])]
            in_off, out_off = e["in_off"], e["out_off"]

            if op_type == "dwconv":
                out, acc_all = dwconv(act, in_off, out_off, cin, fpg, h_in, w_in, K, S, P,
                                       w_flat, w_off, b_flat, b_off, shift_table, sh_off)
            else:
                out, acc_all = pwconv(act, in_off, out_off, cin, cout, h_in, w_in,
                                       w_flat, w_off, b_flat, b_off, shift_table, sh_off)
            act[out_off:out_off + out.size] = out
            acc_min_seen, acc_max_seen = int(acc_all.min()), int(acc_all.max())
            size = out.size

        elif kind == "gelu":
            cin_, h_, w_ = e["cin"], e["h"], e["w"]
            total = cin_ * h_ * w_
            in_off, out_off = e["in_off"], e["out_off"]
            x = act[in_off:in_off + total]
            gate = quantized_sigmoid_vec(x)
            prod = x * gate
            out = clip_shift_vec(prod, np.int64(7))
            act[out_off:out_off + total] = out
            size = total

        elif kind == "add":
            cin_, h_, w_ = e["cin"], e["h"], e["w"]
            total = cin_ * h_ * w_
            op0, op1, out_off = e["op0_off"], e["op1_off"], e["out_off"]
            s = act[op0:op0 + total] + act[op1:op1 + total]
            out = clip_shift_vec(s, np.int64(0))
            act[out_off:out_off + total] = out
            size = total

        else:
            raise ValueError(f"entry {i}: unexpected kind {kind}")

        hw_dump = np.fromfile(os.path.join(REF_DIR, f"entry_{i:02d}.bin"), dtype=np.int8)
        py_out = act[e["out_off"]:e["out_off"] + size].astype(np.int8)
        mismatch = py_out != hw_dump
        n_mismatch = int(mismatch.sum())
        max_mismatch = int(np.max(np.abs(py_out.astype(np.int32) - hw_dump.astype(np.int32)))) if size else 0

        acc_str = f"[{acc_min_seen},{acc_max_seen}]" if acc_min_seen is not None else "n/a"
        of_flag = ""
        if acc_min_seen is not None and (acc_min_seen < ACC_MIN or acc_max_seen > ACC_MAX):
            of_flag = " ACC_T OVERFLOW"
        print(f"{i:<6d} {kind:8s} {max_mismatch:13d} {n_mismatch:6d}/{size:<6d} {acc_str}{of_flag}")

        if n_mismatch > 0 and first_mismatch is None:
            first_mismatch = i

    print()
    if first_mismatch is None:
        print(">>> Python bit-exact simulation matches real hardware EXACTLY for all entries 0-16.")
    else:
        print(f">>> FIRST DIVERGENCE at entry {first_mismatch}.")


if __name__ == "__main__":
    main()
