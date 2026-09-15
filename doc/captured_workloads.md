# Captured Qwen3 and DeepSeek CSA workloads

## Workload definitions

The input snapshots originate from the corresponding V200-benchmark workloads.
The per-case manifests identify the source benchmark, the source PyPTO-Lib base
commit, and exact capture hashes. Source base commits alone do not identify local
model changes; the paired capture snapshots are the generation authority.

| Workload | Expanded nodes | Expanded unique wait edges | Min kernel ns | Max kernel ns |
|---|---:|---:|---:|---:|
| Qwen3 decode layer, Attention 120 | 857 | 52604 | 920 | 67740 |
| DeepSeek V4 CSA A | 1182 | 28981 | 580 | 22940 |
| DeepSeek V4 CSA B | 1422 | 45001 | 700 | 71260 |

Qwen keeps its adjusted output projection (10 groups of 10), Gate and Up
projections (17 groups of 5 each), and Attention=120 configuration. SiLU waits
for its corresponding Gate/Up producers through the captured dependencies.
Its batch-16 fixture uses seed 1234 and per-request sequence lengths in 1..4096;
128 request/KV-head work items are distributed over 120 Attention blocks.

CSA A uses B=4, S=2 and starts `[8192, 0, 2, 3]`. CSA B uses B=20, S=2
and cycles the deduplicated mainline boundary set across 20 requests:
`[8192,0,2,3,7,127,128,255,511,8192,0,2,3,7,127,128,255,511,8192,0]`.
This is a deterministic boundary fixture, not randomly generated lengths.
Its paired capture and both header formats were regenerated from V200-benchmark
commit `9603974`. The source uses `chip_swimlane_records.json`; the bundled
snapshot retains the Scheduler generator's existing `Chip_swimlane_records.json` name.
KV length is start+2. B retains A's non-length-related dispatch widths, with
additional token work inside each block. Its score has 200 logical page-split
work items across 100 blocks; QK/PV also has 200 work items across 100 blocks.
A/B are workload definitions, not a controlled performance comparison.

The V200 SVG counts one mixed SPMD block as one physical task (CSA B: 1022).
Scheduler retains separate Cube/Vector execution nodes (CSA B: 1422), since
these have distinct resources and measured durations. This refresh does not
change that abstraction or infer hardware block pairing from execution order.

Sequence length affects visible score pages, valid sparse blocks, compression
boundary work and per-record duration. The exporter keeps those duration
variations. It does not manufacture equal-length tasks or extrapolate 24-core
timings to a 120-core machine.

## Conversion contract

1. Validate physical record identity `(core_id, register_task_id)`, core type,
   time bounds and counts against every logical call's BlockDim/kernel slots.
2. Sort by logical ID, then core type, start tick, core ID and register ID; assign
   contiguous node IDs. The mapping's ordinal is a record order, **not** a
   recovered SPMD block ID. No Cube/Vector pairing is inferred from duration.
3. Use each record's `(end_tick - start_tick) / clock_freq_hz` execution duration,
   converted to signed-int ns with checked range. Zero-duration records retain
   their zero; missing/duplicate records and unsupported slot identity fail.
4. Deduplicate wait edges and validate a DAG. A non-executing creator is recognized
   only from a creator edge referring to a known tensor. Contract its paths;
   reject unexplained dependency endpoints instead of silently dropping them.
5. Expand each remaining call dependency to a complete bipartite set of physical
   predecessor/successor nodes. Keep redundant but valid call dependencies; no
   optional transitive reduction or fake completion task changes the graph.
6. Emit a single-painter `cases/` graph and the equivalent `cases2/` graph from the
   same nodes and edges. Derive successor arrays, counts, offsets, and cases2
   predecessor XOR from that edge set. Initialize execution state and coordinates
   to zero, independent of the original hardware placements.

The relatively large edge arrays encode full call completion constraints rather
than block-local dataflow. This is intentional and can increase dependency
processing cost versus a scheduler with native SPMD-group completion support.

## Validation

Validation on Linux/aarch64:

- Generator reproducibility and byte comparison for all six headers and mappings.
- 10 portable unit tests, including parsing the emitted C arrays, cross-format
  comparison, complete predecessor/successor consistency and three randomized
  ready-order completion traversals per workload.
- Synthetic tests for creator-path contraction, duplicate edges, missing and
  duplicate physical records, cycles, unknown dependencies and invalid/overflowing
  duration values.
- GNU C11 syntax checks for all six headers.
- All 15 existing/new `cases/` workloads build with dispatcher counts 1 and 2
  (30 binaries); unused-array and other pre-existing warnings are not hidden.
- Makefile checks cover all five named CASE selections and switching back to a
  cached case without reusing another case's executable.

No NPU register execution or 120-AIC performance test is represented by these
checks. `cases2` data are checked independently of its unfinished hardware path.

Two build defects encountered during validation are fixed narrowly: expose Linux
CPU-affinity macros with `_GNU_SOURCE`; restore the missing `static` on
`fg_t480_p8_l8_c6_d5.h`'s `total_task_coord`, matching its generator and peer cases.
No workload values or dispatcher algorithms are changed by those fixes.
