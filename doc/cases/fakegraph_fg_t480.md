# Fakegraph `fg_t480` 参数族

## 用途

合成 DAG，用固定公式压测 painter 解依赖与多子图划分；与捕获无关。

## 来源

[`tools/gen_fake_graph.py`](../../tools/gen_fake_graph.py)。文件名编码：

| 字段 | 含义 | 本族取值 |
|------|------|----------|
| `t` | 每层任务数 | 480 |
| `p` | 每任务前驱窗口 | 4 |
| `l` | 层数（chain depth） | 8 |
| `c` | `PAINTER_THREAD_CNT` / 子图数 | 2 或 4 |
| `d` | 平均 duration 档（工具 `-d`） | 5 → 与生成时 avg 对应 |

总任务 = `t × l` = 3840。

## 规模（PR 详写的两份）

| 头文件 | 任务数 | Cube / Vector | 线程 |
|--------|-------:|--------------:|-----:|
| [`fg_t480_p4_l8_c2_d5.h`](../../cases/fg_t480_p4_l8_c2_d5.h) | 3840 | 1920 / 1920 | 2 |
| [`fg_t480_p4_l8_c4_d5.h`](../../cases/fg_t480_p4_l8_c4_d5.h) | 3840 | 1920 / 1920 | 4 |

同族其它 `fg_t480_p*_l*_c*_d5.h` 仅参数不同，不逐个出图。

## 结构图

全图节点多，下面两张为 header 直接渲染的 Graphviz 总览（可读缩放）：

### c2（2 子图）

![fg c2](figures/fg_t480_p4_l8_c2_d5.png)

### c4（4 子图）

![fg c4](figures/fg_t480_p4_l8_c4_d5.png)

参数关系示意：

```mermaid
flowchart TB
  params["t x l tasks"] --> layers["l layers"]
  layers --> pred["each node: up to p preds"]
  pred --> split["round-robin chunks -> c subgraphs"]
```

## 如何选用

```bash
./build_all.sh fg_t480_p4_l8_c2_d5
./build_all.sh fg_t480_p4_l8_c4_d5
```

重生成示例：

```bash
python3 tools/gen_fake_graph.py -t 480 -p 4 -l 8 -c 2 -d 5 \
  --png doc/cases/figures/fg_t480_p4_l8_c2_d5.png
# 写出 header 时按工具当前 CLI / 仓库约定落地到 cases/
```

## 注意

- duration：**ns**（合成值）。
- 无 tensormap 伪依赖问题。
