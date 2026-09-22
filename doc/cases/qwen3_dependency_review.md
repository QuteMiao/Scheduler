# Qwen3 LT / HT 数学依赖复核（2026-09-22）

结论：旧 SVG 不能作为正确依赖图使用。旧 Scheduler 依赖存在漏边，LT fold 还被错误并行化；V200 源目录保留的原始依赖与 Scheduler 清洗副本不同。现已从两个源样例修正依赖表达，重跑并通过数值校验，重新生成同源的 deps、header 和 SVG。

## 已确认的问题

| 问题 | 证据 | 后果与修正 |
|---|---|---|
| 最终残差漏等 down partial | 旧 LT 的 `dcr_xgamma`（task `4294967337`）仅能到达 85 个 down producer 中的 1 个；HT 五窗口各同样漏 84 个 | 删掉 down 串行链后，仅等“最后一个”不再充分。源代码收集全部 85 个 down TaskId，最终 SPMD 显式等待它们及 5 个 residual cast |
| LT fold 的真实 RAW 被删 | `8589934729 → 8589934730` 的 explicit 和 tensormap 边均被删；共 102 条 fold 顺序关系被替换为 fold→SiLU，新增边备注 `stages are not ordered` | 后一 fold 读取前一 fold 写入的 sum，不能并行。保留每个 tile 的四阶段链，仅不同 tile 并行 |
| 源与清洗副本混用 | 旧叶目录 LT/HT 分别 3439/13772 条边；Scheduler 副本 2920/11967 条 | 新叶目录和 workloads 使用重跑的原始捕获，不做后处理删边 |
| HT 产物落后于源代码 | 开始审查时两个 decode 源码相同，都有 gate/up fold；旧 HT capture 没有 fold | 重跑后 HT 逻辑任务 1400→2080，物理 Vector 600→1280 |
| header 生成丢 dummy 路径 | 旧 `gen_capture_cases.py` 将无物理记录的 dummy 当成普通任务，查 `ids[dummy]` 得空集 | 将无执行 kernel 的任务连同 creator 一起收缩，保持所有传递依赖 |
| SVG 展示不完整 | 旧 LT 48 节点/75 边，HT 36 节点/32 边；遗漏最终残差等关键阶段，HT 只取高频子集 | 新图完整展示 52/260 个 executable kernel 分组、113/579 条聚合边；MIX 包含 AIC+AIV 名称，不再误当成只有 AIC |

LT 清洗实际删除 621 条旧记录（519 tensormap + 102 explicit），新增 102 条 fold→SiLU 记录，净减少 519。manifest 的净边数不能证明清洗正确。

## 数学基准与依赖范围

单个窗口 B=16，H=5120，I=17408。以下是本样例使用的分块与运算语义；BF16 转换位置保持原代码，通过原 golden 容差校验，不宣称有限精度下变换与实数运算逐位等价。

1. 输入归一化拆成 `x_gamma = x * gamma` 和 `r = 1/sqrt(mean(x²)+eps)`。Q/K/V 投影消费 x_gamma；Phase0 必须等待 Q/K/V 与 r，再完成 Q/K norm、RoPE 和 cache append；attention 等待 Phase0 及输出初始化。
2. 输出投影每个 N tile 为 `O[n] = sum_k A[k] W_o[k,n]`，10 个 512-column N tiles × 5 个 K splits。seed 必须先清零；不同 K partial 用原子加，允许并行。现有 26 个单 block dispatch 加 1 个 24-block dispatch 保持不变。
3. `R = x + O` 分成 5 个 1024-column cast。每个 cast 必须等待覆盖自己两个 N tiles 的全部 K partial。post RMS reduction 读取全部 H 列，必须等待全部输出投影。
4. 对每个 MLP N tile，`G[n] = sum_k (R[k]*gamma_post[k]) W_gate[k,n]`，U 同理。5 个 K splits 写独立 partial buffers；每个任务只需等待自己的 cast 和初始化。6 个前沿 N tiles 合成 SPMD，其余 11 个单独提交。
5. 对固定 n，fold 执行 `s1=p0+p1; s2=s1+p2; s3=s2+p3; s4=s3+p4`。这三条 RAW 链必须保留，17 个 N tiles 之间没有数据相交。gate 和 up 两支总计 `17×3×2=102` 条阶段顺序关系。
6. `Z[n] = SiLU(r_post*G[n]) * (r_post*U[n])`，必须同时等待对应 gate/up 最后 fold 和全列 post RMS reduction。down 计算 `D[m] = sum_n Z[n] W_down[n,m]`，5 个输出 tiles × 17 个输入 tiles；每个 partial 只需等待对应 SiLU 和 seed，使用原子加。
7. 最终 `Y = R + D` 是一个覆盖 5 个 block 的 SPMD dispatch，因此必须等待全部 85 个 down partial 和 5 个 residual cast，再由自动追踪将结果发布给 copy_out / 后续层。

实现对上述内部 partial / sum / residual / MLP 缓冲区使用 `manual_dep=True`，并补齐显式 TaskId 关系。保留一般输入输出、归一化标量、attention 状态的自动追踪，不使用整段 manual scope，不仅凭同名或 WAW 标签删边。共享 cache / 外部输出的跨窗口存储约束仍按 runtime 捕获保留；本次没有假定任意外部 page/slot 映射都互不重叠。

## 源样例补丁

源样例属于独立的 V200-benchmark 仓库。本 PR 附带 [源代码补丁](../../workloads/qwen_source_dependency_fix.patch) 和 [修改前后 SHA256](../../workloads/qwen_source_dependency_fix.json)，只包含此次两个叶目录的依赖修正。补丁基于审查开始时的工作区源码；请先核对 before_sha256，不能假定任意 V200 版本都匹配。

在匹配的 V200-benchmark checkout 中执行 `git apply --check <补丁路径>`，再 `git apply <补丁路径>`。Scheduler 中的原始捕获、生成器、数学检查、header 和 SVG 均随 PR 提供；下述完整上板日志及旧版本备份保留在原工作区 artifacts 目录。

## 重跑与证据

工作区证据目录：`artifacts/qwen_deps_review_20260922/`。

- `before/lt`、`before/ht`：原源码、叶目录原始捕获、Scheduler 清洗副本、旧 SVG/header 及 simpler 代码备份。
- `lt/`、`ht/`：修改后完整生成代码、原始 deps/name map、物理 swimlane。
- `lt.log`、`ht.log`：golden 校验与运行日志。
- `*_audit.json`：新图数学检查；`*_old_audit.json`：旧图反例。旧 HT 未匹配当前 fold 结构的检查失败属于版本差异，down 漏等是独立验证的问题。
- workloads manifest 记录捕获目录、任务 ID、源文件 SHA256 和数学检查摘要；mapping 记录全部输入文件 SHA256。

| | LT | HT |
|---|---:|---:|
| 队列任务 | `task_20260922_002139_15137011592` | `task_20260922_002153_16336620731` |
| serving case | LT_BATCH16 | HT_BATCH80 |
| device / platform | 3 / a2a3 | 4 / a2a3 |
| seed / layers | 1234 / 1 | 1234 / 1 |
| 数值校验 | PASS，out `[16,5120]` BF16 | PASS，out `[80,5120]` BF16 |
| 原始新边记录 | 1608 | 8064 |
| 数学检查 | 902 PASS | 4510 PASS |
| 物理节点 / 边 | 943 / 34764 | 3275 / 75904 |

运行使用各叶 `run_benchmark.py -p a2a3 -d "$TASK_DEVICE" --enable-chip-swimlane 4 --enable-dep-gen --dep-output-dir <独立捕获目录>`，没有 `--skip-golden`。容差沿用样例 `atol=0.003, rtol=0.003, max_error_ratio=0.02`。依赖捕获与干净 timing 是 runner 的两次独立执行；同一次 runner invocation 的映射和记录配套使用。

HT 相比 LT 每窗口 attention block 数不同（24 vs 120），不能将物理节点或时间直接按五倍推算。旧 `concurrency_analysis.json` 和 `sim120_swimlane/` 是历史性能产物，本次未作为依赖复核证据，也未更新为新性能报告。

## 自动验证与限制

- 数学检查直接读取原始捕获；验证必需生产者的可达性、tile 间无多余串行链、kernel dispatch 的 block 数和分块数量。
- 该检查是此固定单层 tiling 的 oracle，依赖 kernel 家族内提交顺序映射循环索引；deps 未包含标量 n/k 实参，所以同时审核了源码与生成 orchestration，不能把脚本当作任意算子的通用竞争检测器。
- 测试覆盖人为删除 84 条 down 汇合、删除 fold RAW、dummy 收缩、MIX 三个物理 kernel slot，以及新捕获全部逻辑可达关系与物理展开图的精确等价。
- LT/HT 两种 header 格式均编译并在主机执行前驱/后继一致性与拓扑解依赖检查；完整 Scheduler 的 1/2 dispatcher 构建通过（需 `-D_GNU_SOURCE`）。没有执行访问设备 MMIO 的 Scheduler 主程序，不将主机 DAG 验证描述成设备调度性能测试。
- SVG 可由同一捕获重复生成并 `--check`；它保留全部聚合 wait 边，creator/dummy 收缩保留路径，边标注为去重逻辑任务对数量。
