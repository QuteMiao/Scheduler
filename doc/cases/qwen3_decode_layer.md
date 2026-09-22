# Qwen3 decode layer（LT / HT）

Painter 静态 DAG，来自 Qwen3 单层 decode 的上板捕获，用于软件调度 / 解依赖压力测试。

## 来源与规模

2026-09-22：先修正源样例依赖，再重跑通过 golden 校验。当前数据不做 JSON 后处理删边。
数学依据、旧图问题和证据见 [依赖复核报告](qwen3_dependency_review.md)。

| | LT | HT |
|--|---:|---:|
| V200 源目录 | `qwen3_decode_layer/lt_batch16` | `qwen3_decode_layer/ht_batch80` |
| public batch / 16-row 窗口数 | 16 / 1 | 80 / 5 |
| 逻辑任务（含 dummy） | 416 | 2080 |
| 原始捕获边记录 | 1608 | 8064 |
| 去重 wait 边（含 creator / dummy） | 1580 | 7914 |
| 物理节点 | 943 | 3275 |
| Cube / Vector | 495 / 448 | 1995 / 1280 |
| 展开后物理边 | 34764 | 75904 |
| PAINTER_THREAD_CNT | 1 | 1 |

HT 旧捕获未包含当前源码的 fold 阶段；本次每窗口新增 136 个 Vector fold 任务，物理节点从 2595 变为 3275。LT / HT 的 attention 分别使用 120 / 24 blocks，因此物理节点数并非简单五倍。

输入为叶目录的 `deps.json`、`chip_swimlane_records.json` 和 `name_map.json`，完整同步到 `Scheduler/workloads/`；manifest 记录运行与源码指纹。生成器将 creator / dummy 路径收缩，再将逻辑依赖展开到全部物理执行记录。

## 结构图

完整展示 executable kernel 分组，未按频次截断；点击原 SVG 可放大。

- 节点显示逻辑任务数及每个 kernel slot 的 block 总数。
- MIX 显示 AIC 与 AIV 两个名称；逻辑上仍是一次 dispatch。
- 边标签是去重后的逻辑任务对数量，不是物理边数或原始 JSON 记录数。
- creator / dummy 不显示为节点，但其传递路径保留；图中也保留实际捕获的共享存储约束。

### LT

![qwen3_decode_layer_lt deps](figures/qwen3_decode_layer_lt_deps.svg)

### HT

![qwen3_decode_layer_ht deps](figures/qwen3_decode_layer_ht_deps.svg)

## 复核与生成

在 Scheduler 目录执行：

```bash
python3 tools/audit_qwen_deps.py workloads/qwen3_decode_layer_lt
python3 tools/audit_qwen_deps.py workloads/qwen3_decode_layer_ht
python3 tools/gen_capture_cases.py --check qwen3_decode_layer_lt qwen3_decode_layer_ht
python3 tools/gen_qwen_deps_svg.py workloads/qwen3_decode_layer_lt doc/cases/figures/qwen3_decode_layer_lt_deps.svg --check
python3 tools/gen_qwen_deps_svg.py workloads/qwen3_decode_layer_ht doc/cases/figures/qwen3_decode_layer_ht_deps.svg --check
python3 -m pytest tests/test_qwen_capture.py -q
CPPFLAGS='-D_GNU_SOURCE -Iinclude -Isrc -I.' ./build_all.sh qwen3_decode_layer_lt qwen3_decode_layer_ht
```

去掉生成命令的 `--check` 可重新生成对应产物。duration 单位为 ns；MIX 物理节点不建模核内 AIC/AIV 同步。C header 测试使用主机 DAG 遍历，不访问 Scheduler 的设备 MMIO。
