"""Regression checks for the mathematical fan-in and capture conversions."""

import copy
import json
from pathlib import Path
import subprocess

import pytest

from audit_qwen_deps import audit, read_capture, role, wait_graph
from gen_capture_cases import load_graph, topological
from gen_qwen_deps_svg import render_dot

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize("kind", ["lt", "ht"])
def test_capture_matches_math_and_physical_graph(kind):
    folder = ROOT / "workloads" / f"qwen3_decode_layer_{kind}"
    deps, names = read_capture(folder)
    result = audit(deps, names)
    assert result["passed"], result["errors"]
    nodes = load_graph(folder)
    physical = {n["id"]: set(n["predecessors"]) for n in nodes}
    order = topological(physical)
    ancestors = {}
    for tid in order:
        ancestors[tid] = set(physical[tid])
        for parent in physical[tid]:
            ancestors[tid].update(ancestors[parent])
    by_logical = {}
    for node in nodes:
        by_logical.setdefault(node["logical_task_id"], []).append(node["id"])
    # All logical dependency paths (including dummy paths) survive conversion.
    _, parents, logical_order = wait_graph(deps)
    logical_ancestors = {}
    for tid in logical_order:
        logical_ancestors[tid] = set(parents[tid])
        for parent in parents[tid]:
            logical_ancestors[tid].update(logical_ancestors[parent])
        expected = {p for a in logical_ancestors[tid] for p in by_logical.get(a, [])}
        for pid in by_logical.get(tid, []):
            assert ancestors[pid] == expected


def test_missing_down_fanin_is_rejected():
    deps, names = read_capture(ROOT / "workloads/qwen3_decode_layer_lt")
    deps = copy.deepcopy(deps)
    roles = {t["task_id"]: role(t, names) for t in deps["tasks"]}
    # Reproduce post-hoc pruning: leave only one of 85 down producers at final.
    down = [
        e
        for e in deps["edges"]
        if roles.get(e["pred"]) == "down_proj" and roles.get(e["succ"]) == "dcr_xgamma"
    ]
    removed = {id(e) for e in down[1:]}
    deps["edges"] = [e for e in deps["edges"] if id(e) not in removed]
    result = audit(deps, names)
    assert any("final down fan-in" in e and "misses 84" in e for e in result["errors"])


def test_missing_fold_step_is_rejected():
    deps, names = read_capture(ROOT / "workloads/qwen3_decode_layer_lt")
    deps = copy.deepcopy(deps)
    folds = [t for t in deps["tasks"] if role(t, names) == "gate_fold"]
    a, b = folds[0]["task_id"], folds[1]["task_id"]
    deps["edges"] = [e for e in deps["edges"] if (e["pred"], e["succ"]) != (a, b)]
    assert any("gate fold 0:1" in e for e in audit(deps, names)["errors"])


def test_dummy_paths_and_mix_slots_are_preserved(tmp_path):
    tasks = [
        {"task_id": "1", "kernel_ids": [0, 1, 1], "block_num": 1},
        {"task_id": "2", "kernel_ids": [-1, -1, -1], "block_num": 0},
        {"task_id": "3", "kernel_ids": [-1, 2, -1], "block_num": 1},
    ]
    deps = {
        "tasks": tasks,
        "tensors": [],
        "edges": [
            {"pred": "1", "succ": "2", "source": "explicit", "flags": ["wait"]},
            {"pred": "2", "succ": "3", "source": "explicit", "flags": ["wait"]},
        ],
    }
    names = {"0": "mix_aic", "1": "mix_aiv", "2": "consumer"}
    raw = {
        "metadata": {"core_types": ["aic", "aiv", "aiv"], "clock_freq_hz": 1000000000},
        "aicore_tasks": [
            [0, 1, 1, 1, 10],
            [1, 1, 1, 1, 10],
            [2, 1, 1, 1, 10],
            [1, 3, 2, 11, 20],
        ],
    }
    manifest = {
        "expected_logical_tasks": 3,
        "expected_cube_nodes": 1,
        "expected_vector_nodes": 3,
    }
    for filename, data in [
        ("deps.json", deps),
        ("name_map.json", {"callable_id_to_name": names}),
        ("Chip_swimlane_records.json", raw),
        ("manifest.json", manifest),
    ]:
        (tmp_path / filename).write_text(json.dumps(data))
    nodes = load_graph(tmp_path)
    assert nodes[-1]["predecessors"] == [0, 1, 2]
    dot = render_dot(deps, names, "test", "digest")
    assert '"mix_aic + mix_aiv" -> "consumer" [label="1"]' in dot
    assert "dummy" not in dot.split("node [", 1)[1]


@pytest.mark.parametrize("kind", ["lt", "ht"])
@pytest.mark.parametrize("version", [1, 2])
def test_generated_c_headers_resolve_all_dependencies(kind, version, tmp_path):
    folder = "cases" if version == 1 else "cases2"
    binary = tmp_path / "check_header"
    subprocess.run(
        [
            "cc",
            "-std=c11",
            "-O2",
            "-I",
            str(ROOT),
            "-I",
            str(ROOT / "include"),
            "-DPAINTER_THREAD_CNT=1",
            f"-DHEADER_FORMAT={version}",
            f'-DHEADER="{folder}/qwen3_decode_layer_{kind}.h"',
            str(ROOT / "tests/check_capture_header.c"),
            "-o",
            str(binary),
        ],
        check=True,
        capture_output=True,
    )
    result = subprocess.run([str(binary)], check=True, capture_output=True, text=True)
    assert "all dependencies resolved" in result.stdout


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
