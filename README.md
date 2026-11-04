# AM-DMF 双层解耦框架：C++ 核心复现

本项目依据 Jiang 等人的论文《A Two-Layer Decoupled Synthesis Framework for Uncertain Biochemical Protocols on Active-Matrix Digital Microfluidic Platforms》（IEEE TCAD，2026；DOI：[10.1109/TCAD.2026.3721419](https://doi.org/10.1109/TCAD.2026.3721419)），重建其中的**核心控制流程**。文档使用中文说明，但保留论文中的 DAG、CFG、basic block、Scheduler/Placer、Router、cost field 等术语。

论文给出了框架设计和伪代码，但未公开实现代码，也未完整给出基准生成程序。因此，本项目目前是**功能性重建**，不能宣称复现了论文表格中的数值。现阶段只实现原论文框架，尚未加入 PIBT。

## 已实现内容

- dynamic DAG：根据模拟的检测反馈，动态追加新的 basic block。
- Scheduler/Placer：检查操作依赖，在虚拟模块网格中选址，并在操作期间设置 3×3 障碍区。
- 两层之间的事件接口：发布 routing task、添加或清除操作障碍，以及接收液滴到达状态。
- Router：按目标计算 BFS cost field，逐步决定液滴移动，检查保守的液滴间距约束，并处理模拟的移动失败。
- 一个小型演示协议：两个并行混合操作、检测操作，以及检测后动态激活的输出或废弃分支。

## 编译与运行

需要 C++17 编译器和 CMake。在 PowerShell 中运行：

```powershell
cmake -S . -B build
cmake --build build --config Release
./build/Release/am_dmf.exe --self-test
./build/Release/am_dmf.exe --demo
```

演示程序会输出 execution cycles、已完成操作数、已激活 basic block 数、已完成 routing task 数及 Router 决策总耗时。**这些数值不能直接与论文表格比较**，因为演示协议及硬件参数尚未与论文实验对齐。

## 复现时补充的假设

| 细节 | 当前实现 | 与论文的关系 |
| --- | --- | --- |
| 虚拟模块布局 | 3×3 模块之间留出三格宽通道，候选位置按六格步长扫描 | 论文说明使用 3×3 虚拟模块及一个模块宽的干扰区，但未给出完整布局规则。 |
| 操作过程 | 输入液滴在操作启动时被消耗；混合和检测操作完成后，在模块中心生成一个液滴 | 属于对生化操作和合并过程的简化模拟。 |
| 操作时长 | 演示协议采用自定常数 | 论文未完整公开模块库参数。 |
| 液滴间距 | 对互不相关的 1×1 液滴要求切比雪夫距离至少为 2，并检查其他液滴的当前位置和已规划的下一位置 | 属于保守解释，尚未进行物理平台验证。 |
| cost field 更新 | 障碍变化后同步刷新 | 论文描述的是异步更新；当前实现未模拟并行计算。 |
| 死锁处理 | 根据 cost field 的梯度选下一步，使用两步代价打破平局 | 是对论文 two-step lookahead 规则的简化实现，不能据此保证无死锁。 |
| 移动失败 | 按每步失败概率随机模拟，之后重新检查安全性 | 尚未模拟论文中的电极位置相关可靠性场景。 |
| 分支结果 | 演示中使用固定反馈 | 论文未充分公开基准生成方法和执行路径分布，暂时无法逐项重建。 |

## 后续复现顺序

1. 用配置文件替代演示程序中的常数，明确芯片尺寸、module library、液滴入口和分支结果。
2. 按论文描述重建两类协议，以及 sequential batch 和 modified A* 两种 baseline。
3. 输出逐周期轨迹，检查 DAG 依赖、液滴间距、操作障碍和反馈事件。
4. 加入异步 cost field 维护，并在论文使用的芯片尺寸和液滴数量下测量性能。报告结果时同时列出仍需自行确定的参数。

当前代码是第一个可运行的里程碑。只有在基准和模块参数得到确认，或被明确固定为新假设之后，才适合尝试数值复现。
