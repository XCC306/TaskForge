# TaskForge v0.3.0 开发端验证记录

日期：2026-10-15。基线：v0.2.0，用户已发布提交 a957b34b8d6dce069d0ef4829e0d2ea5d08b4d2c。

## 环境与结果

开发端为 Linux x86_64、GCC 13.3.0、CMake 3.31.10、C++17。启用 -Wall -Wextra -Wpedantic，并额外用 -Werror 将警告视为错误。

| 验证 | 实际结果 |
| --- | --- |
| CMake Debug 配置、编译、CTest | 8/8 通过 |
| CMake Release 配置、编译、CTest | 8/8 通过 |
| thread_pool_unit 重复 50 次 | 全部通过 |
| task_lifecycle_unit 重复 50 次 | 全部通过 |
| AddressSanitizer + UndefinedBehaviorSanitizer | 最终生命周期测试通过，未报告错误 |
| 真实 CLI 等待超时 | 临时 32 MiB PCM16 WAV、1 ms 等待预算：超时提示，退出码 1，正常 join 后退出 |
| 串行/并行 CLI 对比 | 除首行执行模式外输出一致 |
| 无效文件隔离 | 两个有效结果保留，缺失文件报错，退出码 1 |
| 参数边界 | 非法线程数、等待预算、缺少参数及重复模式被拒绝 |
| 源码空白与包完整性 | 打包前检查；ZIP CRC 和逐文件字节核对 |

CTest 入口：wav_unit、thread_pool_unit、parallel_audio_unit、task_lifecycle_unit、lifecycle_demo、cli_regressions、sample_cli、sample_cli_parallel。

ASan/UBSan 命令：

~~~bash
g++ -std=c++17 -pthread -O1 -g -fsanitize=address,undefined \
  -fno-omit-frame-pointer -Iinclude src/thread_pool.cpp \
  src/wav_analyzer.cpp tests/test_task_lifecycle.cpp -o taskforge_lifecycle_san
ASAN_OPTIONS=detect_leaks=0 ./taskforge_lifecycle_san samples/tone_440hz_mono_8k.wav
~~~

本次 detect_leaks=0，未检查泄漏；ASan/UBSan 不等于数据竞争检测，也没有运行 ThreadSanitizer。重复测试不能证明不存在所有竞争。

## 生命周期覆盖

- queued/running/succeeded/failed/cancelled 的可观察行为。
- queued 取消立即发布 TaskCancelled，且不执行回调。
- running 取消只设置请求，直到函数退出才发布终态。
- 普通函数不被强杀；取消不撤销副作用，迟到结果被丢弃。
- 等待超时不改变状态，随后仍可正常 get。
- move-only 参数、函数、返回值，以及 void/引用返回值。
- 标准和非标准异常重抛、终态不可再次取消、get 只能消费一次。
- Drain、CancelPending、升级关闭、重复等待、关闭后拒绝提交。
- 外部并发 join、工作线程拒绝等待自身、工作线程可关闭接收。
- 取消与完成竞争：先阻塞四个工作线程，排好任务后放行执行与取消。
- 提交与关闭竞争，核对已接收任务没有丢失或重复执行。
- 可取消 WAV 与原统计一致；入口取消检查优先于打开文件。
- v0.2.0 原有线程池和 72 个混合音频任务的回归。

## 待用户本机验收

没有在开发端执行 Windows 或 MSVC。请按 UPGRADE_v0.3.0.md 在已安装的 VS 2026 / CMake 4.3.1 环境执行 Debug、Release 构建及各 8 项 CTest，再运行生命周期演示和音频命令。

本包是待本机验收的 v0.3.0 源码交付，不代表远端已经提交或创建 v0.3.0 标签。不宣称实测加速比，性能复现留到 v1.0.0。

## Windows/MSVC 本机验收：2026-10-15

- Windows 11，Visual Studio 2026 MSVC。
- Debug 构建成功，CTest 8/8 通过。
- Release 构建成功，CTest 8/8 通过。
- Debug、Release 生命周期演示均输出 Lifecycle demo: OK。
- 版本输出为 TaskForge v0.3.0。
- 覆盖后曾出现新增接口的 LNK2019；使用 --clean-first 干净重建后解决。
