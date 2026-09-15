# Scheduler workload cases

Scheduler contains the `cases/` painter/dispatcher interface and the newer
`cases2/` hardware-queue interface. The captured workloads below provide the same
DAG in both formats; existing synthetic and Qwen cases retain their names.

| Case | Configuration | Logical calls | Cube nodes | Vector nodes | Total nodes |
|---|---|---:|---:|---:|---:|
| `qwen3_decode_layer_120` | batch 16, Attention BlockDim 120 | 166 | 545 | 312 | 857 |
| `deepseek_v4_csa_a` | batch 4, S=2, score 80, QK/PV 40 | 70 | 745 | 437 | 1182 |
| `deepseek_v4_csa_b` | batch 20, S=2, score 100, QK/PV 100 | 70 | 825 | 597 | 1422 |

A node is **one physical Cube or Vector execution record**, not one PyPTO
logical call. Mixed scopes contribute one Cube and two Vector records per
block in these captures. The `120` suffix identifies Qwen's Attention dispatch
width, not the hardware on which its timings were measured.

## Generate and build

Python 3.9+ and a GNU C11 compiler are sufficient. No private repository, PyPTO,
Simpler, Graphviz, or NetworkX is required to regenerate these cases.

```sh
python3 tools/gen_capture_cases.py          # regenerate all three, both formats
python3 tools/gen_capture_cases.py --check  # fail if checked-in output is stale
make check-captures                        # data tests, completion simulation, header compilation

make CASE=qwen3_decode_layer_120
make CASE=deepseek_v4_csa_a
make CASE=deepseek_v4_csa_b
# Original choices still work:
make CASE=subgraph
make CASE=total_graph

./build_all.sh                             # every cases/*.h, dispatchers 1 and 2
```

`make` retains the `scheduler` executable name and caches objects separately by
CASE and LOG. Switching back to a previously built case relinks the correct
objects. `build_all.sh` emits independently named binaries under `bin/`.

The `src2` include can be selected with a quoted macro, for example:

```sh
cc -std=gnu11 -I. -Iinclude -Iinclude2 \
  -DSCHEDULER_CASE2='"cases2/deepseek_v4_csa_b.h"' ...
```

This shows the include selection for an existing hardware build; it is not a
complete `src2` build command. Without the macro, the original cases2 Qwen
header remains selected. `make check-captures` compiles all six new headers in
isolation, including their atomic arrays.

## Data and interpretation

`workloads/<case>/` contains paired dependency data, kernel names, physical
execution rows, a manifest, and the generated node mapping. The manifests record
original capture SHA-256 values and model parameters. `mapping.json` records
hashes of the actual bundled generator inputs. The bundled swimlane is a subset
containing unchanged `metadata` and `aicore_tasks`; scheduler/host timing fields
are excluded because this conversion uses only kernel execution intervals.

Each duration is rounded to the nearest integer **nanosecond**, from the
capture's timestamp clock, with half ties rounded up. It includes kernel-internal
waits but excludes scheduler dispatch-to-start latency. Values are not averaged
across an operator or multiplied by batch. Arrays are descriptive: current
Scheduler code does not use these duration arrays to reproduce timed kernels.
A future hardware consumer must explicitly convert ns to its required unit;
the task descriptor's cycle field is a different interface.

Logical wait dependencies become all-to-all predecessor/completion constraints
between the physical nodes of the two calls. Duplicate edges are removed.
Recognized tensor-creation nodes are contracted, not emitted as dummy kernels.
No cross-core startup barrier, early-dispatch hint, or dummy task is added.

**Mixed is an approximation:** Cube/Vector nodes can schedule independently.
There is no intra-kernel pipe synchronization, paired-block identity, or atomic
reservation of Cube and Vector resources. A downstream call waits for all
physical records of each predecessor call. The model preserves these call-level
barriers but cannot predict actual mixed-kernel overlap or NPU performance.

All durations were captured on **24 AIC / 48 AIV**. The model tilings target
120 AIC, but this PR does not claim 120-AIC hardware execution. The current
hardware paths contain TODO register/kernel implementations and topology
assumptions; portable tests validate data and completion logic without executing
those paths. See [the capture report](doc/captured_workloads.md) for boundaries,
length effects, validation results, and generation details.
