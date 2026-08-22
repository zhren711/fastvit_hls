"""
compute_stem_arm.py -- Route C (ZHR-92, 2026-08-21): Stem's plain conv
(layer_0000_conv, K=3 S=2, cin=3->cout=48, group=1) computed off-chip,
standing in for what the real ARM driver will do. Chosen over Route B
(3x DW fpg=48 + 2x Add) specifically because Route B forces an int8
round-trip on each of 3 partial sums before combining -- each partial's
own dynamic range is only ~1/3 of what the calibrated out_shift=8 assumes,
losing low-order bits and risking single-channel saturation the correct
single-pass sum wouldn't hit. This script does the reduction in one pass
with a wide (Python int, unbounded -- stands in for the ARM's int32)
accumulator, matching hardware's clip_shift exactly (int64 accumulate,
arithmetic right-shift, clamp to int8) so the result is bit-identical to
what a correct single-pass int32 ARM implementation would produce -- zero
precision loss beyond the single out_shift quantization step every other
layer in this network already takes once.

This is NOT the real ARM C driver (that's Phase A3's board-bring-up work)
-- it's the reference computation this round needs to (a) produce Stem's
real output for the A2 exit cosine table's hardware side, and (b) serve
as the known-correct prototype for that future C implementation.

--input-scale (2026-08-21, A2 exit measurement follow-up): quant_config's
stored out_shift assumes input_scale==output_scale==1/127 (see
export_weights.py's compute_out_shift -- the formula drops input_scale
entirely because it cancels when input_scale==output_scale; the function
signature keeps the parameter but never uses it). The measured real image
range is [-1.434, 1.748] -- at scale=1/127 (i.e. dividing by ~1/127 to
quantize) that saturates 14.1% of pixels, which is IN SCOPE for A2 (it
corrupts the very anchor checkpoint the exit table's judgment criteria
depend on), not Phase C's full 52-layer calibration (a separate,
independent workflow measuring real per-layer dynamic ranges from a real
image batch). When --input-scale differs from the fixed output_scale=1/127
(unchanged -- everything downstream of Stem still expects it), out_shift
must be re-derived: output_int8 = acc_int32 * weight_scale *
(input_scale/output_scale), i.e. the naive quant_config out_shift is
wrong by log2(input_scale/output_scale) once the two scales diverge.

用法:
  python compute_stem_arm.py [--image <img_NNNN.bin>] [--weights <dir>] [--out <bin>]
                              [--input-scale <float>]
"""
import numpy as np
import json
import argparse
import os

PLACEHOLDER_SCALE = 1.0 / 127.0  # the original uniform default_act_scale every weight/bias
                                  # file on disk was quantized against -- a fact about the
                                  # export, not a target; do not confuse with --output-scale


def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("--image", default=r"E:\codes\microzed\fastvit_hls\accuracy_test_imgs_256\img_0000.bin")
    p.add_argument("--weights", default=r"E:\codes\microzed\fastvit_hls\weights_t8_gamma_folded")
    p.add_argument("--out", default=r"E:\codes\microzed\fastvit_hls\accuracy_test_imgs_256\stem_output_0000.bin")
    p.add_argument("--input-scale", type=float, default=1.0 / 127.0,
                    help="must match whatever scale --image was quantized with")
    p.add_argument("--output-scale", type=float, default=None,
                    help="Stem's own output scale (per-layer, shared with whatever reads Stem's "
                         "output downstream -- see calibrate_stem_256.py). Defaults to the fixed "
                         "1/127 placeholder if omitted.")
    p.add_argument("--shift-table-out", default=None,
                    help="A3 board bring-up (ZHR-92, 2026-08-21): dump the per-channel shift "
                         "array (int32, Cout=48 entries) and the rescaled int32 bias to sibling "
                         "*_shift.bin / *_bias_rescaled.bin files next to this path, so the "
                         "ARM-side C port (petalinux/software/fastvit_app/src/stem_arm.c) can do "
                         "the conv+shift purely in integer arithmetic -- log2/weight_scale stay "
                         "host-side, same division of labor as every other layer's shift table.")
    return p.parse_args()


def compute_shift_per_channel(input_scale, output_scale, weight_scale_arr):
    """Re-derivation of export_weights.py's compute_out_shift, generalized
    to input_scale != output_scale (the original assumes they're equal and
    the ratio cancels -- see module docstring). Returns the PER-CHANNEL
    array, not the mean -- 2026-08-21 finding (ZHR-92): weight_scale spans
    396x across Stem's 48 channels (network-wide median 43.7x, worst layer
    5507x), so averaging into one shared shift saturates roughly half the
    channels and wastes precision on the rest. Confirmed distinctly from
    the earlier input-scale/bias fixes, which were real but couldn't have
    explained a low cosine at all (cosine is scale-invariant to a uniform
    correction) -- per-channel cosine on unsaturated channels was already
    0.977-1.000, meaning the math was right and only the shared shift was
    wrong."""
    ratio = input_scale / output_scale
    return np.round(np.log2(1.0 / (weight_scale_arr * ratio + 1e-30))).clip(0, 31).astype(int)


def clip_shift(acc, shift):
    """Bit-identical to mac_array.cpp's clip_shift: arithmetic right-shift
    (Python's >> on Python ints matches C++ ap_int's signed arithmetic
    shift -- both floor-divide, sign-extending), then clamp to int8."""
    v = acc >> shift
    if v > 127:
        v = 127
    if v < -128:
        v = -128
    return v


def main():
    args = parse_args()

    with open(os.path.join(args.weights, "quant_config.json")) as f:
        qcfg = json.load(f)
    entry = qcfg["layer_0000_conv"]
    cin, cout = entry["Cin"], entry["Cout"]
    assert (cin, cout) == (3, 48), f"expected Stem shape 3->48, got {cin}->{cout}"

    output_scale = args.output_scale if args.output_scale is not None else PLACEHOLDER_SCALE

    weight_scale_arr = np.array(entry["weight_scale"])
    shift_per_channel = compute_shift_per_channel(args.input_scale, output_scale, weight_scale_arr)
    print(f">>> input_scale={args.input_scale:.6f}  output_scale={output_scale:.6f}")
    print(f">>> per-channel shift: min={shift_per_channel.min()} max={shift_per_channel.max()} "
          f"(quant_config's single averaged shift was {entry['out_shift']})")

    K, S, P = 3, 2, 1
    H_in = W_in = 256
    H_out = (H_in + 2 * P - K) // S + 1
    W_out = H_out
    assert H_out == 128

    img = np.fromfile(args.image, dtype=np.int8).astype(np.int64).reshape(cin, H_in, W_in)
    w = np.fromfile(os.path.join(args.weights, entry["weight_file"]), dtype=np.int8) \
          .astype(np.int64).reshape(cout, cin, K, K)
    b = np.fromfile(os.path.join(args.weights, entry["bias_file"]), dtype=np.int32) \
          .astype(np.int64)
    assert b.shape[0] == cout

    # bias_int32 on disk was quantized as bias_real/(old_input_scale*weight_scale)
    # with old_input_scale==output_scale==1/127 (export_weights.py's convention
    # -- see fold_layer_scale.py's b_scale_old = default_act_scale * w_scale).
    # Changing input_scale without rescaling bias leaves the accumulator's bias
    # term implicitly still at the OLD scale -- found by checking magnitudes
    # directly, not assumed: mean|bias|~=107853 vs typical conv-sum~=12678, bias
    # DOMINATES this layer's accumulator, so a ~1.75x bias miscalibration alone
    # is enough to explain a near-zero cosine even with input quantization fixed
    # (confirmed: stem cosine barely moved, 0.7549->0.7505, when only the input
    # scale was fixed). Rescale bias to match the new input_scale exactly.
    old_input_scale = PLACEHOLDER_SCALE  # quant_config's stored bias assumes this, always --
                                          # a fact about the export, unrelated to --output-scale
    if abs(args.input_scale - old_input_scale) > 1e-12:
        ratio = args.input_scale / old_input_scale
        b = np.round(b.astype(np.float64) / ratio).astype(np.int64)
        print(f">>> rescaled bias by 1/{ratio:.4f} for the new input_scale "
              f"(old mean|b|={np.mean(np.abs(b.astype(np.float64)*ratio)):.0f}, new mean|b|={np.mean(np.abs(b)):.0f})")

    # Zero-pad then gather K*K shifted, strided views -- exact int64
    # arithmetic throughout (no float), just vectorized instead of a
    # 48*128*128*3*3*3 ~= 21.2M-iteration Python loop. Max |acc| is
    # bounded by 127*127*3*3*3 + |bias| << 2**63, no overflow risk.
    img_pad = np.zeros((cin, H_in + 2 * P, W_in + 2 * P), dtype=np.int64)
    img_pad[:, P:P + H_in, P:P + W_in] = img

    acc = np.zeros((cout, H_out, W_out), dtype=np.int64)
    for kh in range(K):
        for kw in range(K):
            # strided slice: output (oh,ow) reads padded input at
            # (oh*S+kh, ow*S+kw) -- exactly the ih/iw formula above,
            # vectorized over oh,ow via numpy striding.
            patch = img_pad[:, kh:kh + S * H_out:S, kw:kw + S * W_out:S]  # [cin, H_out, W_out]
            wk = w[:, :, kh, kw]  # [cout, cin]
            acc += np.einsum('ihw,oi->ohw', patch, wk)
    acc += b[:, None, None]

    out = np.zeros((cout, H_out, W_out), dtype=np.int8)
    for oc in range(cout):
        s = int(shift_per_channel[oc])
        out[oc] = np.vectorize(lambda v: clip_shift(int(v), s))(acc[oc]).astype(np.int8)

    out.tofile(args.out)
    print(f">>> wrote {args.out} ({out.size} bytes, shape {out.shape})")
    print(f">>> range=[{out.min()},{out.max()}]  zeros={np.mean(out == 0) * 100:.2f}%")

    if args.shift_table_out:
        shift_path = args.shift_table_out
        bias_path = shift_path.replace(".bin", "") + "_bias_rescaled.bin"
        shift_per_channel.astype(np.int32).tofile(shift_path)
        b.astype(np.int32).tofile(bias_path)
        print(f">>> wrote {shift_path} ({shift_per_channel.size} int32, per-channel shift)")
        print(f">>> wrote {bias_path} ({b.size} int32, rescaled bias)")


if __name__ == "__main__":
    main()
