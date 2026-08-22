"""
gen_ckpt_harness.py -- A2 exit measurement (ZHR-92, 2026-08-21): turn the
real 82-entry hardware sequence (layer_hw_sequence_256.json) into
something a C++ csim harness can actually run --

  1. A flat weight buffer (int8) + flat bias buffer (int32), concatenating
     the real quantized weights for the 51 hardware-dispatched conv
     layers (Stem, layer_idx=0, is excluded -- computed ARM-side, see
     tools/compute_stem_arm.py -- its weight is never loaded here).
  2. A C++ header with the 82-entry LayerDescV2 array, brace-initialized
     through in2_off (matching every existing descriptor in
     mac_array_tb.cpp); h_out/w_out/tile fields are filled at runtime by
     derive_mac_array_params(), same convention as every other test.
  3. A checkpoint map: for each of the 6 hardware-side checkpoints
     (stem is excluded -- it's the ARM's own output, not a hardware
     sequence position), the (sequence_index, out_off, size, tag) to dump
     after running that entry.

out_shift for the synthetic (non-conv) ops is NOT a free parameter here --
each is derived from the uniform default_act_scale=1/127 placeholder
already confirmed (2026-08-21) to be the SAME input_scale=output_scale
for all 52 conv layers:
  - ADD: 0. Both operands already share scale S; (a_real+b_real) =
    (a_int+b_int)*S exactly, no shift needed, no error introduced.
  - GAP: 0. avg = sum/HW is already an exact average at scale S.
  - GELU / SCALE: 7. Both multiply an activation at scale S=1/127 by a
    sigmoid-family gate in raw range [0,127] representing a 0..1 fraction
    (gate_real = gate_int/127) -- out_int = x_int*gate_int/127, and 2^7=128
    is the closest power-of-2 shift to dividing by 127.
  - RELU / SIGMOID: unused by their own implementation (no clip_shift
    call) -- set to 0 for cleanliness, value is inert.

用法:
  python gen_ckpt_harness.py [--out-dir <dir>]
"""
import json
import os
import argparse

LDESC_OP = {
    "dwconv": 0, "pwconv": 1, "add": 2, "gap": 3,
    "relu": 4, "sigmoid": 5, "scale": 6, "gelu": 7,
}

SYNTHETIC_OUT_SHIFT = {"add": 0, "gap": 0, "gelu": 7, "scale": 7, "relu": 0, "sigmoid": 0}

CHECKPOINTS_HW = ["stage1", "stage2", "stage3", "stage4", "finaldw", "se"]
LAST_CONV_TAG = {
    "stage1":  "layer_0010_pwconv",
    "stage2":  "layer_0020_pwconv",
    "stage3":  "layer_0038_pwconv",
    "stage4":  "layer_0048_pwconv",
    "finaldw": "layer_0049_dwconv",
}


def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("--descriptor", default=r"E:\codes\microzed\fastvit_hls\tools\layer_descriptor_256.json")
    p.add_argument("--hw-seq", default=r"E:\codes\microzed\fastvit_hls\tools\layer_hw_sequence_256.json")
    p.add_argument("--weights", default=r"E:\codes\microzed\fastvit_hls\weights_t8_gamma_folded")
    p.add_argument("--shift-table", default=r"E:\codes\microzed\fastvit_hls\accuracy_test_imgs_256\shift_table_flat.bin")
    p.add_argument("--shift-meta", default=r"E:\codes\microzed\fastvit_hls\accuracy_test_imgs_256\shift_table_meta.json")
    p.add_argument("--out-dir", default=r"E:\codes\microzed\fastvit_hls\fastvit_ip_v2")
    return p.parse_args()


def main():
    args = parse_args()
    with open(args.descriptor) as f:
        descriptor = json.load(f)
    with open(args.hw_seq) as f:
        hwdata = json.load(f)
    seq = hwdata["sequence"]
    with open(os.path.join(args.weights, "quant_config.json")) as f:
        qcfg = json.load(f)

    by_layer_idx = {e["layer_idx"]: e for e in descriptor}
    by_tag = {e["tag"]: e for e in descriptor}

    # ---- flat weight/bias buffers (51 real conv layers, Stem excluded) ----
    w_bytes = bytearray()
    b_words = []  # int32
    w_off_by_layer_idx = {}
    b_off_by_layer_idx = {}
    for e in descriptor:
        if e["layer_idx"] == 0:
            continue  # Stem -- ARM-side, no weight load here
        tag = e["tag"]
        qc = qcfg[tag]
        with open(os.path.join(args.weights, qc["weight_file"]), "rb") as f:
            wbytes = f.read()
        w_off_by_layer_idx[e["layer_idx"]] = len(w_bytes)
        w_bytes += wbytes

        import numpy as np
        bwords = np.fromfile(os.path.join(args.weights, qc["bias_file"]), dtype=np.int32)
        b_off_by_layer_idx[e["layer_idx"]] = len(b_words)
        b_words.extend(bwords.tolist())

    # per-channel shift table (ZHR-92, 2026-08-21) appended right after the
    # weight data, in the SAME flat buffer -- shift_off is an element
    # offset into w_base per mac_array.h's LayerDescV2.shift_off, reusing
    # the existing array rather than adding a new mac_array_top parameter.
    with open(args.shift_table, "rb") as f:
        shift_bytes = f.read()
    with open(args.shift_meta) as f:
        shift_meta = json.load(f)
    shift_table_base = len(w_bytes)
    w_bytes += shift_bytes
    print(f">>> appended shift table: {len(shift_bytes)} bytes at w_base offset {shift_table_base}")

    weight_bin_path = os.path.join(args.out_dir, "ckpt_weights_flat.bin")
    bias_bin_path = os.path.join(args.out_dir, "ckpt_bias_flat.bin")
    with open(weight_bin_path, "wb") as f:
        f.write(w_bytes)
    import numpy as np
    np.array(b_words, dtype=np.int32).tofile(bias_bin_path)
    print(f">>> wrote {weight_bin_path} ({len(w_bytes)} bytes)")
    print(f">>> wrote {bias_bin_path} ({len(b_words)} int32 words)")

    # ---- descriptor array ----
    lines = []
    lines.append("#pragma once")
    lines.append('#include "mac_array.h"')
    lines.append("")
    lines.append(f"static const int N_HW_SEQ = {len(seq)};")
    lines.append("static LayerDescV2 g_hw_seq[N_HW_SEQ] = {")

    checkpoint_map = []  # (tag, seq_index, out_off)
    shift_lines = []

    for idx, e in enumerate(seq):
        kind = e["kind"]
        if kind == "conv":
            d = by_layer_idx[e["layer_idx"]]
            tag = d["tag"]
            op_type = LDESC_OP[d["op_type"]]
            cin, cout = d["cin"], d["cout"]
            h_in, w_in = d["h_in"], d["w_in"]
            k, stride, pad, fpg = d["k"], d["stride"], d["pad"], d["fpg"]
            out_shift = 0  # unused when use_shift_table=1, kept at 0 for clarity, not left stale
            in_off, out_off = e["in_off"], e["out_off"]
            w_off, b_off = w_off_by_layer_idx[e["layer_idx"]], b_off_by_layer_idx[e["layer_idx"]]
            in2_off = 0
            use_shift_table = 1
            shift_off = shift_table_base + shift_meta["shift_off_by_layer"][str(e["layer_idx"])]
            comment = f"conv layer_idx={e['layer_idx']} tag={tag}"
            for cktag, ck_last_tag in LAST_CONV_TAG.items():
                if tag == ck_last_tag:
                    checkpoint_map.append((cktag, idx, None))  # out_off resolved after add-lookahead below
        else:
            op_type = LDESC_OP[kind]
            # the 'scale' entry (SE's final gate multiply) is built from
            # final_conv_shape in gen_hw_sequence.py, which uses
            # cout/h_out/w_out keys instead of the cin/h/w every other
            # synthetic op uses -- fall back to those, or this silently
            # resolves to cin=0 (found by inspection: entry 80 printed
            # cin=0/cout=0 and dumped a 0-byte 'se' checkpoint, since
            # run_scale's `for(c<d.cin)` loop then never executes at all).
            cin = e.get("cin", e.get("cout", 0))
            h_in = e.get("h", e.get("h_out", 0))
            w_in = e.get("w", e.get("w_out", 0))
            cout = cin
            k, stride, pad, fpg = 1, 1, 0, 1
            out_shift = SYNTHETIC_OUT_SHIFT[kind]
            w_off, b_off = 0, 0
            use_shift_table, shift_off = 0, 0
            if kind in ("add", "scale"):
                in_off, in2_off = e["op0_off"], e["op1_off"]
            else:
                in_off, in2_off = e["in_off"], 0
            out_off = e["out_off"]
            comment = f"{kind}"
            if kind == "scale":
                checkpoint_map.append(("se", idx, None))

        lines.append(
            f"    {{ {op_type}, {cin},{cout}, {h_in},{w_in}, {k},{stride},{pad}, {fpg}, {out_shift}, "
            f"{in_off},{w_off},{b_off},{out_off}, {in2_off} }}, // [{idx}] {comment}"
        )
        shift_lines.append(f"g_hw_seq[{idx}].use_shift_table = {use_shift_table}; "
                            f"g_hw_seq[{idx}].shift_off = {shift_off};")

    lines.append("};")
    lines.append("")
    lines.append("// use_shift_table/shift_off sit 8 fields after in2_off in the struct")
    lines.append("// (past all the host-computed h_out/tile fields) -- positional init can't")
    lines.append("// reach them without also fixing every field in between, so set by name,")
    lines.append("// same convention this codebase already uses for in2_off elsewhere.")
    lines.append("static void set_shift_table_fields() {")
    lines.extend(f"    {s}" for s in shift_lines)
    lines.append("}")

    # resolve stage checkpoints: the entry AFTER the matching conv (the
    # 'add' that combines it with the pending residual) -- see
    # gen_hw_sequence.py's build_hw_sequence for why this is always
    # exactly the next entry when the conv's own kind was 'mul'.
    resolved_ckpts = []
    for cktag, idx, _ in checkpoint_map:
        if cktag in ("stage1", "stage2", "stage3", "stage4"):
            add_idx = idx + 1
            assert seq[add_idx]["kind"] == "add", (
                f"{cktag}: expected entry {add_idx} to be 'add' (the block's residual "
                f"combine), got {seq[add_idx]['kind']} -- hw sequence structure changed, "
                f"re-derive this checkpoint mapping"
            )
            resolved_ckpts.append((cktag, add_idx, seq[add_idx]["out_off"]))
        elif cktag == "finaldw":
            resolved_ckpts.append((cktag, idx, seq[idx]["out_off"]))
        elif cktag == "se":
            resolved_ckpts.append((cktag, idx, seq[idx]["out_off"]))

    lines.append("")
    lines.append("struct CkptEntry { const char* tag; int seq_index; int out_off; int size; };")
    lines.append(f"static const int N_CKPT = {len(resolved_ckpts)};")
    lines.append("static CkptEntry g_ckpts[N_CKPT] = {")
    for cktag, idx, out_off in resolved_ckpts:
        e = seq[idx]
        if e["kind"] == "conv":
            size = e["cout"] * e["h_out"] * e["w_out"]
        else:
            # same cout/h_out/w_out fallback as above -- 'scale' entries
            # (the only consumer of this branch among checkpoints) use
            # those key names, not cin/h/w.
            c = e.get("cin", e.get("cout", 0))
            hh = e.get("h", e.get("h_out", 1))
            ww = e.get("w", e.get("w_out", 1))
            size = c * hh * ww
        lines.append(f'    {{ "{cktag}", {idx}, {out_off}, {size} }},')
    lines.append("};")
    lines.append("")

    header_path = os.path.join(args.out_dir, "mac_array_ckpt_desc.h")
    with open(header_path, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f">>> wrote {header_path} ({len(seq)} descriptor entries, {len(resolved_ckpts)} checkpoints)")
    for cktag, idx, out_off in resolved_ckpts:
        print(f"    {cktag:10s} seq_index={idx:3d} out_off={out_off}")


if __name__ == "__main__":
    main()
