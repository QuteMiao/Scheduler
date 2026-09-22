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

| 头文件 | 类型 | 任务数 | Cube / Vector | 线程 | 文档 |
|--------|------|-------:|--------------:|-----:|------|
| [`qwen3_decode_layer_lt.h`](../../cases/qwen3_decode_layer_lt.h) | capture | 943 | 495 / 448 | 1 | [qwen3_decode_layer.md](qwen3_decode_layer.md) |
| [`qwen3_decode_layer_ht.h`](../../cases/qwen3_decode_layer_ht.h) | capture | 3275 | 1995 / 1280 | 1 | 同上 |
| [`deepseek_v4_csa_a.h`](../../cases/deepseek_v4_csa_a.h) | capture | 535 | 313 / 222 | 1 | [deepseek_v4_csa.md](deepseek_v4_csa.md) |
| [`deepseek_v4_csa_b.h`](../../cases/deepseek_v4_csa_b.h) | capture | 3459 | 1833 / 1626 | 1 | 同上 |
| [`fg_t480_p4_l8_c2_d5.h`](../../cases/fg_t480_p4_l8_c2_d5.h) | fakegraph | 3840 | 1920 / 1920 | 2 | [fakegraph_fg_t480.md](fakegraph_fg_t480.md) |
| [`fg_t480_p4_l8_c4_d5.h`](../../cases/fg_t480_p4_l8_c4_d5.h) | fakegraph | 3840 | 1920 / 1920 | 4 | 同上 |
| 其余 `fg_t480_*` / `qwen3_14b_*` | legacy / 族内变体 | — | — | — | 文件名见 `cases/`；不单开长文 |

## Capture 去伪依赖

Qwen LT / HT 已在源样例中按数学依赖修正共享缓冲区追踪，并重新上板通过数值校验；当前 `deps.json` **未做后处理删边**。旧清洗曾误删 fold 顺序依赖并漏补 down 汇合，不能继续使用。详见 [依赖复核报告](qwen3_dependency_review.md)。图与 header 使用同一次捕获，保留 dummy 路径；物理节点数和 duration 来自 onboard swimlane。

CSA 两个 capture 仍保留此前清洗版本，本次 Qwen 复核不对其正确性作结论。

## 注意

- duration 单位为 **ns**。
- MIX（Cube+Vector）节点不建模核内 AIC/AIV 同步。
