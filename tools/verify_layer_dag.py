"""
verify_layer_dag.py -- Phase A, enhanced topology verifier (ZHR-91 item 4).

The Phase 0.8 self-check (verify_driver_layer_coverage.py) only validated
per-layer shape/stride/pad/dispatch-coverage -- it had no notion of *which
buffer feeds which operation*, which is exactly the blind spot that let the
DW3(token_mixer) fan-out bug (ZHR-8 2026-08-15: token_mixer's output feeds
BOTH the ConvFFN branch AND the residual Add, the old driver only wired the
former) go undetected. This tool closes that gap generically, for any
future dispatch plan (old driver or Phase A's descriptor table):

1. Walks the real ONNX graph and extracts the ground-truth dataflow DAG for
   every one of the 52 Conv-layer tensors: which node(s) actually consume
   each layer's output (fan-out), and for every multi-input op (Add,
   Concat, ...) downstream, which tensor feeds which specific operand.
2. Exposes `check_dispatch_plan()`, which a descriptor generator (or any
   driver) can call with its own claimed producer/consumer wiring to get a
   pass/fail diff against the ground truth -- not just "did every layer
   run", but "did every consumer of every tensor actually get wired up".

Ground truth only depends on graph topology, not resolution -- verified
2026-08-15 that fastvit_t8_processed_128x128.onnx and _256x256.onnx have
identical node/op_type/name sequences (only the declared input shape
differs), so this tool's output is resolution-independent and doesn't need
to be regenerated when the target resolution changes.

CAVEAT for whoever wires this into the descriptor generator (step 2): 27 of
the 52 layers show fan-out=2 in the ground truth, but they're two different
*kinds* of fan-out and need different handling in check_dispatch_plan():
  - 10 are the real bug-4 pattern (token_mixer/DW3 feeding both the ConvFFN
    branch AND the residual Add directly) -- these need two genuinely
    separate dispatched consumers.
  - 17 are a conv feeding into a Div+Mul pair that are just the two halves
    of the erf-GELU decomposition (Div->Erf->Add->Mul->Mul) -- the driver's
    GELU op is a single atomic LUT operation that swallows this whole
    5-node chain, so at the descriptor/dispatch level this is ONE logical
    consumer ("the GELU op"), not two. Don't let the generator flag these
    as a second bug-4 case; collapse GELU-internal fan-out before comparing
    against a real dispatch plan's consumer list.

用法:
  python verify_layer_dag.py [--model <path>] [--out <json path>]
"""
import onnx
import json
import argparse
import os


def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("--model", default=r"E:\codes\microzed\fastvit_t8_processed_256x256.onnx")
    p.add_argument("--out",
                   default=r"E:\codes\microzed\fastvit_hls\tools\layer_dag_ground_truth.json")
    return p.parse_args()


def is_constant_input(name, init_names):
    return name in init_names


def build_ground_truth(model):
    graph = model.graph
    init_names = {i.name for i in graph.initializer}

    name_to_producer = {}
    for n in graph.node:
        for o in n.output:
            name_to_producer[o] = n

    def tensor_consumers(tensor_name):
        """every node that reads this tensor, and which of its named
        operand slots (in0/in1/...) it fills"""
        out = []
        for n in graph.node:
            for slot_idx, inp in enumerate(n.input):
                if inp == tensor_name:
                    out.append({"node_name": n.name, "op_type": n.op_type, "operand_slot": slot_idx})
        return out

    conv_nodes = [(i, n) for i, n in enumerate(graph.node) if n.op_type == "Conv"]
    assert len(conv_nodes) == 52, f"expected 52 Conv layers, found {len(conv_nodes)}"

    ground_truth = {"layers": [], "multi_input_nodes": []}

    for node_idx, node in conv_nodes:
        out_name = node.output[0]
        consumers = tensor_consumers(out_name)
        ground_truth["layers"].append({
            "node_idx": node_idx,
            "node_name": node.name,
            "output_tensor": out_name,
            "fan_out": len(consumers),
            "consumers": consumers,
        })

    # every multi-non-constant-input node in the graph (Add/Concat/Mul with
    # 2+ real tensor operands) -- record each operand's real source
    for node in graph.node:
        real_operands = [inp for inp in node.input if not is_constant_input(inp, init_names)]
        if len(real_operands) < 2:
            continue
        operand_sources = []
        for slot_idx, inp in enumerate(node.input):
            if inp not in real_operands:
                continue
            producer = name_to_producer.get(inp)
            operand_sources.append({
                "operand_slot": slot_idx,
                "tensor": inp,
                "producer_node": producer.name if producer else None,
                "producer_op_type": producer.op_type if producer else "GRAPH_INPUT",
            })
        ground_truth["multi_input_nodes"].append({
            "node_name": node.name,
            "op_type": node.op_type,
            "operands": operand_sources,
        })

    return ground_truth


def check_dispatch_plan(ground_truth, claimed_producer_of, claimed_consumers_of):
    """Generic checker for a descriptor generator / driver's claimed wiring.

    claimed_producer_of: dict tensor_name -> node_name that the plan believes
                          produces it (or None if it's the graph input)
    claimed_consumers_of: dict tensor_name -> list of node_names the plan
                           actually dispatches as consumers

    Returns a list of mismatch strings (empty = 0 diff)."""
    problems = []
    for layer in ground_truth["layers"]:
        out_name = layer["output_tensor"]
        real_consumer_names = {c["node_name"] for c in layer["consumers"]}
        claimed = set(claimed_consumers_of.get(out_name, []))
        missing = real_consumer_names - claimed
        extra = claimed - real_consumer_names
        if missing:
            problems.append(f"{out_name}: plan is missing consumer(s) {sorted(missing)} "
                             f"(fan_out={layer['fan_out']} in real graph)")
        if extra:
            problems.append(f"{out_name}: plan has unexpected consumer(s) {sorted(extra)}")

    for mi in ground_truth["multi_input_nodes"]:
        for op in mi["operands"]:
            tensor = op["tensor"]
            expected_producer = op["producer_node"]
            claimed = claimed_producer_of.get(tensor)
            if claimed is not None and claimed != expected_producer:
                problems.append(f"{mi['node_name']} operand slot {op['operand_slot']} "
                                 f"({tensor}): plan says producer={claimed}, real={expected_producer}")
    return problems


def main():
    args = parse_args()
    model = onnx.load(args.model)

    ground_truth = build_ground_truth(model)

    n_fanout = sum(1 for l in ground_truth["layers"] if l["fan_out"] > 1)
    print(f">>> {len(ground_truth['layers'])} Conv-layer output tensors analyzed")
    print(f">>> {n_fanout} have fan-out > 1 (feed more than one consumer):")
    for layer in ground_truth["layers"]:
        if layer["fan_out"] > 1:
            consumer_desc = ", ".join(f"{c['node_name']}({c['op_type']})" for c in layer["consumers"])
            print(f"    layer node_idx={layer['node_idx']:3d} {layer['node_name']:55s} -> {consumer_desc}")

    print(f"\n>>> {len(ground_truth['multi_input_nodes'])} multi-input nodes (Add/Concat/...) found:")
    for mi in ground_truth["multi_input_nodes"]:
        print(f"    {mi['op_type']:6s} {mi['node_name']}")
        for op in mi["operands"]:
            print(f"        slot {op['operand_slot']}: {op['tensor']}  <- {op['producer_op_type']} "
                  f"({op['producer_node']})")

    with open(args.out, "w") as f:
        json.dump(ground_truth, f, indent=2)
    print(f"\n>>> wrote ground truth to {args.out}")
    print(">>> use check_dispatch_plan(ground_truth, ...) to validate a descriptor generator's output")


if __name__ == "__main__":
    main()
