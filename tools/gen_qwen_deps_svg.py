#!/usr/bin/env python3
"""Render all executable kernel groups and wait edges from a Qwen capture.

Creator/dummy nodes are contracted with their paths retained. No edge pruning
or frequency threshold is applied. Edge labels count unique logical task pairs;
MIX kernels are one logical group listing both AIC and AIV names.
"""

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import subprocess

from audit_qwen_deps import read_capture, task_name, wait_graph


def render_dot(deps, names, title, digest):
    tasks, pred, order = wait_graph(deps)
    executed = {
        tid
        for tid, task in tasks.items()
        if any(k >= 0 for k in task["kernel_ids"]) and task["block_num"] > 0
    }
    contracted = {}
    for tid in order:
        parents = set()
        for parent in pred[tid]:
            parents.update({parent} if parent in executed else contracted[parent])
        contracted[tid] = parents
    label = {tid: task_name(tasks[tid], names) for tid in executed}
    counts = Counter(label.values())
    blocks = Counter()
    for tid in executed:
        blocks[label[tid]] += tasks[tid]["block_num"]
    edges = Counter(
        (label[parent], label[tid]) for tid in executed for parent in contracted[tid]
    )
    quote = json.dumps
    graph_label = (
        f"{title}: captured execution dependencies (no post-hoc pruning)\n"
        f"All {len(counts)} kernel groups; {len(edges)} aggregated edges; "
        f"{len(executed)} executable logical tasks\n"
        "Nodes: logical tasks / blocks per kernel slot. Edges: unique task pairs.\n"
        "Creator/dummy paths contracted. MIX AIC+AIV shown together.\n"
        f"deps.json SHA256: {digest}"
    )
    lines = [
        "digraph G {",
        'graph [rankdir=LR, fontname="Helvetica", fontsize=12, labelloc=t,',
        f"label={quote(graph_label)}];",
        'node [shape=box, style="rounded,filled", fillcolor="#edf4fb", fontname="Helvetica", fontsize=10];',
        'edge [fontname="Helvetica", fontsize=8, color="#64748b"];',
    ]
    for name in sorted(counts):
        node_label = (
            name.replace(" + ", "\n+ ")
            + f"\n{counts[name]} tasks / {blocks[name]} blocks"
        )
        lines.append(f"{quote(name)} [label={quote(node_label)}];")
    for (a, b), count in sorted(edges.items()):
        lines.append(f"{quote(a)} -> {quote(b)} [label={quote(str(count))}];")
    return "\n".join(lines + ["}", ""])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("folder", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    deps, names = read_capture(args.folder)
    digest = hashlib.sha256((args.folder / "deps.json").read_bytes()).hexdigest()
    dot = render_dot(deps, names, args.folder.name, digest)
    svg = subprocess.check_output(["dot", "-Tsvg"], input=dot.encode())
    if args.check:
        if args.output.read_bytes() != svg:
            raise SystemExit(f"Stale SVG: {args.output}")
    else:
        args.output.write_bytes(svg)
    print(f"{args.output}: {'verified' if args.check else 'generated'}")


if __name__ == "__main__":
    main()
