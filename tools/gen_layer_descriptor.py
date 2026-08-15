"""
gen_layer_descriptor.py -- Phase A step 2a: pure structural layer descriptor.

Deliberately scoped to STRUCTURE ONLY (shape/stride/pad/group/fpg, weight
file index, DAG consumer wiring) -- no LayerScale gamma folding, no
activation calibration. Those are numeric transforms (step 2b) that need
real data and per-layer float comparison to validate; mixing them into this
step would make it impossible to tell, if the generated table is wrong,
whether the bug is in structure extraction or in a numeric fold -- exactly
the confounded-round pattern this project has been burned by before.

This table, on its own, already fixes topology/dispatch bugs 1-4 from
ZHR-91 by construction: shape/stride/pad/group come straight from the ONNX
graph (bug 1/2, no hand-typed literals to transcribe wrong), and the
`consumers` field makes every tensor's real fan-out explicit instead of an
implicit linear-chain assumption (bug 3/4, in particular the token_mixer/
DW3 fan-out to both the ConvFFN branch and the residual Add).

Sources combined (all already independently produced/verified this
session):
  - fastvit_t8_processed_256x256.onnx: shape (via shape inference),
    stride/pad/kernel_shape/group (node attributes)
  - weights_t8_pruned/quant_config.json: Cin/Cout (post-pruning truth,
    resolution-independent -- channel counts don't change with input size)
    and weight/bias file names
  - layer_dag_ground_truth.json (tools/verify_layer_dag.py): real DAG
    wiring, used directly as the `consumers` field, not re-derived

Self-check: after generation, replays the descriptor's implied wiring
through verify_layer_dag.py's check_dispatch_plan() against the same
ground truth and expects 0 diff. This is NOT a check for shape/stride
correctness (that comes from the ONNX graph by construction, no separate
gold reference exists to diff against) -- it is specifically a check that
the `consumers` field was faithfully carried over and that layer_idx
resolution didn't introduce an off-by-one or dropped entry.

用法:
  python gen_layer_descriptor.py [--model <onnx>] [--quant-config <json>]
                                  [--dag-ground-truth <json>] [--out <json>]
"""
import onnx
import onnx.shape_inference
import json
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from verify_layer_dag import check_dispatch_plan  # reuse, don't re-derive


def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("--model", default=r"E:\codes\microzed\fastvit_t8_processed_256x256.onnx")
    p.add_argument("--quant-config",
                   default=r"E:\codes\microzed\fastvit_hls\weights_t8_pruned\quant_config.json")
    p.add_argument("--dag-ground-truth",
                   default=r"E:\codes\microzed\fastvit_hls\tools\layer_dag_ground_truth.json")
    p.add_argument("--out",
                   default=r"E:\codes\microzed\fastvit_hls\tools\layer_descriptor_256.json")
    return p.parse_args()


def node_attrs(n):
    return {a.name: (list(a.ints) if a.ints else a.i) for a in n.attribute}


def classify_consumers(raw_consumers, node_idx_by_conv_output_producer):
    """Map a layer's full raw ONNX consumer list into the semantic kinds
    the layer controller actually dispatches -- collapsing GELU's internal
    Div/Erf/Add/Mul/Mul chain into ONE atomic 'gelu' entry (see
    verify_layer_dag.py caveat: naively keeping Div and Mul as two separate
    consumers would misclassify 17 benign GELU fan-outs as bug-4 cases).

    Distinguishing "Mul belongs to GELU" from "Mul is a genuine separate
    consumer" (e.g. SE's final gating multiply, which also reads FinalDW's
    raw output directly, same fan-out *shape* as bug 4 but a real,
    already-correctly-handled second consumer) is done structurally, not
    by node name: GELU's erf decomposition always pairs a Div sibling with
    a Mul sibling reading the SAME source tensor. If a Div sibling exists,
    every Mul sibling on this tensor is folded into that one 'gelu' entry;
    otherwise a Mul is its own consumer (kind='mul', e.g. the SE gate)."""
    op_types_present = {c["op_type"] for c in raw_consumers}
    has_gelu_div = "Div" in op_types_present

    out = []
    gelu_emitted = False
    for c in raw_consumers:
        op = c["op_type"]
        name = c["node_name"]
        if has_gelu_div and op in ("Div", "Mul"):
            if not gelu_emitted:
                out.append({"kind": "gelu"})
                gelu_emitted = True
            continue
        if op == "Add":
            out.append({"kind": "add", "add_node": name})
        elif op == "ReduceMean":
            out.append({"kind": "se_reduce"})
        elif op == "Mul":
            out.append({"kind": "mul", "mul_node": name})
        elif op == "Conv":
            target_idx = node_idx_by_conv_output_producer.get(name)
            out.append({"kind": "conv", "layer_idx": target_idx})
        else:
            out.append({"kind": "other", "node_name": name, "op_type": op})
    return out


def main():
    args = parse_args()

    model = onnx.load(args.model)
    inferred = onnx.shape_inference.infer_shapes(model)
    graph = inferred.graph

    shape_map = {}
    for vi in list(graph.value_info) + list(graph.input) + list(graph.output):
        dims = vi.type.tensor_type.shape.dim
        shape_map[vi.name] = [d.dim_value for d in dims]

    with open(args.quant_config) as f:
        qcfg = json.load(f)
    with open(args.dag_ground_truth) as f:
        dag_gt = json.load(f)

    layers_by_tag = sorted(qcfg.items(), key=lambda kv: int(kv[0].split("_")[1]))
    assert len(layers_by_tag) == 52

    # node_name (that is itself a Conv) -> its layer_idx, for resolving
    # "conv consumes conv" edges in the DAG to a layer_idx instead of a
    # raw ONNX node name
    node_idx_by_conv_output_producer = {}
    for layer_idx, (tag, entry) in enumerate(layers_by_tag):
        node = graph.node[entry["node_idx"]]
        node_idx_by_conv_output_producer[node.name] = layer_idx

    dag_by_node_idx = {l["node_idx"]: l for l in dag_gt["layers"]}

    descriptor = []
    for layer_idx, (tag, entry) in enumerate(layers_by_tag):
        node_idx = entry["node_idx"]
        node = graph.node[node_idx]
        a = node_attrs(node)

        kernel = a.get("kernel_shape", [1, 1])
        stride = a.get("strides", [1, 1])
        pad = a.get("pads", [0, 0, 0, 0])
        group = a.get("group", 1)

        in_name = node.input[0]
        assert in_name in shape_map, f"{tag}: no shape info for {in_name!r}"
        in_shape = shape_map[in_name]
        assert len(in_shape) == 4, f"{tag}: expected NCHW, got {in_shape}"
        h_in, w_in = in_shape[2], in_shape[3]

        cin = entry["Cin"]
        cout = entry["Cout"]
        op_type = entry["op_type"]
        fpg = (cout // cin) if (op_type == "dwconv" and cin > 0) else 1

        assert kernel[0] == kernel[1], f"{tag}: non-square kernel {kernel}"
        assert stride[0] == stride[1], f"{tag}: non-square stride {stride}"
        assert pad[0] == pad[1] == pad[2] == pad[3], f"{tag}: asymmetric pad {pad}"

        dag_entry = dag_by_node_idx.get(node_idx)
        consumers = []
        if dag_entry is not None:
            consumers = classify_consumers(dag_entry["consumers"], node_idx_by_conv_output_producer)

        descriptor.append({
            "layer_idx": layer_idx,
            "tag": tag,
            "node_idx": node_idx,
            "node_name": node.name,
            "op_type": op_type,
            "cin": cin, "cout": cout,
            "h_in": h_in, "w_in": w_in,
            "k": kernel[0], "stride": stride[0], "pad": pad[0],
            "group": group, "fpg": fpg,
            "weight_file": entry["weight_file"],
            "bias_file": entry["bias_file"],
            "consumers": consumers,
        })

    with open(args.out, "w") as f:
        json.dump(descriptor, f, indent=2)
    print(f">>> wrote {args.out} ({len(descriptor)} layers)")

    # ---- self-check: replay this descriptor's implied wiring against the
    # DAG ground truth via check_dispatch_plan(), expect 0 diff ----
    claimed_producer_of = {}
    claimed_consumers_of = {}
    for layer in descriptor:
        node_idx = layer["node_idx"]
        node = graph.node[node_idx]
        out_tensor = node.output[0]
        claimed_producer_of[out_tensor] = node.name
        consumer_node_names = []
        for c in layer["consumers"]:
            if c["kind"] == "gelu":
                # descriptor collapses GELU's 5-node erf chain to one entry;
                # ground truth still lists the raw Div+Mul nodes as
                # consumers, so expand back out for a fair diff
                dag_entry = dag_by_node_idx[node_idx]
                consumer_node_names.extend(
                    cc["node_name"] for cc in dag_entry["consumers"]
                    if cc["op_type"] in ("Div", "Mul"))
            elif c["kind"] == "add":
                consumer_node_names.append(c["add_node"])
            elif c["kind"] == "se_reduce":
                dag_entry = dag_by_node_idx[node_idx]
                consumer_node_names.extend(
                    cc["node_name"] for cc in dag_entry["consumers"]
                    if cc["op_type"] == "ReduceMean")
            elif c["kind"] == "mul":
                consumer_node_names.append(c["mul_node"])
            elif c["kind"] == "conv":
                target_node = graph.node[layers_by_tag[c["layer_idx"]][1]["node_idx"]]
                consumer_node_names.append(target_node.name)
            else:
                consumer_node_names.append(c["node_name"])
        claimed_consumers_of[out_tensor] = consumer_node_names

    problems = check_dispatch_plan(dag_gt, claimed_producer_of, claimed_consumers_of)
    print(f"\n>>> self-check vs layer_dag_ground_truth.json: {len(problems)} problems")
    for p in problems:
        print("   ", p)
    if not problems:
        print(">>> 0 差异")


if __name__ == "__main__":
    main()
