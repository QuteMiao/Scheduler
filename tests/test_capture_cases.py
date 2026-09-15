"""Portable data/header tests: never access Scheduler hardware registers."""

import importlib.util
import json
from pathlib import Path
import random
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "generator", ROOT / "tools/gen_capture_cases.py"
)
gen = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gen)


def arrays(text):
    return {
        name: [int(x) for x in re.findall(r"-?\d+", values)]
        for name, values in re.findall(r"\b(\w+)\[\d+\]\s*=\s*\{([^}]+)\};", text)
    }


class CapturedCases(unittest.TestCase):
    def test_roundtrip_and_host_completion(self):
        for folder in sorted((ROOT / "workloads").iterdir()):
            with self.subTest(case=folder.name):
                nodes = gen.load_graph(folder)
                a = arrays((ROOT / "cases" / f"{folder.name}.h").read_text())
                b = arrays((ROOT / "cases2" / f"{folder.name}.h").read_text())
                for x, y in [
                    ("total_task_id", "total_task_id"),
                    ("total_type", "task_type"),
                    ("total_duration", "task_duration"),
                    ("pre_cnt", "task_pre_cnt"),
                    ("pre_idx", "pre_idx"),
                    ("predecessors", "predecessors"),
                    ("suc_cnt", "task_suc_cnt"),
                    ("suc_idx", "task_suc_idx"),
                    ("successors", "task_successors"),
                ]:
                    self.assertEqual(a[x], b[y], (x, y))
                n = len(nodes)
                self.assertEqual(a["total_task_id"], list(range(n)))
                self.assertEqual(a["total_type"], [x["type"] for x in nodes])
                self.assertEqual(a["total_duration"], [x["duration_ns"] for x in nodes])
                predecessors, successors = [], []
                for i in range(n):
                    parents = a["predecessors"][
                        a["pre_idx"][i] : a["pre_idx"][i] + a["pre_cnt"][i]
                    ]
                    children = a["successors"][
                        a["suc_idx"][i] : a["suc_idx"][i] + a["suc_cnt"][i]
                    ]
                    self.assertEqual(parents, nodes[i]["predecessors"])
                    predecessors.extend((p, i) for p in parents)
                    successors.extend((i, c) for c in children)
                    xor = 0
                    for p in parents:
                        xor ^= p
                    self.assertEqual(xor, b["task_pre_xor"][i])
                self.assertEqual(set(predecessors), set(successors))
                self.assertEqual(len(predecessors), len(set(predecessors)))
                # Completion callbacks against the parsed C arrays, in several ready orders.
                for seed in range(3):
                    rng = random.Random(seed)
                    left = b["task_pre_cnt"].copy()
                    ready = [i for i in range(n) if left[i] == 0]
                    done = set()
                    while ready:
                        i = ready.pop(rng.randrange(len(ready)))
                        parents = b["predecessors"][
                            b["pre_idx"][i] : b["pre_idx"][i] + b["task_pre_cnt"][i]
                        ]
                        self.assertTrue(set(parents) <= done)
                        self.assertNotIn(i, done)
                        done.add(i)
                        for c in b["task_successors"][
                            b["task_suc_idx"][i] : b["task_suc_idx"][i]
                            + b["task_suc_cnt"][i]
                        ]:
                            left[c] -= 1
                            self.assertGreaterEqual(left[c], 0)
                            if left[c] == 0:
                                ready.append(c)
                    self.assertEqual(len(done), n)
                    self.assertEqual(left, [0] * n)
                for path, content in gen.outputs(ROOT, folder.name).items():
                    self.assertEqual(path.read_text(), content, str(path))

    def test_headers_compile(self):
        for folder in ("cases", "cases2"):
            for manifest in sorted((ROOT / "workloads").glob("*/manifest.json")):
                name = manifest.parent.name
                code = f'#include "{folder}/{name}.h"\nint main(void) {{ return total_task_cnt < 1; }}\n'
                subprocess.run(
                    [
                        "cc",
                        "-std=gnu11",
                        "-D_GNU_SOURCE",
                        "-DPAINTER_THREAD_CNT=1",
                        "-Iinclude",
                        "-I.",
                        "-x",
                        "c",
                        "-fsyntax-only",
                        "-",
                    ],
                    input=code,
                    text=True,
                    cwd=ROOT,
                    check=True,
                    capture_output=True,
                )


class InvalidCaptures(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name)
        self.deps = {
            "tasks": [
                {"task_id": "10", "kernel_ids": [0, -1, -1], "block_num": 2},
                {"task_id": "20", "kernel_ids": [-1, 1, -1], "block_num": 1},
            ],
            "tensors": [{"tensor_id": "tensor"}],
            "edges": [
                {
                    "pred": "10",
                    "succ": "15",
                    "source": "creator",
                    "tensor_id": "tensor",
                    "flags": ["wait"],
                },
                {
                    "pred": "15",
                    "succ": "20",
                    "source": "creator",
                    "tensor_id": "tensor",
                    "flags": ["wait"],
                },
            ],
        }
        self.raw = {
            "metadata": {"core_types": ["aic", "aiv"], "clock_freq_hz": 50_000_000},
            "aicore_tasks": [
                [0, 10, 1, 10, 20],
                [0, 10, 2, 20, 40],
                [1, 20, 3, 40, 70],
            ],
        }

    def load(self):
        data = {
            "deps.json": self.deps,
            "Chip_swimlane_records.json": self.raw,
            "name_map.json": {"callable_id_to_name": {"0": "cube", "1": "vector"}},
            "manifest.json": {
                "expected_logical_tasks": 2,
                "expected_cube_nodes": 2,
                "expected_vector_nodes": 1,
            },
        }
        for name, value in data.items():
            (self.path / name).write_text(json.dumps(value))
        return gen.load_graph(self.path)

    def test_management_path_and_rounding(self):
        nodes = self.load()
        self.assertEqual(nodes[2]["predecessors"], [0, 1])
        self.assertEqual([n["duration_ns"] for n in nodes], [200, 400, 600])

    def test_missing_record(self):
        self.raw["aicore_tasks"].pop()
        with self.assertRaisesRegex(ValueError, "Incomplete"):
            self.load()

    def test_duplicate_record(self):
        self.raw["aicore_tasks"].append(self.raw["aicore_tasks"][0])
        with self.assertRaisesRegex(ValueError, "duplicate"):
            self.load()

    def test_cycle(self):
        self.deps["edges"].append({"pred": "20", "succ": "10", "flags": ["wait"]})
        with self.assertRaisesRegex(ValueError, "cycle"):
            self.load()

    def test_unknown_reference(self):
        self.deps["edges"].append({"pred": "99", "succ": "20", "flags": ["wait"]})
        with self.assertRaisesRegex(ValueError, "Unexplained"):
            self.load()

    def test_duplicate_edges(self):
        self.deps["edges"] *= 2
        self.assertEqual(self.load()[2]["predecessors"], [0, 1])

    def test_invalid_duration(self):
        self.raw["aicore_tasks"][0][-1] = -1
        with self.assertRaises(ValueError):
            self.load()

    def test_duration_overflow(self):
        self.raw["aicore_tasks"][0][-1] = 2**40
        with self.assertRaisesRegex(ValueError, "Duration"):
            self.load()


if __name__ == "__main__":
    unittest.main()
