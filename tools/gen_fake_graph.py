#!/usr/bin/env python3
# Usage (generate a graph header; output auto-named under cases/):
#   python3 tools/gen_fake_graph.py -t 480 -p 8 -l 4 -c 4 -d 5 -s 16
#     -> cases/fg_t480_p8_l4_c4_d5.h   [--dot out.dot] [--png out.png]
#   # -t/--tasks-per-layer  -p/--pre-cnt  -l/--chain-depth  -c/--thread-cnt
#   # -s/--chunk-size  -d/--avg-duration  --dot  --png  --from-header
"""
Generate a synthetic DAG subgraph header file in the same shape as
cases/fg_t480_p4_l4_c4_d5.h (global total_task_id/total_type/total_duration/
total_pre_cnt/total_task_state plus per-subgraph task_id/pre_idx/predecessors and
suc_cnt/suc_idx/successors).

Parameters:
  --tasks-per-layer  Tasks per layer (breadth); total = tasks_per_layer * chain_depth
  --pre-cnt          Number of predecessors each layer-k task has from layer-(k-1).
                    Layer 0 tasks have 0 predecessors.
  --chain-depth      Dependency chain depth; total tasks = tasks_per_layer * chain_depth
  --thread-cnt       Number of painter threads / subgraphs (PAINTER_THREAD_CNT)
  --avg-duration     Average task execution time in ns (default: 30000)
  --dot              Optional output DOT file for visualization
  Output auto-named: cases/fg_t<tasks>_p<pre>_l<depth>_c<threads>_d<dur>.h
"""

import argparse
import random
import os
import re
import subprocess
import sys
from typing import Optional

def generate_task_ids(task_cnt: int, chunk_size: int, thread_idx: int, thread_cnt: int) -> list[int]:
    """Assign global task ids to one subgraph as round-robin chunks.

    Global task ids 0..task_cnt-1 are split into consecutive chunks of
    ``chunk_size``. Chunks are distributed round-robin across the
    ``thread_cnt`` subgraphs: chunk 0 -> sg0, chunk 1 -> sg1, ...,
    chunk thread_cnt-1 -> sg{thread_cnt-1}, chunk thread_cnt -> sg0, etc.
    Thread ``thread_idx`` therefore receives chunks at positions
    thread_idx, thread_idx + thread_cnt, thread_idx + 2*thread_cnt, ...

    Example (task_cnt=32, chunk_size=16, thread_cnt=2):
        sg0 (thread_idx=0): [0..15]
        sg1 (thread_idx=1): [16..31]

    Example (task_cnt=64, chunk_size=16, thread_cnt=2):
        sg0: [0..15], [32..47]
        sg1: [16..31], [48..63]
    """
    ids = []
    for start in range(thread_idx * chunk_size, task_cnt, thread_cnt * chunk_size):
        end = min(start + chunk_size, task_cnt)
        ids.extend(range(start, end))
    return ids


def generate_pre_cnt_data(
    task_ids: list[int],
    pre_cnt: int,
    per_sub_layer: int,
    chain_depth: int,
) -> tuple[list[int], list[int], list[int]]:
    """Generate pre_cnt, pre_idx, predecessors arrays.

    Layer-to-layer dependency: tasks in layer k (k>0) each depend on
    ``pre_cnt`` tasks from layer k-1 (wrapping within the layer).

    Returns (pre_cnt_list, pre_idx_list, predecessors_list).
    """
    n = len(task_ids)  # == per_sub_layer * chain_depth

    pre_cnt_list = [0] * n
    pre_idx_list = [0] * n
    predecessors_list = []

    offset = 0
    for i in range(n):
        pre_idx_list[i] = offset
        layer = i // per_sub_layer
        if layer == 0:
            pre_cnt_list[i] = 0
        else:
            pos_in_layer = i % per_sub_layer
            cnt = min(pre_cnt, per_sub_layer)
            pre_cnt_list[i] = cnt
            for k in range(cnt):
                pred_pos = (pos_in_layer + k) % per_sub_layer
                pred_idx = (layer - 1) * per_sub_layer + pred_pos
                predecessors_list.append(task_ids[pred_idx])
            offset += cnt

    return pre_cnt_list, pre_idx_list, predecessors_list


def generate_type_array(n: int) -> list[int]:
    """Alternate aic(1) / aiv(0)."""
    return [1 if i % 2 == 0 else 0 for i in range(n)]


def generate_durations(n: int, avg_duration: int) -> list[int]:
    """Generate synthetic durations (ns) around avg_duration.

    AIC tasks (even index) get longer durations: avg_duration * [0.9, 1.6]
    AIV tasks (odd index)  get shorter durations: avg_duration * [0.04, 1.0]
    This mimics the paged_attention pattern where AIC ≈ 50k and AIV ≈ 2.5k.
    """
    rng = random.Random(42)  # fixed seed → deterministic output
    durations = []
    for i in range(n):
        if i % 2 == 0:  # AIC
            factor = rng.uniform(0.9, 1.6)
        else:           # AIV
            factor = rng.uniform(0.04, 1.0)
        durations.append(max(1, int(avg_duration * factor)))
    return durations


def _array_body(values: list[int]) -> str:
    """Comma-separated C static array initializer body."""
    return ", ".join(str(v) for v in values)


def compute_successors(
    groups: list[dict],
) -> tuple[list[list[int]], list[list[int]], list[list[int]], int, list[int]]:
    """Compute successor arrays split by the group of the SUCCESSOR.

    Mirrors the semantics of tools/inject_successors.py so the emitted header
    matches the runtime's resolve_dep() expectations:

        suc_cnt[g][p] = number of task p's successors that live in group g
        successors[g]  = flattened list of those group-g successor ids
        suc_idx[g][p]  = offset into successors[g] for task p

    ``p`` is the task's position in the ascending task-id ordering (which is
    the task id itself for the contiguous 0..total_task_cnt-1 ids this
    generator produces).

    Returns (suc_cnt, suc_idx, successors, total_task_cnt, id_order).
    """
    group_of_id: dict[int, int] = {}
    for g, group in enumerate(groups):
        for tid in group["task_id"]:
            group_of_id[tid] = g

    id_order = sorted(group_of_id.keys())
    total_task_cnt = len(id_order)
    id_to_pos = {tid: pos for pos, tid in enumerate(id_order)}

    n_groups = len(groups)
    suc_cnt = [[0] * total_task_cnt for _ in range(n_groups)]

    for g, group in enumerate(groups):
        task_id = group["task_id"]
        pre_cnt = group["pre_cnt"]
        pre_idx = group["pre_idx"]
        predecessors = group["predecessors"]
        for i in range(len(task_id)):
            for k in range(pre_cnt[i]):
                p = predecessors[pre_idx[i] + k]
                suc_cnt[g][id_to_pos[p]] += 1

    suc_idx = []
    successors = []
    for g in range(n_groups):
        idx = [0] * total_task_cnt
        total = 0
        for pos in range(total_task_cnt):
            idx[pos] = total
            total += suc_cnt[g][pos]
        suc_idx.append(idx)
        successors.append([0] * total)

    cursor = [idx[:] for idx in suc_idx]
    for g, group in enumerate(groups):
        task_id = group["task_id"]
        pre_cnt = group["pre_cnt"]
        pre_idx = group["pre_idx"]
        predecessors = group["predecessors"]
        for i in range(len(task_id)):
            successor_id = task_id[i]
            for k in range(pre_cnt[i]):
                p = predecessors[pre_idx[i] + k]
                p_pos = id_to_pos[p]
                successors[g][cursor[g][p_pos]] = successor_id
                cursor[g][p_pos] += 1

    return suc_cnt, suc_idx, successors, total_task_cnt, id_order


def _auto_name(args: argparse.Namespace) -> str:
    """Derive the output filename base from the graph parameters."""
    return (
        f"fg_t{args.tasks_per_layer}_p{args.pre_cnt}_l{args.chain_depth}"
        f"_c{args.thread_cnt}_d{args.avg_duration}"
    )


def generate_header(args: argparse.Namespace) -> str:
    chain_depth = args.chain_depth
    pre_cnt_p = args.pre_cnt
    thread_cnt = args.thread_cnt
    name = _auto_name(args)
    avg_duration = args.avg_duration

    tasks_per_layer = args.tasks_per_layer
    task_cnt = tasks_per_layer * chain_depth
    chunk_size = args.chunk_size

    # Validate
    if tasks_per_layer % thread_cnt != 0:
        raise ValueError(
            f"tasks_per_layer {tasks_per_layer} must be divisible by thread_cnt {thread_cnt}"
        )
    if chunk_size <= 0:
        raise ValueError(f"chunk_size must be positive, got {chunk_size}")
    if task_cnt % (chunk_size * thread_cnt) != 0:
        raise ValueError(
            f"total tasks {task_cnt} must be divisible by chunk_size*thread_cnt "
            f"({chunk_size}*{thread_cnt}={chunk_size * thread_cnt}) so that every "
            f"subgraph receives the same number of chunks"
        )

    per_sub_layer = tasks_per_layer // thread_cnt

    # Build each subgraph's local arrays.
    groups = []
    for t in range(thread_cnt):
        ids = generate_task_ids(task_cnt, chunk_size, t, thread_cnt)
        n_sub = len(ids)
        types = generate_type_array(n_sub)
        durations = generate_durations(n_sub, avg_duration)
        pre_cnt, pre_idx, predecessors = generate_pre_cnt_data(
            ids, pre_cnt_p, per_sub_layer, chain_depth
        )
        groups.append({
            "task_id": ids,
            "type": types,
            "duration": durations,
            "pre_cnt": pre_cnt,
            "pre_idx": pre_idx,
            "predecessors": predecessors,
        })

    suc_cnt, suc_idx, successors, total_cnt, id_order = compute_successors(groups)
    id_to_pos = {tid: pos for pos, tid in enumerate(id_order)}

    # Global (merged) arrays, indexed by global task id.
    total_task_id_vals = list(id_order)
    total_type_vals = [0] * total_cnt
    total_duration_vals = [0] * total_cnt
    total_pre_cnt_vals = [0] * total_cnt
    total_task_state_vals = [0] * total_cnt
    for group in groups:
        for i, tid in enumerate(group["task_id"]):
            pos = id_to_pos[tid]
            total_type_vals[pos] = group["type"][i]
            total_duration_vals[pos] = group["duration"][i]
            total_pre_cnt_vals[pos] = group["pre_cnt"][i]

    lines = []
    guard = f"CASES_STATIC_{name.upper()}_SUBGRAPH_H"
    up = name.upper()

    lines.append("/*")
    lines.append(" * AUTO-GENERATED by tools/gen_fake_graph.py. Do not edit by hand.")
    lines.append(f" * workload: {name}, spmd_tier={thread_cnt}, tasks: {task_cnt}.")
    lines.append(f" * {thread_cnt} subgraphs, round-robin chunks of {chunk_size} tasks (layer_size={per_sub_layer}); PAINTER_THREAD_CNT={thread_cnt}.")
    lines.append(f" * type/duration: synthetic (avg {avg_duration} ns).")
    lines.append(f" * predecessors: synthetic layered DAG (pre_cnt={pre_cnt_p}, chain_depth={chain_depth}).")
    lines.append(" */")
    lines.append(f"#ifndef {guard}")
    lines.append(f"#define {guard}")
    lines.append("")
    lines.append("#include <stdint.h>")
    lines.append('#include "conf.h"')
    lines.append("")
    lines.append(f"static uint32_t total_task_cnt = {task_cnt};")
    lines.append(f"static uint32_t total_task_id[{task_cnt}] = {{{_array_body(total_task_id_vals)}}};")
    lines.append(f"static char total_type[{task_cnt}] = {{{_array_body(total_type_vals)}}};")
    lines.append(f"static int total_duration[{task_cnt}] = {{{_array_body(total_duration_vals)}}};")
    lines.append("/* total_pre_cnt[] is decremented by every painter thread at runtime;")
    lines.append(" * keep it on its own 64-byte cache line to avoid false sharing. */")
    lines.append(f"static _Alignas(CACHE_LINE_SIZE) int total_pre_cnt[{task_cnt}] = {{{_array_body(total_pre_cnt_vals)}}};")
    lines.append(f"static char total_task_state[{task_cnt}] = {{{_array_body(total_task_state_vals)}}};")
    lines.append("")

    # Per-subgraph arrays.
    for t in range(thread_cnt):
        group = groups[t]
        sg = f"sg{t}"
        n_sub = len(group["task_id"])
        pred_body = _array_body(group["predecessors"]) if group["predecessors"] else "0"
        succ_body = _array_body(successors[t]) if successors[t] else "0"

        lines.append(f"#define {up}_SG{t}_TASK_CNT {n_sub}")
        lines.append(f"/* {sg}_task_id[]: ids of this subgraph's tasks (round-robin chunk of {chunk_size}) */")
        lines.append(f"static uint32_t {sg}_task_id[{up}_SG{t}_TASK_CNT]  = {{{_array_body(group['task_id'])}}};")
        lines.append(f"/* {sg}_pre_idx[]: offset into {sg}_predecessors[] per task */")
        lines.append(f"static int      {sg}_pre_idx[{up}_SG{t}_TASK_CNT]  = {{{_array_body(group['pre_idx'])}}};")
        lines.append(f"/* {sg}_predecessors[]: flattened predecessor id lists (global ids) */")
        lines.append(f"static int      {sg}_predecessors[]            = {{{pred_body}}};")
        lines.append(f"static int {sg}_suc_cnt[{task_cnt}] = {{{_array_body(suc_cnt[t])}}};")
        lines.append(f"static int {sg}_suc_idx[{task_cnt}] = {{{_array_body(suc_idx[t])}}};")
        lines.append(f"static int {sg}_successors[] = {{{succ_body}}};")
        if t < thread_cnt - 1:
            lines.append("")

    # subgraph struct
    lines.append("typedef struct subgraph {")
    lines.append("    uint32_t  task_cnt;")
    lines.append("    uint32_t* task_id;")
    lines.append("    int*      pre_idx;")
    lines.append("    int*      predecessors;")
    lines.append("    int* suc_cnt;")
    lines.append("    int* suc_idx;")
    lines.append("    int* successors;")
    lines.append("    int* total_pre_cnt;")
    lines.append("} subgraph;")
    lines.append("")

    # graph array
    lines.append("static subgraph test_graph[PAINTER_THREAD_CNT] = {")
    for t in range(thread_cnt):
        sg = f"sg{t}"
        n_sub = len(groups[t]["task_id"])
        line = (
            f"    {{{n_sub}, {sg}_task_id, {sg}_pre_idx, {sg}_predecessors, "
            f"{sg}_suc_cnt, {sg}_suc_idx, {sg}_successors, total_pre_cnt}}"
        )
        if t < thread_cnt - 1:
            line += ","
        lines.append(line)
    lines.append("};")
    lines.append("")
    lines.append(f"#endif /* {guard} */")

    return "\n".join(lines) + "\n"


def render_dot(dot_path: str, output_path: str, fmt: str = "png") -> bool:
    """Render DOT file to image using Graphviz."""
    try:
        result = subprocess.run(["which", "dot"], capture_output=True, text=True)
        if result.returncode != 0:
            print(
                "Warning: Graphviz 'dot' command not found. "
                "Install with: brew install graphviz"
            )
            print(f"Generated DOT file: {dot_path}")
            return False

        cmd = ["dot", "-T", fmt, "-o", output_path, dot_path]
        result = subprocess.run(cmd, capture_output=True, text=True)

        if result.returncode == 0:
            print(f"Rendered image saved to: {output_path}")
            return True
        else:
            print(f"Error rendering DOT: {result.stderr}")
            return False

    except Exception as e:
        print(f"Error rendering DOT: {e}")
        return False


def _extract_c_array(text: str, var_prefix: str) -> Optional[list[int]]:
    """Extract integer values from a C static array initializer like ``sg0_task_id[720] = {0,1,2};``."""
    pattern = re.compile(rf"\b{re.escape(var_prefix)}\s*\[[^\]]*\]\s*=\s*\{{\s*([^}}]*)\}}\s*;", re.DOTALL)
    m = pattern.search(text)
    if not m:
        return None
    body = m.group(1)
    values = [int(v.strip()) for v in body.split(",") if v.strip()]
    return values


def _derive_pre_cnt(pre_idx: list[int], n: int, total_preds: int) -> list[int]:
    """Reconstruct per-task predecessor counts from pre_idx offsets.

    Headers generated after the ``pre_cnt`` field was dropped only store
    ``pre_idx`` + ``predecessors``. The count for task ``i`` is the span
    between ``pre_idx[i]`` and ``pre_idx[i+1]`` (or the end of the flattened
    predecessor array for the final task).
    """
    counts = []
    for i in range(n):
        start = pre_idx[i]
        end = pre_idx[i + 1] if i + 1 < n else total_preds
        counts.append(end - start)
    return counts


def parse_header(header_path: str) -> tuple[list[list[int]], list[list[int]], list[list[int]], list[list[int]], int]:
    """Parse a generated .h subgraph header file.

    Returns:
        task_ids_per_sg:  list of lists – task_id arrays per subgraph
        types_per_sg:     list of lists – type arrays per subgraph
        pre_cnts_per_sg:  list of lists – pre_cnt arrays per subgraph
        preds_per_sg:     list of lists – predecessor arrays per subgraph
        thread_cnt:       number of subgraphs detected
    """
    with open(header_path, "r") as f:
        content = f.read()

    total_type = _extract_c_array(content, "total_type")

    task_ids_per_sg = []
    types_per_sg = []
    pre_cnts_per_sg = []
    preds_per_sg = []
    t = 0

    while True:
        ids = _extract_c_array(content, f"sg{t}_task_id")
        if ids is None:
            break
        typs = _extract_c_array(content, f"sg{t}_type")
        if typs is None and total_type is not None:
            typs = [total_type[tid] for tid in ids]
        pre_cnts = _extract_c_array(content, f"sg{t}_pre_cnt")
        preds = _extract_c_array(content, f"sg{t}_predecessors")
        if pre_cnts is None and preds is not None:
            pre_idx = _extract_c_array(content, f"sg{t}_pre_idx")
            if pre_idx is not None:
                pre_cnts = _derive_pre_cnt(pre_idx, len(ids), len(preds))
        task_ids_per_sg.append(ids)
        types_per_sg.append(typs)
        pre_cnts_per_sg.append(pre_cnts if pre_cnts is not None else [])
        preds_per_sg.append(preds if preds is not None else [])
        t += 1

    if t == 0:
        raise ValueError(f"No subgraph data found in {header_path}")

    return task_ids_per_sg, types_per_sg, pre_cnts_per_sg, preds_per_sg, t


def generate_dot_from_data(
    task_ids_per_sg: list[list[int]],
    types_per_sg: list[list[int]],
    predecessors_per_sg: list[list[int]],
    pre_cnts_per_sg: Optional[list[list[int]]] = None,
    graph_name: str = "DAG",
) -> str:
    """Generate DOT content from parsed subgraph arrays."""
    TASK_TYPE_NAMES = {0: "AIV", 1: "AIC", 2: "MIX"}
    SHAPE_MAP = {0: "ellipse", 1: "rect", 2: "diamond"}
    COLOR_MAP = {0: "#9c27b0", 1: "#ff9800", 2: "#00bcd4"}

    thread_cnt = len(task_ids_per_sg)

    dot_lines = [
        f"digraph {graph_name} {{",
        "  rankdir=TB;",
        '  bgcolor="#ffffff";',
        '  node [fontname="Arial" fontcolor="#000000"];',
        '  edge [color="#666" fontname="Arial"];',
        "",
    ]

    # Build id → type mapping and edges from predecessors arrays
    all_nodes: dict[int, int] = {}  # node_id → type
    edges: set[tuple[int, int]] = set()

    for sg_idx in range(thread_cnt):
        ids = task_ids_per_sg[sg_idx]
        typs = types_per_sg[sg_idx] if types_per_sg[sg_idx] else [0] * len(ids)
        preds = predecessors_per_sg[sg_idx]
        pcnts = pre_cnts_per_sg[sg_idx] if pre_cnts_per_sg else None

        # Use pre_cnt (if available) to properly consume predecessor array
        pred_ptr = 0
        for local_i in range(len(ids)):
            node_id = ids[local_i]
            task_type = typs[local_i] if local_i < len(typs) else 0
            all_nodes[node_id] = task_type

            if pcnts is not None and local_i < len(pcnts):
                cnt = pcnts[local_i]
            else:
                # Fallback heuristic: if pred_ptr still has entries matching
                # the previous ID, consume one predecessor
                cnt = 0
                if local_i > 0 and pred_ptr < len(preds) and preds[pred_ptr] == ids[local_i - 1]:
                    cnt = 1

            for _ in range(cnt):
                if pred_ptr < len(preds):
                    edges.add((preds[pred_ptr], node_id))
                    pred_ptr += 1

    # Add nodes
    for node_id in sorted(all_nodes):
        task_type = all_nodes[node_id]
        type_name = TASK_TYPE_NAMES.get(task_type, "UNKNOWN")
        shape = SHAPE_MAP.get(task_type, "rect")
        color = COLOR_MAP.get(task_type, "#ff9800")
        dot_lines.append(
            f'  T{node_id} [label="T{node_id}\\n{type_name}" '
            f'shape={shape} fillcolor="{color}" fontcolor="#000000"];'
        )

    dot_lines.append("")

    for src, dst in sorted(edges):
        dot_lines.append(f"  T{src} -> T{dst};")

    dot_lines.append("}")
    return "\n".join(dot_lines) + "\n"


def generate_dot(args: argparse.Namespace) -> str:
    """Generate a DOT file for visualization of the DAG from parameters."""
    chain_depth = args.chain_depth
    tasks_per_layer = args.tasks_per_layer
    task_cnt = tasks_per_layer * chain_depth
    pre_cnt_p = args.pre_cnt
    thread_cnt = args.thread_cnt

    task_ids_per_sg = []
    types_per_sg = []
    pre_cnts_per_sg = []
    predecessors_per_sg = []

    for t in range(thread_cnt):
        ids = generate_task_ids(task_cnt, args.chunk_size, t, thread_cnt)
        n_sub = len(ids)
        types = generate_type_array(n_sub)
        per_sub_layer = tasks_per_layer // thread_cnt
        pre_cnt, pre_idx, predecessors = generate_pre_cnt_data(ids, pre_cnt_p, per_sub_layer, chain_depth)
        task_ids_per_sg.append(ids)
        types_per_sg.append(types)
        pre_cnts_per_sg.append(pre_cnt)
        predecessors_per_sg.append(predecessors)

    return generate_dot_from_data(
        task_ids_per_sg, types_per_sg, predecessors_per_sg,
        pre_cnts_per_sg=pre_cnts_per_sg, graph_name="SyntheticDAG",
    )


def main():
    parser = argparse.ArgumentParser(
        description="Generate a synthetic DAG subgraph header for the scheduler."
    )
    parser.add_argument(
        "--tasks-per-layer", "-t", type=int, default=32,
        help="Tasks per layer; total = tasks_per_layer * layers (default: 32)"
    )
    parser.add_argument(
        "--pre-cnt", "-p", type=int, default=3,
        help="Number of predecessors per task (sliding window). "
             "Task i depends on the min(i, p) preceding tasks. (default: 3)"
    )
    parser.add_argument(
        "--chain-depth", "-l", type=int, default=60,
        help="Dependency chain depth; total tasks = tasks_per_layer * chain_depth (default: 60)"
    )
    parser.add_argument(
        "--thread-cnt", "-c", type=int, default=2,
        help="PAINTER_THREAD_CNT / number of subgraphs (default: 2)"
    )
    parser.add_argument(
        "--chunk-size", "-s", type=int, default=16,
        help="Number of consecutive task ids assigned to one subgraph before "
             "moving to the next subgraph (round-robin). (default: 16)"
    )
    parser.add_argument(
        "--dot", type=str, default=None,
        help="Optional DOT output file for DAG visualization"
    )
    parser.add_argument(
        "--png", type=str, default=None,
        help="Optional PNG output file for DAG visualization (rendered via Graphviz)"
    )
    parser.add_argument(
        "--from-header", type=str, default=None,
        help="Parse an existing .h subgraph header and output DOT/PNG from it"
    )
    parser.add_argument(
        "--avg-duration", "-d", type=int, default=30000,
        help="Average task execution time in ns (default: 30000)"
    )
    args = parser.parse_args()

    # --- from-header mode: parse existing .h and optionally visualize ---
    if args.from_header:
        if args.dot or args.png:
            task_ids_per_sg, types_per_sg, pre_cnts_per_sg, preds_per_sg, thread_cnt = parse_header(args.from_header)
            print(f"Parsed {len(task_ids_per_sg)} subgraphs from {args.from_header}")
            total = sum(len(ids) for ids in task_ids_per_sg)
            print(f"Total tasks: {total}, total edges: {sum(len(p) for p in preds_per_sg)}")

            graph_name = os.path.splitext(os.path.basename(args.from_header))[0]
            dot_content = generate_dot_from_data(
                task_ids_per_sg, types_per_sg, preds_per_sg,
                pre_cnts_per_sg=pre_cnts_per_sg, graph_name=graph_name,
            )
            dot_path = args.dot
            if not dot_path:
                dot_path = args.png.replace(".png", ".dot") if args.png and args.png.endswith(".png") else (args.png + ".dot" if args.png else None)
            if dot_path:
                with open(dot_path, "w") as f:
                    f.write(dot_content)
                print(f"DOT written to: {dot_path}")

            if args.png:
                render_dot(dot_path, args.png, "png")
        else:
            print("--from-header requires --dot or --png to specify an output file")
            sys.exit(1)
        return

    # --- generation mode ---
    header = generate_header(args)

    out_dir = "cases"
    os.makedirs(out_dir, exist_ok=True)
    out_path = os.path.join(out_dir, _auto_name(args) + ".h")
    with open(out_path, "w") as f:
        f.write(header)
    print(f"Header written to: {out_path}")

    if args.dot or args.png:
        dot_content = generate_dot(args)
        dot_path = args.dot
        if not dot_path:
            dot_path = args.png.replace(".png", ".dot") if args.png.endswith(".png") else args.png + ".dot"
        with open(dot_path, "w") as f:
            f.write(dot_content)
        print(f"DOT written to: {dot_path}")

        if args.png:
            render_dot(dot_path, args.png, "png")


if __name__ == "__main__":
    main()