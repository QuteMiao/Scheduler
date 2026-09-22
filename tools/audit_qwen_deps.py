#!/usr/bin/env python3
"""Check the single-layer Qwen B16-window mathematical DAG, without deleting edges.

The oracle matches the leaf tiling: hidden=5120, intermediate=17408,
output N/K=10/5, MLP N/K=17/5, direct output dispatch=24 blocks,
critical gate/up dispatch=6 blocks, down N/K=5/17. Submission order
within each kernel family identifies its loop iteration (deps omits scalars).
This is deliberately a case-specific oracle, not a general tensor-race prover.
"""

import argparse
from collections import defaultdict
import json
from pathlib import Path
import re

from gen_capture_cases import topological


def read_capture(folder):
    folder = Path(folder)
    deps = json.loads((folder / "deps.json").read_text())
    maps = sorted(folder.glob("name_map*.json"))
    if len(maps) != 1:
        raise ValueError(f"Expected one name map in {folder}")
    names = json.loads(maps[0].read_text())["callable_id_to_name"]
    return deps, names


def task_name(task, names):
    kernels = list(dict.fromkeys(names[str(k)] for k in task["kernel_ids"] if k >= 0))
    return " + ".join(kernels) if kernels else "dummy"


def role(task, names):
    name = task_name(task, names).split(" + ")[0]
    if name.startswith("attn_swpipe"):
        return "attention"
    return re.sub(r"_\d+$", "", name)


def wait_graph(deps):
    tasks = {str(t["task_id"]): t for t in deps["tasks"]}
    if len(tasks) != len(deps["tasks"]):
        raise ValueError("Duplicate task IDs")
    creators = {str(e["pred"]) for e in deps["edges"] if e["source"] == "creator"}
    pred = {tid: set() for tid in tasks.keys() | creators}
    for edge in deps["edges"]:
        if "wait" not in edge.get("flags", []):
            continue
        a, b = str(edge["pred"]), str(edge["succ"])
        if a not in pred or b not in tasks:
            raise ValueError(f"Unknown dependency endpoint {a} -> {b}")
        pred[b].add(a)
    order = topological(pred)
    return tasks, pred, order


def audit(deps, names):
    tasks, pred, order = wait_graph(deps)
    ancestors = {}
    for tid in order:
        ancestors[tid] = set(pred[tid])
        for parent in pred[tid]:
            ancestors[tid].update(ancestors[parent])
    errors = []
    checks = 0

    def require(condition, message):
        nonlocal checks
        checks += 1
        if not condition:
            errors.append(message)

    def waits(consumer, producers, label):
        missing = {t["task_id"] for t in producers} - ancestors[consumer["task_id"]]
        require(
            not missing,
            f"{label}: {consumer['task_id']} misses {len(missing)} producers: {sorted(missing)}",
        )

    windows = []
    for task in deps["tasks"]:
        if role(task, names) == "copy_hidden":
            windows.append([])
        if not windows:
            raise ValueError("Expected capture to start with copy_hidden")
        windows[-1].append(task)
    summaries = []
    for wi, window in enumerate(windows):
        by_role = defaultdict(list)
        for task in window:
            by_role[role(task, names)].append(task)
        label = f"window {wi}"
        expected = {
            "copy_hidden": 1,
            "x_gamma0": 1,
            "rms_recip": 1,
            "q_seed": 1,
            "kv_seed": 1,
            "mlp_out_seed": 1,
            "q_proj": 1,
            "k_proj": 1,
            "v_proj": 1,
            "attn_out_seed": 1,
            "attn_phase0": 1,
            "attention": 1,
            "out_proj": 27,
            "residual_rms_cast": 5,
            "post_rms_reduce": 1,
            "gate_proj": 60,
            "up_proj": 60,
            "gate_fold": 68,
            "up_fold": 68,
            "silu": 17,
            "down_proj": 85,
            "dcr_xgamma": 1,
            "copy_out": 1,
            "dummy": 11,
        }
        shape_ok = True
        for kind, count in expected.items():
            ok = len(by_role[kind]) == count
            require(ok, f"{label}: expected {count} {kind}, got {len(by_role[kind])}")
            shape_ok &= ok

        # This check also detects the historical pruned HT graph (without folds).
        for final in by_role["dcr_xgamma"]:
            waits(final, by_role["down_proj"], f"{label} final down fan-in")
            waits(final, by_role["residual_rms_cast"], f"{label} final residual fan-in")
            summaries.append(
                {
                    "window": wi,
                    "down_tasks": len(by_role["down_proj"]),
                    "down_reachable": sum(
                        t["task_id"] in ancestors[final["task_id"]]
                        for t in by_role["down_proj"]
                    ),
                }
            )
        if not shape_ok:
            continue

        def one(kind):
            return by_role[kind][0]

        waits(one("x_gamma0"), by_role["copy_hidden"], f"{label} x gamma")
        waits(one("rms_recip"), by_role["copy_hidden"], f"{label} input rms")
        for kind, seed in [
            ("q_proj", "q_seed"),
            ("k_proj", "kv_seed"),
            ("v_proj", "kv_seed"),
        ]:
            waits(one(kind), [one(seed), one("x_gamma0")], f"{label} {kind}")
        waits(
            one("attn_phase0"),
            [one(k) for k in ("q_proj", "k_proj", "v_proj", "rms_recip")],
            f"{label} QK norm/RoPE/cache append",
        )
        waits(
            one("attention"),
            [one("attn_phase0"), one("attn_out_seed")],
            f"{label} attention",
        )
        out = by_role["out_proj"]
        require(
            [t["block_num"] for t in out] == [1] * 26 + [24],
            f"{label} output dispatch layout",
        )
        for task in out:
            waits(
                task,
                [one("attention"), one("mlp_out_seed")],
                f"{label} output projection",
            )
        casts = by_role["residual_rms_cast"]
        for k, task in enumerate(casts):
            # Cast K slice spans two 512-column out tiles, five K partials each.
            producers = {min(i, 26) for i in range(k * 10, (k + 1) * 10)}
            waits(
                task,
                [out[i] for i in sorted(producers)] + [one("copy_hidden")],
                f"{label} cast {k}",
            )
        waits(
            one("post_rms_reduce"),
            out + [one("copy_hidden")],
            f"{label} post RMS all hidden columns",
        )
        for kind in ("gate", "up"):
            projs = by_role[f"{kind}_proj"]
            require(
                [t["block_num"] for t in projs] == [6] * 5 + [1] * 55,
                f"{label} {kind} dispatch layout",
            )

            def producer(k, n):
                return projs[k] if n < 6 else projs[5 + k * 11 + n - 6]

            for k in range(5):
                for n in range(17):
                    waits(
                        producer(k, n),
                        [casts[k], one("mlp_out_seed")],
                        f"{label} {kind}[{k},{n}]",
                    )
            folds = by_role[f"{kind}_fold"]
            for n in range(17):
                chain = folds[n * 4 : n * 4 + 4]
                waits(
                    chain[0],
                    [producer(0, n), producer(1, n), one("mlp_out_seed")],
                    f"{label} {kind} fold {n}:0",
                )
                for stage in range(1, 4):
                    waits(
                        chain[stage],
                        [chain[stage - 1], producer(stage + 1, n)],
                        f"{label} {kind} fold {n}:{stage}",
                    )
                waits(by_role["silu"][n], [chain[-1]], f"{label} silu {n} {kind}")
        for task in by_role["silu"]:
            waits(task, [one("post_rms_reduce")], f"{label} normalized SiLU")
        for i, task in enumerate(by_role["down_proj"]):
            waits(
                task,
                [by_role["silu"][i % 17], one("mlp_out_seed")],
                f"{label} down[{i // 17},{i % 17}]",
            )
        waits(one("copy_out"), by_role["dcr_xgamma"], f"{label} output publication")

        # Independent tiles must not inherit an accidental scheduling chain.
        for kind in (
            "down_proj",
            "out_proj",
            "residual_rms_cast",
            "gate_proj",
            "up_proj",
            "silu",
        ):
            family = {t["task_id"] for t in by_role[kind]}
            for tid in family:
                require(
                    not (ancestors[tid] & family),
                    f"{label} unnecessary {kind} serialization at {tid}",
                )
        for kind in ("gate_fold", "up_fold"):
            folds = by_role[kind]
            tile_of = {t["task_id"]: i // 4 for i, t in enumerate(folds)}
            for tid, tile in tile_of.items():
                require(
                    all(tile_of[a] == tile for a in ancestors[tid] & tile_of.keys()),
                    f"{label} cross-tile {kind} serialization at {tid}",
                )
    return {
        "logical_tasks": len(tasks),
        "edge_records": len(deps["edges"]),
        "unique_wait_edges": sum(map(len, pred.values())),
        "checks": checks,
        "windows": summaries,
        "errors": errors,
        "passed": not errors,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("folder", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = audit(*read_capture(args.folder))
    text = json.dumps(result, indent=2) + "\n"
    if args.output:
        args.output.write_text(text)
    print(text)
    raise SystemExit(0 if result["passed"] else 1)


if __name__ == "__main__":
    main()
