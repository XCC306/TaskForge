# TaskForge v0.3.0

TaskForge 是本地 C++17 轻量并发任务处理项目，以 WAV/PCM 音频统计为业务示例。本版以已发布的 v0.2.0（a957b34）为基线，补齐任务状态、取消、等待超时和显式关闭。

## 功能与范围

| 能力 | 实际行为 |
| --- | --- |
| 音频分析 | PCM16 WAV 采样率、声道、帧数、时长、Peak、RMS、Energy |
| 线程池 | 固定工作线程，无界 FIFO 队列；函数在池锁外执行 |
| 兼容接口 | 原 `submit(fn, args...)` 仍返回 `std::future<R>` |
| 任务句柄 | `submit_task()` / `submit_cancellable()` 返回 move-only `TaskHandle<R>` |
| 状态 | `queued`、`running`、`succeeded`、`failed`、`cancelled` |
| 排队取消 | future 立即就绪，get 抛出 TaskCancelled，函数不执行 |
| 运行中取消 | 请求协作退出，函数返回后才发布取消终态 |
| 等待超时 | wait_for 返回 timeout，不改变任务状态 |
| 关闭 | Drain 排空，或 CancelPending 取消队列中任务；最终 join 工作线程 |

本版不加入优先级、持久化、有界队列或强制终止线程。性能测量与工程收尾留到 v1.0.0。

## Windows 构建与运行

使用已有的 VS 2026 MSVC、Windows SDK 和 C++ CMake 工具。VS Code 负责编辑，命令在已加载开发工具环境的 PowerShell 中运行。VS 2026 生成器需要 CMake 4.2 或更新版本。

~~~powershell
cd D:\Projects\TaskForge
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure

.\build\Debug\taskforge.exe --version
.\build\Debug\taskforge.exe --serial .\samples\tone_440hz_mono_8k.wav
.\build\Debug\taskforge.exe --threads 4 .\samples\tone_440hz_mono_8k.wav .\samples\tone_440hz_mono_8k.wav
.\build\Debug\taskforge_lifecycle_demo.exe

cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
~~~

CTest 应通过 8 项入口。Windows 的 pthread 探测即使失败，只要最后是 `Found Threads: TRUE` 就已找到线程支持。新终端找不到 cl/cmake 时，沿用 v0.2.0 的开发工具环境加载方法，无需创建 Conda 环境。

Linux/GCC 使用：

~~~bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 2
ctest --test-dir build --output-on-failure
~~~

## 状态与取消规则

正常路径是 queued -> running -> succeeded/failed。排队取消为 queued -> cancelled。运行中取消请求被接受后，状态仍为 running，直到回调退出后再发布 cancelled。

- `cancel()` 首次接受请求返回 true；重复请求或已经终态返回 false。
- 取消与结果发布共用任务锁：取消先被接受，则丢弃后续值或异常；结果先发布，则取消失败。
- 不检查令牌的运行中函数仍会执行到返回；取消不能撤销已发生的副作用。
- 回调抛出 TaskCancelled 也标记取消；其他未被取消的异常标记 failed，并由 get 原样重抛。
- get 只能消费一次；消费后 valid 为 false，但 status/cancel 仍可用于查询控制状态。
- 单个句柄只允许一个结果消费者；可以由另一个线程调用 status/cancel。不要并发移动、销毁句柄，或并发重复 get。
- 丢弃句柄不会自动取消或阻塞。排队取消的条目保留至工作线程取出或池关闭，捕获对象可能继续存活。
- 终态表示结果已发布，不代表工作线程完成所有包装对象清理；工作线程退出以 wait 返回为准。

## 等待超时的定义

`TaskHandle::wait_for(duration)` 只限制这一次等待，不设置任务执行截止时间，不自动取消。超时后可继续等待、取结果或明确请求取消。

命令行提供“超时后请求取消”的可选策略：

~~~powershell
.\build\Debug\taskforge.exe --threads 2 --wait-ms 5000 .\samples\tone_440hz_mono_8k.wav
~~~

--wait-ms 接受 1–600000 的整数，只能配合 --threads。预算从主线程开始等待当前文件的结果时起算，每个文件各有一次预算，不是从提交起算，也不是整批总时限。超时则报错、请求取消并继续处理其他结果；退出前仍等待所有工作线程。

短样本可能早已完成，不要求它必然超时。请用 lifecycle_demo 稳定演示“超时后仍可继续取结果”。

## C++ 最小完整示例

~~~cpp
#include "taskforge/thread_pool.hpp"
#include "taskforge/wav_analyzer.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>

int main() {
    taskforge::ThreadPool pool(2); // 在 main 内创建对象，构造时启动工作线程。
    auto task = pool.submit_cancellable(
        taskforge::analyze_wav_cancellable,
        std::filesystem::path("sample.wav"));

    if (task.wait_for(std::chrono::seconds(2)) == std::future_status::timeout) {
        task.cancel(); // 请求退出，不能强杀阻塞系统调用。
    }
    int code = 0;
    try {
        const auto stats = task.get();
        std::cout << "rms=" << stats.rms << "\n";
    } catch (const taskforge::TaskCancelled&) {
        std::cerr << "Task cancelled\n";
        code = 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        code = 1;
    }
    pool.shutdown(taskforge::ShutdownMode::Drain);
    return code;
}
~~~

submit_task 调普通函数；submit_cancellable 自动把 CancellationToken 放在第一个参数位置。函数和参数默认按值持有，支持 move-only；显式引用使用 std::ref，并保证对象活到函数退出。

兼容入口 analyze_wav(path) 保留。新入口 analyze_wav_cancellable(token, path) 在打开文件前、64 KiB 读取的间隔、WAV 块解析和每 4096 个样本统计处检查令牌。单次阻塞 I/O 和内存分配无法被强行打断。

## 关闭与所有权

| 接口 | 行为 |
| --- | --- |
| close(Drain) | 拒绝新任务，继续处理已接收任务，不等待线程退出 |
| close(CancelPending) | 拒绝提交，取消仍在队列中的任务；已取出的任务继续 |
| wait() | 先按 Drain 关闭（若未关闭），再 join 所有工作线程 |
| shutdown(mode) | close(mode) 然后 wait() |
| 析构 | 默认 Drain 并 join，保持 v0.2.0 行为 |

关闭与入队共用池锁，保证提交要么接受、要么拒绝。Drain 可以升级为 CancelPending；反向调用不恢复取消任务。关闭和等待可重复调用，外部 join 调用串行化。

任务可以调用 close，但同池工作线程调用 wait/shutdown 会抛 logic_error，避免等待自身。不得在工作线程销毁池；销毁前必须结束其他线程对池的所有 API 调用。不要让所有工作线程等待同池尚未执行的新任务。回调若永不返回，join/析构可能一直等待。

## 测试入口与代码职责

| CTest 入口 | 内容 |
| --- | --- |
| wav_unit | WAV/PCM 统计与非法输入 |
| thread_pool_unit | v0.2.0 返回值、所有权、异常、多生产者、析构排空 |
| parallel_audio_unit | 72 个混合文件任务与串行一致 |
| task_lifecycle_unit | 状态、排队/运行取消、超时、关闭、并发边界与 WAV 取消 |
| lifecycle_demo | 同步门控制的确定性演示 |
| cli_regressions | 参数、退出码、串行/并行输出和坏文件隔离 |
| sample_cli | 默认串行样本 |
| sample_cli_parallel | 四线程样本 |

新增 task.hpp 定义状态、令牌、控制状态及句柄；thread_pool.hpp 放模板接口，thread_pool.cpp 放队列和 join；examples/lifecycle_demo.cpp 是可执行演示；tests/test_task_lifecycle.cpp 与 tests/check_cli.cmake 是新回归。

退出码：成功 0，文件失败/取消/等待超时 1，参数错误 2。固定样本指标不变：duration_s=0.100000、peak=0.500000、rms=0.353551、energy=99.998475。

## 音频范围与后续路线

支持单/双声道 little-endian PCM16 WAV，统计合并声道。每文件最多 256 MiB，仍整体加载内存，并发活动文件内存叠加。MP3、M4A、24-bit/float WAV 和 RF64 不支持。Energy 是 PCM16/32768 归一化样本的平方和，不带物理能量单位。

| 版本 | 核心目标 |
| --- | --- |
| v0.1.0 | 串行 WAV 基线，04da7f5 |
| v0.2.0 | 固定线程池和批量并行，a957b34 |
| v0.3.0 | 状态、取消、等待超时、明确关闭 |
| v1.0.0 | 性能复现、完整回归、文档和最终交付 |

验证记录见 VALIDATION_v0.3.0.md。Windows/MSVC 仍需用户本机验收；不宣称未经测量的加速比或数据竞争检测结论。

标准库参考：[Microsoft future](https://learn.microsoft.com/en-us/cpp/standard-library/future-class?view=msvc-170)、[condition_variable](https://learn.microsoft.com/en-us/cpp/standard-library/condition-variable-class?view=msvc-170)。
