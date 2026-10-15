# TaskForge v0.3.0 覆盖与验收

基线：已发布 v0.2.0，提交 a957b34b8d6dce069d0ef4829e0d2ea5d08b4d2c。复制上一版交付源码，并保留已修正的 MSVC lambda 显式捕获。

## 1. 分支与覆盖

~~~powershell
cd D:\Projects\TaskForge
git status --short
git branch --show-current
~~~

确认 main、工作区干净，然后：

~~~powershell
git switch -c feature/v0.3-task-lifecycle
~~~

解压后，把包内 TaskForge 文件夹的全部内容复制到现有 D:\Projects\TaskForge 并替换同名文件。避免套成 D:\Projects\TaskForge\TaskForge。保留已有 Git 与 build 目录。

包中无编译产物，不增加外部运行库、网络服务、数据库或 Python 环境。v0.2.0 的提交可作为回看基线。

## 2. Debug 配置与测试

~~~powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
if ($LASTEXITCODE -ne 0) { throw "配置失败，请停止" }

cmake --build build --config Debug
if ($LASTEXITCODE -ne 0) { throw "Debug 构建失败，请停止" }

ctest --test-dir build -C Debug --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "Debug 测试失败，请停止" }
~~~

应为 8/8 全部通过。新增了构建目标和 CTest 入口，因此需要重新配置；可以继续使用原 build 目录及生成器。

## 3. 新能力演示

~~~powershell
.\build\Debug\taskforge.exe --version
.\build\Debug\taskforge_lifecycle_demo.exe
~~~

预期：

~~~text
TaskForge v0.3.0 lifecycle demo
running task: running
pending task: queued
pending cancellation: cancelled (callable skipped)
wait_for: timeout (task still running, no cancellation)
later result: 42; state=succeeded
running cancellation: requested (state remains running until exit)
cooperative exit: cancelled
CancelPending close: queued cancelled; running finished; workers joined
Lifecycle demo: OK
~~~

使用同步门控制任务进度，不依赖音频处理速度。等待超时不是任务终态；运行中取消需要函数退出才能完成。

## 4. 音频兼容验收

~~~powershell
.\build\Debug\taskforge.exe --serial .\samples\tone_440hz_mono_8k.wav
.\build\Debug\taskforge.exe --threads 4 .\samples\tone_440hz_mono_8k.wav .\samples\tone_440hz_mono_8k.wav
.\build\Debug\taskforge.exe --threads 1 --wait-ms 5000 .\samples\tone_440hz_mono_8k.wav
~~~

三种路径指标一致，RMS 为 0.353551；第三条通常正常完成，不要求超时。

~~~powershell
.\build\Debug\taskforge.exe --threads 1 --wait-ms 0 .\samples\tone_440hz_mono_8k.wav
$LASTEXITCODE
~~~

预期参数错误、退出码 2，等待预算范围是 1–600000 ms。

~~~powershell
.\build\Debug\taskforge.exe --threads 2 .\samples\tone_440hz_mono_8k.wav .\samples\missing_for_test.wav .\samples\tone_440hz_mono_8k.wav
$LASTEXITCODE
~~~

不存在的文件报错，其他两个有效文件仍输出结果，整批退出码为 1。

## 5. Release 与 Git 检查

~~~powershell
cmake --build build --config Release
if ($LASTEXITCODE -ne 0) { throw "Release 构建失败，请停止" }

ctest --test-dir build -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "Release 测试失败，请停止" }

git --no-pager diff --check
git status --short
~~~

保存验收输出，再进行 Git 暂存审核、提交和发布。交付源码包不等于已在 Windows/MSVC 验收通过的正式标签。

## 关键边界

- CLI 的 --wait-ms 是逐个结果的一次等待预算，超时后请求取消，退出前仍等待线程结束。
- close(CancelPending) 只取消仍在队列里的任务。已取出/运行中的任务继续。
- 不强杀线程、不撤销副作用。析构默认 Drain。
- 队列仍无界。性能测量、工程整理属于 v1.0.0。
