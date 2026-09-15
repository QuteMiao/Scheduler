#!/usr/bin/env python3
"""Generate both Scheduler header formats from paired deps/physical captures.

Standard library only. Durations are integer nanoseconds, not device cycles.
Mixed scopes are an independent-resource abstraction, not a hardware replay.
"""

import argparse
from collections import Counter, defaultdict, deque
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(path):
    return json.loads(path.read_text())


def topological(pred):
    successors = {node: [] for node in pred}
    left = {node: len(parents) for node, parents in pred.items()}
    for node, parents in pred.items():
        for parent in parents:
            if parent not in pred:
                raise ValueError(f"Unknown predecessor {parent}")
            successors[parent].append(node)
    ready = deque(sorted(node for node in pred if not left[node]))
    order = []
    while ready:
        node = ready.popleft()
        order.append(node)
        for child in sorted(successors[node]):
            left[child] -= 1
            if not left[child]:
                ready.append(child)
    if len(order) != len(pred):
        raise ValueError("Dependency graph contains a cycle")
    return order


def load_graph(folder):
    deps, raw, names = [
        read(folder / f)
        for f in ("deps.json", "Chip_swimlane_records.json", "name_map.json")
    ]
    manifest = read(folder / "manifest.json")
    tasks = {int(t["task_id"]): t for t in deps["tasks"]}
    if len(tasks) != len(deps["tasks"]):
        raise ValueError("Duplicate logical task ID")
    # Tensor creation nodes have no execution record; retain their paths only.
    tensor_ids = {str(t["tensor_id"]) for t in deps.get("tensors", [])}
    known = set(tasks) | {
        int(e["pred"])
        for e in deps["edges"]
        if e.get("source") == "creator" and str(e.get("tensor_id")) in tensor_ids
    }
    pred = {tid: set() for tid in known}
    for edge in deps["edges"]:
        if "wait" not in edge.get("flags", []):
            continue
        a, b = int(edge["pred"]), int(edge["succ"])
        if a not in known or b not in known:
            raise ValueError(f"Unexplained dependency endpoint {a} -> {b}")
        pred[b].add(a)
    order = topological(pred)
    contracted = {}
    for tid in order:
        parents = set()
        for parent in pred[tid]:
            parents.update({parent} if parent in tasks else contracted[parent])
        contracted[tid] = parents
    cores = raw["metadata"]["core_types"]
    hz = int(raw["metadata"]["clock_freq_hz"])
    if hz <= 0:
        raise ValueError("Invalid capture clock")
    records = defaultdict(list)
    identities = set()
    for row in raw["aicore_tasks"]:
        if len(row) not in (5, 6):
            raise ValueError("Unsupported physical record schema")
        core, tid, reg, start, end = map(int, row[:5])
        if (
            (core, reg) in identities
            or tid not in tasks
            or not 0 <= core < len(cores)
            or not 0 < start <= end
        ):
            raise ValueError(f"Invalid/duplicate physical record {row}")
        identities.add((core, reg))
        typ = cores[core]
        if typ not in ("aic", "aiv"):
            raise ValueError(f"Unsupported core type {typ}")
        duration = ((end - start) * 1_000_000_000 + hz // 2) // hz
        if not 0 <= duration <= 2**31 - 1:
            raise ValueError("Duration does not fit signed int nanoseconds")
        records[tid].append(
            dict(
                core_id=core,
                register_task_id=reg,
                start_tick=start,
                duration_ns=duration,
                type=0 if typ == "aic" else 1,
            )
        )
    nodes, ids = [], defaultdict(list)
    for tid in sorted(tasks):
        task = tasks[tid]
        if len({k for k in task["kernel_ids"][1:] if k >= 0}) > 1:
            raise ValueError(
                "Capture lacks kernel-slot identity for distinct AIV kernels"
            )
        expected = {
            0: task["block_num"] * (task["kernel_ids"][0] >= 0),
            1: task["block_num"] * sum(k >= 0 for k in task["kernel_ids"][1:]),
        }
        if Counter(r["type"] for r in records[tid]) != Counter(
            {k: v for k, v in expected.items() if v}
        ):
            raise ValueError(f"Incomplete physical capture for {tid}")
        for ordinal, record in enumerate(
            sorted(
                records[tid],
                key=lambda r: (
                    r["type"],
                    r["start_tick"],
                    r["core_id"],
                    r["register_task_id"],
                ),
            )
        ):
            kid = next(
                k
                for k in (
                    task["kernel_ids"][:1]
                    if record["type"] == 0
                    else task["kernel_ids"][1:]
                )
                if k >= 0
            )
            nid = len(nodes)
            ids[tid].append(nid)
            nodes.append(
                dict(
                    id=nid,
                    logical_task_id=str(tid),
                    record_ordinal=ordinal,
                    kernel_id=kid,
                    kernel_name=names["callable_id_to_name"][str(kid)],
                    **record,
                )
            )
    for node in nodes:
        node["predecessors"] = sorted(
            p
            for parent in contracted[int(node["logical_task_id"])]
            for p in ids[parent]
        )
    graph = {n["id"]: set(n["predecessors"]) for n in nodes}
    topological(graph)
    if [
        len(tasks),
        sum(n["type"] == 0 for n in nodes),
        sum(n["type"] == 1 for n in nodes),
    ] != [
        manifest[k]
        for k in (
            "expected_logical_tasks",
            "expected_cube_nodes",
            "expected_vector_nodes",
        )
    ]:
        raise ValueError("Manifest counts do not match capture")
    return nodes


def array(name, values, ctype="int"):
    # ISO C cannot express a zero-sized array; counts still describe zero edges.
    values = values or [0]
    lines = [", ".join(map(str, values[i : i + 16])) for i in range(0, len(values), 16)]
    return (
        f"static {ctype} {name}[{len(values)}] = {{\n    "
        + ",\n    ".join(lines)
        + "\n};\n"
    )


def render(name, nodes, version):
    n = len(nodes)
    pre = [x["predecessors"] for x in nodes]
    suc = [[] for _ in nodes]
    for child, parents in enumerate(pre):
        for parent in parents:
            suc[parent].append(child)

    def flat(rows):
        counts, offsets, data = [], [], []
        for row in rows:
            counts.append(len(row))
            offsets.append(len(data))
            data.extend(row)
        return counts, offsets, data

    pc, pi, pp = flat(pre)
    sc, si, ss = flat(suc)
    types = [x["type"] for x in nodes]
    durations = [x["duration_ns"] for x in nodes]
    guard = f"CAPTURE_{name.upper()}_V{version}_H"
    out = f'/* Generated by tools/gen_capture_cases.py; do not edit.\n * duration unit: ns; physical execution records; PAINTER_THREAD_CNT=1.\n * Mixed Cube/Vector nodes do not model intra-kernel synchronization.\n */\n#ifndef {guard}\n#define {guard}\n#include <stdint.h>\n#include <stdatomic.h>\n#include "conf.h"\nstatic int total_task_cnt = {n};\n'
    out += array("total_task_id", list(range(n)), "uint32_t")
    if version == 1:
        for key, values, ctype in [
            ("total_type", types, "char"),
            ("total_duration", durations, "int"),
            ("task_id", list(range(n)), "uint32_t"),
            ("pre_cnt", pc, "int"),
            ("pre_idx", pi, "int"),
            ("predecessors", pp, "int"),
            ("suc_cnt", sc, "int"),
            ("suc_idx", si, "int"),
            ("successors", ss, "int"),
            ("total_pre_cnt", pc, "_Alignas(CACHE_LINE_SIZE) int"),
            ("total_task_state", [0] * n, "char"),
            ("total_task_coord", [0] * n, "int"),
        ]:
            out += array(key, values, ctype)
        out += (
            """typedef struct subgraph {
    uint32_t task_cnt;
    uint32_t *task_id;
    int *pre_cnt, *pre_idx, *predecessors;
    int *suc_cnt, *suc_idx, *successors, *total_pre_cnt;
} subgraph;
static subgraph test_graph[PAINTER_THREAD_CNT] = {
    {"""
            + f"{n}, task_id, pre_cnt, pre_idx, predecessors, suc_cnt, suc_idx, successors, total_pre_cnt"
            + "},\n};\n"
        )
    else:
        xors = []
        for parents in pre:
            value = 0
            for parent in parents:
                value ^= parent
            xors.append(value)
        for key, values, ctype in [
            ("task_type", types, "char"),
            ("task_duration", durations, "int"),
            ("pre_idx", pi, "int"),
            ("task_pre_xor", xors, "int"),
            ("predecessors", pp, "int"),
            ("task_suc_cnt", sc, "int"),
            ("task_suc_idx", si, "int"),
            ("task_successors", ss, "int"),
            ("task_state", [0] * n, "char"),
            ("task_coord", [0] * n, "int"),
            ("task_pre_cnt", pc, "_Alignas(CACHE_LINE_SIZE) atomic_int"),
        ]:
            out += array(key, values, ctype)
    return out + f"#endif /* {guard} */\n"


def outputs(root, name):
    folder = root / "workloads" / name
    nodes = load_graph(folder)
    summary = {
        "name": name,
        "nodes": len(nodes),
        "cube_nodes": sum(n["type"] == 0 for n in nodes),
        "vector_nodes": sum(n["type"] == 1 for n in nodes),
        "edges": sum(len(n["predecessors"]) for n in nodes),
        "duration_unit": "ns",
        "input_sha256": {
            p: hashlib.sha256((folder / p).read_bytes()).hexdigest()
            for p in (
                "deps.json",
                "Chip_swimlane_records.json",
                "name_map.json",
                "manifest.json",
            )
        },
        "nodes_mapping": [
            {k: v for k, v in n.items() if k != "predecessors"} for n in nodes
        ],
    }
    return {
        root / "cases" / f"{name}.h": render(name, nodes, 1),
        root / "cases2" / f"{name}.h": render(name, nodes, 2),
        folder / "mapping.json": json.dumps(summary, indent=2) + "\n",
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("names", nargs="*")
    parser.add_argument(
        "--check", action="store_true", help="Check generated files without writing"
    )
    args = parser.parse_args()
    for name in args.names or sorted(
        p.name for p in (ROOT / "workloads").iterdir() if p.is_dir()
    ):
        for path, content in outputs(ROOT, name).items():
            if args.check:
                if not path.exists() or path.read_text() != content:
                    raise SystemExit(f"Stale generated file: {path}")
            else:
                path.write_text(content)
        print(f"{name}: {'verified' if args.check else 'generated'}")


if __name__ == "__main__":
    main()
