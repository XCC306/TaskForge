# TaskForge v0.1.0

TaskForge 是一个规划中的 C++17 轻量并发任务处理项目。v0.1.0 先建立**可验证的串行 WAV 分析基线**，供后续线程池版本核对结果。当前代码没有线程池、取消或性能提升结论。

## 目前能做什么

- 命令行接收一个或多个 WAV 路径，依次分析并输出结果；任一文件失败时返回退出码 1，仍继续处理其他文件。
- 解析 RIFF/WAVE 块结构，包括非音频块和奇数长度块的填充字节。
- 支持未压缩 little-endian PCM、16 bit、单声道或双声道；对双声道合并所有采样计算统计量。
- 输出采样率、声道数、帧数、时长、峰值、RMS、Energy。采样先除以 32768：`Peak = max(|x|)`、`RMS = sqrt(sum(x²) / N)`、`Energy = sum(x²)`。Energy 随文件长度变化，不能当作平均响度。
- 当前一次性加载文件，设置 256 MiB 上限；暂不支持 MP3、M4A、24 bit PCM、float WAV 和 RF64。

## 目录

| 位置 | 职责 |
| --- | --- |
| `include/taskforge/wav_analyzer.hpp` | 对外的数据结构和分析函数声明 |
| `src/wav_analyzer.cpp` | WAV 块解析和 PCM 统计 |
| `app/main.cpp` | 命令行入口和错误码 |
| `tests/test_wav_analyzer.cpp` | 已知采样的结果测试与坏文件测试 |
| `samples/tone_440hz_mono_8k.wav` | 可直接运行的演示音频 |
| `CMakeLists.txt` | C++17 构建目标和 CTest 注册 |

## 在 Windows 11 上运行

继续使用 **VS Code** 编写和查看代码，在 `D:\Projects\TaskForge` 本地编译运行。VS Code 是编辑器；CMake 生成构建配置；MSVC（Visual Studio 的 C++ 编译工具）负责把 C++ 源码编译成 `.exe`。不使用 Conda，也不需要启动 VoiceTalk 的三个终端。

先在 PowerShell 检查：

```powershell
cmake --version
Get-Command cl, g++ -ErrorAction SilentlyContinue | Select-Object Name, Source
```

如果缺少 CMake 或 C++ 编译器，可通过 **Visual Studio Installer → Build Tools for Visual Studio → Desktop development with C++** 安装 MSVC、Windows SDK 和 CMake。只装 VS Code 的 C/C++ 扩展不会自动获得编译器。若普通 PowerShell 找不到 `cl`，但 Build Tools 已安装，可以使用 Visual Studio 的 **Developer PowerShell** 或让 CMake 的 Visual Studio 生成器查找已安装工具链。

在 VS Code 中通过“文件 → 打开文件夹”打开 `D:\Projects\TaskForge`，打开内置终端，执行：

```powershell
cd D:\Projects\TaskForge
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

不同生成器会把程序放在 `build\Debug\taskforge.exe` 或 `build\taskforge.exe`。先执行：

```powershell
Get-ChildItem .\build -Recurse -Filter taskforge.exe | Select-Object FullName
```

若前者存在，运行：

```powershell
.\build\Debug\taskforge.exe .\samples\tone_440hz_mono_8k.wav
```

若后者存在，运行 `./build/taskforge.exe` 并带同样的样本路径。也可把自己的 PCM16 WAV 路径作为参数。样本应显示 `sample_rate_hz=8000`、`channels=1`、`duration_s=0.100000`，并有非零 Peak、RMS 和 Energy。

`cmake -S . -B build` 读取当前目录的 `CMakeLists.txt` 并把生成文件放入 `build`；`cmake --build` 编译；`ctest` 执行注册的单元测试和命令行演示。Visual Studio 属于多配置生成器，故明确指定 `-C Debug`。

## 版本规划与停止点

| 版本 | 增量 | 验收 |
| --- | --- | --- |
| v0.1.0 | CMake/CTest、串行 WAV/PCM16 分析 | 正常、损坏与空数据测试；样本 CLI 可运行 |
| v0.2.0 | 固定线程池、阻塞队列、提交和异步结果 | 并发处理与 v0.1.0 串行结果一致 |
| v0.3.0 | 任务状态、取消、等待超时、优雅关闭 | 异常、取消与关闭竞争的测试通过 |
| v1.0.0 | 批处理使用体验、实测对比、回归与项目说明 | 干净目录可构建；测试和实际结果可复现 |

本项目属于本地单机 C++ 工程展示。当前 v0.1.0 只代表串行基线完成；后续功能须在对应版本实现和测试后再写入简历。
