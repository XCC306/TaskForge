# TaskForge v0.2.0

TaskForge 是 C++17 轻量并发任务处理项目，以 WAV/PCM 音频分析为首个业务示例。v0.2.0 在已发布的 v0.1.0 串行基线上增加固定线程池、线程安全队列、异步结果及异常传递。

## 已实现能力

| 模块 | 当前行为 |
| --- | --- |
| WAV 分析 | 沿用 v0.1.0 的 RIFF/WAVE 块解析和 PCM16 统计逻辑 |
| ThreadPool | 固定数量的工作线程；线程安全 FIFO 队列；空队列时用条件变量等待 |
| submit | 接收函数、lambda 及参数，返回 `std::future<R>`；支持 move-only 函数和参数 |
| 异常 | `std::packaged_task` 保存任务异常；`future.get()` 在调用线程重新抛出 |
| 资源释放 | 对象析构时处理完已接受的任务，再 join 所有工作线程 |
| 命令行 | 默认串行；`--threads N` 并发分析；按输入顺序输出结果 |
| 测试 | WAV 回归、线程池行为、多线程提交、析构排空、串行/并发音频一致性 |

CLI 输出退出码：全部文件成功为 `0`；至少一个文件失败为 `1`，其他文件仍会处理；参数错误为 `2`。

## 范围与使用约定

- 支持单声道或双声道、未压缩 little-endian PCM16 WAV；统计量合并所有声道。MP3、M4A、24 bit、float WAV、RF64 暂不支持。
- 单个文件一次性加载内存，上限 256 MiB。并发时活动任务的文件内存会叠加，首次验收建议使用 2–4 个工作线程。
- 队列在 v0.2.0 中无容量上限；队列先进先出不保证任务完成顺序，CLI 主线程按输入顺序读取和输出结果。
- 析构前，调用方必须停止并完成所有 `submit` 调用。不能从工作任务内部销毁线程池；不要在所有工作线程内等待同一池的新任务，否则可能互相等待。
- 析构会等待任务结束；不会强杀不返回的任务。显式关闭、任务状态、协作式取消及等待超时将在 v0.3.0 实现。
- 本版验证正确性，不承诺并发一定更快。极短音频中线程创建和调度开销可能占主要部分；实测性能对比属于后续交付工作。

## 在 Windows 11 / Visual Studio 2026 上构建

使用已有 MSVC、Windows SDK 和 C++ CMake 工具。VS Code 用于编辑；命令可以在已加载开发环境的 PowerShell 中执行。VS 2026 生成器需要 CMake 4.2 或更新版本。

```powershell
cd D:\Projects\TaskForge
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
.\build\Debug\taskforge.exe --version
```

CTest 应通过 `wav_unit`、`thread_pool_unit`、`parallel_audio_unit`、`sample_cli`、`sample_cli_parallel` 共 5 个入口；单元入口内部包含多个检查。

如果打开新 PowerShell 后命令不可用，先加载已安装的工具：

```powershell
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products "*" -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw "未找到 MSVC 工具链" }
& "$vsPath\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
$cmakeDir = "$vsPath\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
if (-not (Test-Path "$cmakeDir\cmake.exe")) { throw "未找到 C++ CMake 工具组件" }
$env:Path = "$cmakeDir;$env:Path"
```

## 运行串行与并发示例

```powershell
.\build\Debug\taskforge.exe --serial .\samples\tone_440hz_mono_8k.wav
.\build\Debug\taskforge.exe --threads 4 .\samples\tone_440hz_mono_8k.wav .\samples\tone_440hz_mono_8k.wav
```

并发示例首先输出 `TaskForge v0.2.0 mode=parallel workers=4 files=2`，随后按输入顺序输出两组相同的样本指标。样本时长为 `0.100000` 秒，Peak 为 `0.500000`，RMS 为 `0.353551`，Energy 为 `99.998475`。

`--threads` 只接受 1–64 的整数；`--help` 显示用法。以 `-` 开头的文件名应放在 `--` 后面。

## C++ 接口及执行过程

```cpp
#include "taskforge/thread_pool.hpp"
#include "taskforge/wav_analyzer.hpp"

int main() {
    taskforge::ThreadPool pool(4); // 创建对象时启动固定工作线程。
    auto result = pool.submit(taskforge::analyze_wav,
                              std::filesystem::path("sample.wav"));
    auto stats = result.get();    // 等待结果；失败时在这里抛出任务异常。
    return stats.frames == 0 ? 1 : 0;
} // 离开作用域：排空已接受的任务，等待线程退出。
```

调用方将任务包装为 `packaged_task`，取出 future，然后在互斥锁保护下入队。工作线程用带谓词的 `condition_variable::wait` 等待，拿到任务后先释放队列锁，再执行计算。任务把结果或异常写入共享状态；调用方从 future 读取。函数和参数默认由任务按值持有，需要引用时显式传 `std::ref` 并保证被引用对象活得足够久。

## 目录与职责

| 文件 | 职责 |
| --- | --- |
| `include/taskforge/thread_pool.hpp` | 线程池接口、模板 submit、资源所有权约定 |
| `src/thread_pool.cpp` | 创建线程、队列、条件变量、析构排空和 join |
| `include/taskforge/wav_analyzer.hpp`、`src/wav_analyzer.cpp` | v0.1.0 音频数据结构与统计实现 |
| `app/main.cpp` | 参数解析、串行/并发选择、顺序输出、错误码 |
| `tests/test_thread_pool.cpp` | 返回值、move-only、异常、真实并发、多生产者、析构等待 |
| `tests/test_parallel_audio.cpp` | 72 个混合音频任务，在 1/4 线程下与串行结果核对 |
| `tests/test_wav_analyzer.cpp` | v0.1.0 已知采样与异常文件回归 |
| `CMakeLists.txt` | C++17 库、CLI、Threads::Threads 和 CTest 目标 |
| `UPGRADE_v0.2.0.md` | 覆盖方法、验收步骤和已有验证记录 |

采样归一化规则仍为 `x = PCM16 / 32768`；`Peak = max(|x|)`、`RMS = sqrt(sum(x²)/N)`、`Energy = sum(x²)`。Energy 随采样数变化，不是平均响度或带物理单位的能量。

## 版本规划

| 版本 | 目标 |
| --- | --- |
| v0.1.0 | 串行 WAV 正确性基线，发布提交 `04da7f5` |
| v0.2.0 | 固定线程池、队列、异步结果及串行/并发一致性 |
| v0.3.0 | 任务状态、取消、等待超时、明确关闭接口 |
| v1.0.0 | 可复现性能测试、完整交付文档与回归检查 |

本版本仍面向本地单机使用。新版本通过本机验收后再提交和打标签，性能数字只能使用实际测量结果。
