# TaskForge 最终工程验证（保留 v0.3.0）

## 本轮定位

功能基线保持已发布的 v0.3.0：

- 提交：06d95c344240a130c28e2e1b378e3183b81c6e9f。
- 用户本机 Windows/MSVC Debug、Release 的 CTest 均已 8/8 通过。
- 两种配置的生命周期演示均已通过。
- 本轮增加测量工具和记录说明，不修改 C++ 核心、CMake、版本号或标签；不创建 v0.4.0。

这是增量工具包，只有三个新文件：

| 文件 | 职责 |
| --- | --- |
| scripts/benchmark_taskforge.py | 生成合成 WAV，启动现有 Release CLI，核对结果并测量 |
| tests/test_benchmark_tool.py | 验证工具本身的 12 项 Python 标准库测试 |
| docs/FINAL_VERIFICATION.md | 本说明 |

它不是完整项目源码包。把包内 TaskForge 文件夹的内容合并到现有 D:\Projects\TaskForge 即可。已有 C++ 文件和 README 不需要替换。

## 1. 准备分支和工具

~~~powershell
cd D:\Projects\TaskForge
git status --short
~~~

确认当前工作区干净后：

~~~powershell
git switch -c chore/final-verification
~~~

然后复制本增量包中的 scripts、tests、docs 到项目根目录，保留各目录原有文件。

Python 仅用于测量、数据整理和工具测试，不参与 C++ 程序运行，也不改变项目的 C++ 定位。需要 Python 3.10 或更新版本，只使用标准库，无需 pip 安装或创建新环境。

~~~powershell
python --version
~~~

## 2. 工具自检

~~~powershell
python -m unittest discover -s tests -p test_benchmark_tool.py -v
if ($LASTEXITCODE -ne 0) { throw "验证工具自检失败，请停止" }
~~~

预期 Ran 12 tests / OK。它与 C++ CTest 的 8 项入口分别计数。

测试覆盖：PCM 独立解码、生成可重复性、中文及空格路径、结果数量与输入顺序、错误指标/NaN/重复字段拒绝、预热排除、中位数与吞吐量、模式顺序轮换、配置识别、错误退出拒绝，以及测量失败时保存 failed 状态且不生成成功汇总。

## 3. 确认现有 Release 程序

~~~powershell
.\build\Release\taskforge.exe --version
~~~

应为 TaskForge v0.3.0。你刚完成双配置干净构建和验收，核心文件没有再改动时，不需要重复构建。

若以后重新覆盖源码，再执行：

~~~powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release --clean-first
ctest --test-dir build -C Release --output-on-failure
~~~

工具通过可执行文件位置和 CMake 缓存识别 Release；这不能替代正确构建。它还会记录二进制 SHA256 和核心源文件 SHA256，拒绝测量过程中发生的文件变化。

## 4. 运行正式测量

保持相同电源模式，避免同时进行其他大型编译、下载或 CPU 密集工作。合成数据约 103 MiB，全部写入项目所在盘的 build/verification 下。

~~~powershell
python .\scripts\benchmark_taskforge.py --exe .\build\Release\taskforge.exe
if ($LASTEXITCODE -ne 0) { throw "性能验证失败，请检查输出目录中的记录" }
~~~

默认验证：

| 组别 | 文件数 | 单文件时长 | 格式 | 合计文件量 |
| --- | ---: | ---: | --- | ---: |
| tiny | 32 | 0.1 秒 | 16 kHz / 单声道 / PCM16 | 约 0.10 MiB |
| medium | 16 | 10 秒 | 48 kHz / 单声道 / PCM16 | 约 14.65 MiB |
| large_stereo | 8 | 60 秒 | 48 kHz / 双声道 / PCM16 | 约 87.89 MiB |

每组对比真正串行模式，以及 1、2、4 个工作线程的线程池模式。每模式先预热 1 次，再正式测量 5 次。共 12 次预热和 60 次正式 CLI 启动，生成 12 行汇总。

任务以独立文件为单位并发，单个文件内部的采样统计没有拆成多线程。

完整流程每组都会显示 metrics OK，最后显示：

~~~text
Verification benchmark: OK
~~~

加速比小于 1 不是验证失败；它表示当前负载下并行比串行慢。分析错误、输出不一致、意外 stderr 或进程超时才会使本轮失败。

如果只想先检查调用链，可运行：

~~~powershell
python .\scripts\benchmark_taskforge.py --exe .\build\Release\taskforge.exe --suite smoke --repeats 1
~~~

smoke 是轻量联通检查，不用于项目性能结论。正式报告使用默认 standard / 5 次测量的结果。可用 --workers 1,2 调整线程组合，或用 --repeats 9 增加重复次数；不同配置产生独立结果目录，不能混成一次测量。

## 5. 获取本次证据

每次使用独立目录，不覆盖历史测量：

~~~text
build/verification/run-日期时间-唯一编号/
~~~

| 输出 | 用途 |
| --- | --- |
| results.json | passed/failed、系统/CPU/内存（可取得时）、CMake/编译器记录、Git 状态、二进制和源码哈希、数据集清单、每次耗时和汇总 |
| runs.jsonl | 每次成功核对的 CLI 调用，包含预热/正式阶段、轮次、模式与原始纳秒耗时 |
| summary.csv | 适合 Excel 查看与后续制作图表；UTF-8 BOM 编码 |
| SUMMARY.md | 简洁的测量汇总表，供交流与检查 |
| logs/ | 每次 CLI 的原始 stdout/stderr |
| fixtures/ | 合成 PCM16 WAV；文件大小、哈希和独立期望指标保存在 results.json |

取得最新目录并查看：

~~~powershell
$latestRun = Get-ChildItem .\build\verification -Directory -Filter "run-*" |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1

Get-Content (Join-Path $latestRun.FullName "SUMMARY.md") -Encoding UTF8
Import-Csv (Join-Path $latestRun.FullName "summary.csv") | Format-Table -AutoSize
~~~

请保留并发送本机这一轮的 SUMMARY.md、summary.csv 和 results.json。合成 WAV 与每轮日志无需全部上传。build 已被现有 Git 规则忽略，生成数据不会成为源码提交内容。

一旦失败，results.json 的 status 为 failed，保留已有证据，不输出成功的 summary.csv；不要把 failed 目录中的局部数字用于最终性能结论。

## 测量口径

1. 使用 perf_counter_ns 记录启动子进程前到它退出后的墙钟耗时；不把 Python 生成音频、哈希、解析和写报告的时间计入。
2. 计入 CLI 进程启动、线程创建、文件读取、解析/统计、输出到管道及退出/join；不是线程池内部纯计算耗时。
3. 所有模式使用同一份文件、同样输入顺序和同样输出方式。轮换模式执行顺序，减少总是先跑串行的偏差。
4. 合成文件刚写入并被哈希读取，且每模式有预热。未清空 OS 文件缓存，结果偏向暖缓存场景，不能称为冷盘读取性能。
5. 从 PCM 周期样本独立计算预期统计，核对每次结果；不同模式/轮次还要求打印精度下完全一致。能量与 RMS 等使用输出精度对应容差。
6. 预热不计入汇总，保留 5 次正式耗时的中位数、最小值、最大值。
7. 加速比 = 串行批次耗时中位数 / 当前模式批次耗时中位数。MiB/s = 本组完整 WAV 文件字节数 / 2^20 / 批次耗时中位数。
8. 未测量 CPU 利用率、峰值内存、单任务 P95、线程池内部延迟或硬实时截止时间，不在报告中推断这些指标。

短小任务可能主要受进程启动、文件操作与调度开销影响；较大负载也会受磁盘、缓存、内存带宽和 CPU 资源影响。中位数略有改善但波动明显时，不据此宣称稳定加速。开发容器的结果不代表用户电脑；最终报告只采用本机实际记录。

## 开发端已验证内容

- Python 3.12 标准库的 12 项工具自检通过。
- 在现有 v0.3.0 Linux/GCC Release CLI 上跑通三组 standard、四种模式、每模式预热 1 次加正式 5 次。
- 所有运行通过独立期望指标校验及串并行结果一致性检查。
- 检查正常结果 JSON/CSV 的模式数量、测量次数、预热排除和中位数计算。
- 通过注入非零退出与测量失败，验证失败不会发布成功汇总。
- Windows 的 Python/PowerShell 硬件信息采集及测量仍以本机执行为准。CPU/内存采集失败会记为不可用，不中断主体测量。

## 收尾与报告衔接

本轮通过后，将这三个工具/说明文件作为工程验证提交保存；不更改 v0.3.0 标签，不增加 v0.4.0。

报告需要的材料将具备：

- 三阶段功能演进：串行基线 -> 固定线程池 -> 生命周期管理。
- 已发布提交 04da7f5、a957b34、06d95c3 及对应标签。
- Windows Debug/Release 8 项 CTest 和生命周期演示的验收记录。
- 本机真实性能表、设备信息、生成数据参数及原始记录。
- 两个真实问题：MSVC lambda 捕获错误；覆盖后旧构建产物引发 LNK2019，并通过干净重建解决。
- 能力边界：PCM16 文件级并行、协作取消、等待超时、无界队列、非硬实时。

这些数据确认后，再写正式项目报告和面试表达，不提前填入加速比。

参考：[Python 高精度计时](https://docs.python.org/3/library/time.html#time.perf_counter)、[CMake 构建参数](https://cmake.org/cmake/help/latest/manual/cmake.1.html#build-a-project)。
