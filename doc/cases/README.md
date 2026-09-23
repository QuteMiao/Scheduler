# Scheduler cases 说明

本目录描述 [`cases/`](../../cases/) 里 painter 接口用的静态 DAG 样例。

## `cases/` vs `cases2/`

| 目录 | 接口 | 说明 |
|------|------|------|
| [`cases/`](../../cases/) | painter | 本仓库主路径；文档与结构图只覆盖这里 |
| `cases2/` | 硬件队列 | 另一套接口，此处不展开 |

构建：

```bash
./build_all.sh qwen3_decode_layer_lt          # 单 case
./build_all.sh                                # 全部 cases/*.h
# 或：make 仅认 CASE=subgraph|total_graph（旧 Qwen3-14B 样例）
```

线程数取自头文件注释里的 `PAINTER_THREAD_CNT=N`（见 `build_all.sh`）。

## 索引

| 头文件 | 类型 | 任务数 | 屏障 dummy | 线程 | 文档 |
|--------|------|-------:|----------:|-----:|------|
| [`qwen3_decode_layer_lt.h`](../../cases/qwen3_decode_layer_lt.h) | capture | 278 | 5 | 1 | [qwen3_decode_layer.md](qwen3_decode_layer.md) |
| [`qwen3_decode_layer_ht.h`](../../cases/qwen3_decode_layer_ht.h) | capture | 1378 | 13 | 1 | 同上 |
| [`deepseek_v4_csa_lt.h`](../../cases/deepseek_v4_csa_lt.h) | capture | 74 | 3 | 1 | [deepseek_v4_csa.md](deepseek_v4_csa.md) |
| [`deepseek_v4_csa_ht.h`](../../cases/deepseek_v4_csa_ht.h) | capture | 246 | 7 | 1 | 同上 |
| [`fg_t480_p4_l8_c2_d5.h`](../../cases/fg_t480_p4_l8_c2_d5.h) | fakegraph | 3840 | — | 2 | [fakegraph_fg_t480.md](fakegraph_fg_t480.md) |
| [`fg_t480_p4_l8_c4_d5.h`](../../cases/fg_t480_p4_l8_c4_d5.h) | fakegraph | 3840 | — | 4 | 同上 |
| 其余 `fg_t480_*` / `qwen3_14b_*` | legacy / 族内变体 | — | — | — | 文件名见 `cases/`；不单开长文 |

任务数为 **逻辑 SPMD 节点** + `barrier_dummy`。`total_type`∈{0..5}，`total_spmd_cnt` 为 SPMD 宽度（非 SPMD 为 0）。

## SPMD 逻辑节点与依赖展开

逻辑 `pl.spmd` 在 deps 里常是单 task_id + `block_num`。生成器输出 **一个逻辑节点 / 一次 dispatch**，并写 `spmd_cnt`：

1. **异形均分（Qwen）**：`out` 每窗 50→5×10；`gate`/`up` 每窗按 K 85→5×17。
2. **对角 / 分桶 / 屏障**：逻辑边经 `classify_expand`；全员同步插入 `barrier_dummy`。
3. **保持**：`1→N` 广播、真·单消费者 `N→1` 汇聚。

实现见 [`tools/gen_capture_cases.py`](../../tools/gen_capture_cases.py)；单测 [`tests/test_irregular_spmd.py`](../../tests/test_irregular_spmd.py)、[`tests/test_spmd_expand.py`](../../tests/test_spmd_expand.py)。

依赖结构图（逻辑 SPMD 主图 + 捕获对照）由 [`tools/gen_capture_deps_svg.py`](../../tools/gen_capture_deps_svg.py) 生成：

```bash
python3 tools/gen_capture_deps_svg.py --all
python3 tools/gen_capture_deps_svg.py --all --check
```

主图节点格式：`name | N nodes | CUBE_SPMD | spmd_cnt=W`；捕获对照仍为 `N tasks / M blocks`。

## Capture 依赖口径

Qwen LT / HT 已在源样例中按数学依赖修正共享缓冲区追踪，并重新上板通过数值校验；当前 `deps.json` **未做后处理删边**。详见 [依赖复核报告](qwen3_dependency_review.md)。物理展开阶段的对角/分桶/dummy 与「JSON 删边」是不同层。

## 注意

- duration 单位为 **ns**；`barrier_dummy` 为 0。
- MIX（Cube+Vector）节点不建模核内 AIC/AIV 同步。
