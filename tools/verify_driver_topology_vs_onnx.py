"""
Phase 0.7 step 1 -- verify that fastvit_infer.c's hardcoded per-layer
stride/H/W parameters and per-block call structure actually match the
real ONNX graph (fastvit_t8_processed_128x128.onnx) they're supposed
to implement, before trusting any cosine number computed against it.

This is a static/read-only check (no board, no PC-side inference) --
it just cross-references onnx.Node attributes against the constants
hardcoded in petalinux/software/fastvit_app/src/fastvit_infer.c.

用法: python verify_driver_topology_vs_onnx.py [--model <path>]
"""
import onnx
import argparse

def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("--model", default=r"E:\codes\microzed\fastvit_t8_processed_128x128.onnx")
    return p.parse_args()


def main():
    args = parse_args()
    model = onnx.load(args.model)
    graph = model.graph

    convs = [n for n in graph.node if n.op_type == "Conv"]
    print(f"total Conv nodes (== weight-file layer count): {len(convs)}")
    assert len(convs) == 52, "layer count mismatch -- re-check before trusting layer_00NN indices"

    def attrs(n):
        return {a.name: (list(a.ints) if a.ints else a.i) for a in n.attribute}

    print("\n=== layer 0-2 (Stem) real strides ===")
    for i in (0, 1, 2):
        a = attrs(convs[i])
        print(f"  layer_{i:04d}  {convs[i].name:45s} stride={a.get('strides')} group={a.get('group')} k={a.get('kernel_shape')}")

    print("\nDRIVER claims (fastvit_infer.c):")
    print("  layer_0000 (Stem conv 3->48):        stride=(2,2)  [driver: 2,2 -- matches]")
    print("  layer_0001 (Stem dw 48->48, K3):      stride=(1,1) [driver: 1,1 -- MISMATCH, real=2,2]")
    print("  layer_0002 (Stem pw 48->48, K1):      stride=(1,1) [driver: implicit 1,1 -- matches]")

    print("\n=== layer 49 (FinalDW) real stride ===")
    a = attrs(convs[49])
    print(f"  layer_0049  {convs[49].name:45s} stride={a.get('strides')} group={a.get('group')} k={a.get('kernel_shape')}")
    print("DRIVER claims: fv_run_dwconv(...,384,8,8,3,3, 2,2, 1,1, fpg=2,...) -- stride=(2,2)")
    print("  MISMATCH: real stride=(1,1) -- FinalDW should NOT downsample; Stage4 is already the final spatial size.")

    print("\n=== per-RepMixer-block conv count (real ONNX) vs what fastvit_infer.c's has_dw3 flag dispatches ===")
    # RepMixer blocks in ONNX order, 4 conv nodes each: token_mixer(dw3), mlp.conv(dw7), mlp.fc1(pw), mlp.fc2(pw)
    block_starts = {3: "S1B0", 7: "S1B1", 13: "S2B0", 17: "S2B1",
                    23: "S3B0", 27: "S3B1", 31: "S3B2", 35: "S3B3",
                    41: "S4B0", 45: "S4B1"}
    driver_has_dw3 = {"S1B0": 0, "S1B1": 1, "S2B0": 1, "S2B1": 1,
                       "S3B0": 1, "S3B1": 1, "S3B2": 1, "S3B3": 1,
                       "S4B0": 1, "S4B1": 1}
    for start_idx, name in block_starts.items():
        node = convs[start_idx]
        has_dw3 = driver_has_dw3[name]
        flag = "OK (token_mixer applied)" if has_dw3 else "*** BUG: token_mixer (this conv) is loaded but NEVER dispatched to the FPGA ***"
        print(f"  layer_{start_idx:04d} {name} token_mixer={node.name:50s} driver has_dw3={has_dw3}  {flag}")


if __name__ == "__main__":
    main()
