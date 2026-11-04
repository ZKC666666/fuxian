# AM-DMF 双层框架的 C++ 复现

本工程依据论文 [A Two-Layer Decoupled Synthesis Framework for Uncertain Biochemical Protocols on Active-Matrix Digital Microfluidic Platforms](https://doi.org/10.1109/TCAD.2026.3721419) 逐步复现。论文给出了框架和伪代码，没有公开完整实现与实验生成程序。工程使用 C++17，文档使用中文。

当前 `main` 分支处于 **v0.1：协议与运行状态**。此前可运行但简化较多的探索原型保存在 Git 标签 `v0.0-prototype`，不作为论文数值复现结果。

## 先理解两个图

- **CFG**：协议可能走的流程。例如检测通过后输出，失败后丢弃。两条路都预先写在 CFG 中，但运行时只会选择实际发生的一条。
- **block DAG**：一个流程块内部的操作与输入关系。例如混合产生液滴，检测必须等这滴液体产生后才能开始。
- **dynamic DAG**：运行中已激活流程块的操作集合。检测结果到来时，才把选中的后续流程块加入其中。同一个流程块因重试而执行两次，会产生两套不同的运行实例。

## v0.1 已实现

| 部分 | 职责 |
| --- | --- |
| `include/dmf/protocol.hpp`、`src/protocol.cpp` | 定义 CFG、流程块、操作和液滴输入引用；检查块内 DAG 是否有环、分支目标和输入引用是否有效。 |
| `include/dmf/dynamic_dag.hpp`、`src/dynamic_dag.cpp` | 记录已激活的流程块、每次执行的操作编号、液滴是否可用，并根据检测结果在下一周期激活分支。 |
| `src/main.cpp` | 用“混合 → 检测 → 输出或丢弃”的小例子打印流程。完成事件由示例程序手动提供。 |
| `tests/runtime_tests.cpp` | 检查依赖、分支选择、重试产生新实例、液滴不可重复使用，以及错误反馈不改变运行状态。 |

这版中，操作是否**可开始**由两个条件共同决定：生成输入液滴的操作已经完成，而且所需液滴当前可用。开始操作会占用输入液滴；完成操作产生的新液滴到下一周期才可供后续操作使用。检测结果也在下一周期激活选中的流程块。

演示输出中的 `cycle` 目前只是这些事件的顺序编号。操作时长和真实芯片控制周期尚未接入，因此不能把它当作论文中的执行周期数。

## 编译与运行

需要 C++17 编译器和 CMake。在已配置好编译器的终端中执行：

```powershell
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

演示程序接受 `output` 或 `waste` 参数。使用 Visual Studio 多配置生成器时，例如：

```powershell
.\build\Debug\am_dmf.exe output
.\build\Debug\am_dmf.exe waste
```

若使用单配置生成器，可执行文件通常位于 `build` 目录下。当前终端如果找不到 `cmake`，可在 Visual Studio 或 CLion 中打开本目录的 `CMakeLists.txt` 进行构建。

## 当前边界与后续步骤

v0.1 只检查协议和运行状态的先后关系。**还没有**选址、操作时长、电极位置、真实液滴移动、Router、障碍区或执行周期统计。示例中的操作完成与检测结果由程序手动注入，因此其输出不能与论文表格的 execution cycles 比较。

论文没有精确规定所有周期内事件的先后边界。当前固定为“本周期完成的操作，其输出液滴和检测分支从下一周期开始可用”；这是工程假设，后续实验会记录它对结果的影响。

下一版将实现上层的就绪操作处理、模块库、虚拟模块选址和占用检查。完成论文原路由及双层联动后，再构造基准和对照方法；PIBT 改进在原论文版本稳定后单独开展。
