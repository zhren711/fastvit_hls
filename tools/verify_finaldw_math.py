"""
verify_finaldw_math.py - Phase 0.7 step 3 cross-check (2026-08-14).

Replicates dwconv_worker.cpp's exact int8 depthwise-conv math (bias +
Sum(weight*activation), then >>out_shift with [-128,127] clamp) in
software, using the REAL weight/bias bytes from weights_t8/ and a REAL
Stage4 activation dump captured mid-run from finaldw_shift_sweep.c
(petalinux/software/fastvit_app/src/finaldw_shift_sweep.c), to check
whether the real hardware's FinalDW output for real data matches what
the documented math should produce.

Context: the out_shift sweep (Phase 0.7 step 2, Linear ZHR-8) showed
the real hardware's FinalDW output is frozen at {-1,0} regardless of
out_shift in {8,6,4,2,0} and regardless of test image (8/8 images
identical). This script answers: is that because the real accumulator
genuinely is that small (degenerate weights/activations), or because
hardware computes something different from the documented math?

用法: python tools/verify_finaldw_math.py
  (expects weights_t8/layer_0049_dwconv_{weight,bias}.bin and
  accuracy_test_imgs/stage4_real_activation_0000.bin to already exist)
"""
import numpy as np
import os

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    act = np.fromfile(
        os.path.join(BASE, "accuracy_test_imgs", "stage4_real_activation_0000.bin"),
        dtype=np.int8).astype(np.int64).reshape(384, 8, 8)
    w = np.fromfile(
        os.path.join(BASE, "weights_t8", "layer_0049_dwconv_weight.bin"),
        dtype=np.int8).astype(np.int64).reshape(768, 3, 3)
    b = np.fromfile(
        os.path.join(BASE, "weights_t8", "layer_0049_dwconv_bias.bin"),
        dtype=np.int32).astype(np.int64)

    CHin, Hin, Win = 384, 8, 8
    Kh = Kw = 3
    stride = 2
    pad = 1
    fpg = 2
    Hout = (Hin + 2 * pad - Kh) // stride + 1
    Wout = (Win + 2 * pad - Kw) // stride + 1
    print(f"Hout={Hout} Wout={Wout}")

    act_pad = np.zeros((CHin, Hin + 2 * pad, Win + 2 * pad), dtype=np.int64)
    act_pad[:, pad:pad + Hin, pad:pad + Win] = act

    out_raw = np.zeros((768, Hout, Wout), dtype=np.int64)
    for co in range(768):
        ch = co // fpg
        acc = np.full((Hout, Wout), b[co], dtype=np.int64)
        for kh in range(Kh):
            for kw in range(Kw):
                patch = act_pad[ch, kh:kh + Hout * stride:stride, kw:kw + Wout * stride:stride]
                acc += patch * w[co, kh, kw]
        out_raw[co] = acc

    print(f"raw accumulator (pre-shift, pre-clamp): min={out_raw.min()} "
          f"max={out_raw.max()} mean={out_raw.mean():.3f} std={out_raw.std():.3f}")
    print(">>> real hardware's observed output at every out_shift in {8,6,4,2,0}: "
          "min=-1 max=0 mean=-0.829 nonzero=10192/12288 (82.9%), IDENTICAL across all shifts")
    print()

    for shift in [8, 6, 4, 2, 0]:
        s = out_raw >> shift
        r = np.clip(s, -128, 127).astype(np.int8)
        nz = np.count_nonzero(r)
        print(f"software replica shift={shift}: min={r.min()} max={r.max()} "
              f"mean={r.mean():.3f} nonzero={nz}/{r.size} ({100 * nz / r.size:.1f}%)")


if __name__ == "__main__":
    main()
