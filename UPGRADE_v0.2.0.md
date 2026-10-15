# TaskForge v0.2.0 升级与验收

## 基线及覆盖范围

以用户已发布的 v0.1.0（提交 `04da7f54d71a9198030f9f5cc70a1f2080e81edd`）为基线。音频解析源码、音频头文件、原有 WAV 单元测试和样本字节未改动。

本版修改 `CMakeLists.txt`、`app/main.cpp`、`README.md`，新增线程池头文件/实现、两份测试和本说明。交付 ZIP 内有一个 `TaskForge` 文件夹，只含源码、文档和样本，不含 `.git`、`build`、编译产物或本地编辑器配置。

## 1. 在现有本地仓库创建开发分支

在已经能运行 CMake/MSVC 的 PowerShell 中：

```powershell
cd D:\Projects\TaskForge
git status --short
git rev-parse --short HEAD
git branch --show-current
```

开始覆盖前应是干净工作区，HEAD 为 `04da7f5`。若已经创建下面的分支，就跳过创建命令；否则执行：

```powershell
git switch -c feature/v0.2-thread-pool
```

## 2. 覆盖源码

将下载的 `TaskForge-v0.2.0.zip` 解压到单独的临时目录。打开解压后的 `TaskForge` 文件夹，把**里面的内容**复制到 `D:\Projects\TaskForge`，同名文件选择替换。

不要删除整个现有仓库；保留本机 `.git` 和 `build`。也不要将外层 `TaskForge` 文件夹再嵌套为 `D:\Projects\TaskForge\TaskForge`。

预期暂存前改动：3 个已跟踪文件修改（CMake、CLI、README），5 个新文件（线程池头文件/实现、两份测试、本说明）。行尾归一化可能使工作区显示更多修改；以 Git 实际 diff 为准。

## 3. 配置、构建、测试

逐条成功后再执行下一条：

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
.\build\Debug\taskforge.exe --version
```

预期 5 个 CTest 入口全部通过，程序版本输出 `TaskForge v0.2.0`。现有 v0.1.0 的 build 使用相同生成器，可以重新配置并增量构建。

## 4. 手动检查串行/并发输出

```powershell
$sample = ".\samples\tone_440hz_mono_8k.wav"
$serial = & .\build\Debug\taskforge.exe --serial $sample $sample
if ($LASTEXITCODE -ne 0) { throw "串行样本运行失败" }
$parallel = & .\build\Debug\taskforge.exe --threads 4 $sample $sample
if ($LASTEXITCODE -ne 0) { throw "并发样本运行失败" }
$parallel
Compare-Object ($serial | Select-Object -Skip 1) ($parallel | Select-Object -Skip 1)
```

两种模式的第一行运行方式不同，去掉第一行后应完全一致；`Compare-Object` 无输出表示一致。并发示例只是功能演示，不是速度基准。

## 5. 有意测试独立失败

下面使用不存在的文件验证错误传播；报错属于这项测试的预期行为：

```powershell
.\build\Debug\taskforge.exe --threads 4 $sample .\samples\missing.wav $sample
$LASTEXITCODE
```

应显示两份正确样本结果及一条无法打开文件的错误，退出码为 `1`。运行 `--threads 0` 则应拒绝参数并返回 `2`。

## 已完成的开发端验证

- Linux G++ 13.3、C++17、`-pthread -Wall -Wextra -Wpedantic -Werror`：CLI 与三个测试程序严格编译通过。
- 原始 WAV 单元测试、线程池单元测试、72 个音频任务串行/并发一致性测试通过。
- 线程池测试重复运行 20 轮通过；每轮多生产者测试验证 1000 个已接受任务恰好执行一次；另验证 500 个任务在析构前完成。
- CLI 串行/并发输出一致，9 类错误参数、帮助/版本、部分输入失败的退出码检查通过。
- 线程池和并发音频测试通过 AddressSanitizer + UndefinedBehaviorSanitizer 检查；由于执行环境限制，关闭了 LeakSanitizer，因此本项不代表已进行泄漏检查。
- 开发执行环境未安装 CMake，未声称在这里执行过 CTest 或 Windows/MSVC 验收。这两项需要按上面的步骤在用户本机完成。

当前队列无容量上限、无显式取消/超时接口；排空析构要求任务最终返回，并且调用方在销毁前停止提交。这些约定不是性能或生产级可靠性承诺。

## 验收通过后再提交

```powershell
git status --short
git --no-pager diff --check
git --no-pager diff --stat
```

确认源码范围和本机测试结果后，再完成 v0.2.0 提交、合并和发布；此阶段无需提前改动 v0.1.0 标签。
