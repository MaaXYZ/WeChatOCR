# Runtime 配置与验证

WeChatOCR SDK 不包含腾讯的运行时和模型。使用者需要准备有权使用的本地文件，并在创建 engine 时显式配置路径。

0.1.x 的适配代码覆盖以下协议形态；此表描述代码入口，具体已验证版本列在下方。

| 平台 | 协议 | OCR 入口 | runtime 目录 |
| --- | --- | --- | --- |
| Windows x64 | 微信 3 | `WeChatOCR.exe` | 对应微信版本的 runtime 目录，含匹配的 mmmojo DLL |
| Windows x64 | 微信 4 | `wxocr.dll` | 对应微信版本的 runtime 目录；还需对应 `weixin.exe` 及其原始目录关系 |
| Linux x64 | 微信 4 | `wxocr` | 对应微信安装目录，含匹配的 `libmmmojo.so` |

微信 4 Windows 的 `wxocr.dll` 是协议入口路径，实际由对应进程加载运行。仅复制一个 DLL 不足以建立完整 runtime。应保留同一版本文件之间的目录结构，避免混用不同微信版本的程序、mmmojo 和模型。

## 已验证记录

2026-11-11，在 Debian 13 x86_64 / GCC 14 开发环境中，使用腾讯官方 Debian 包解包后的 runtime 完成以下测试。没有安装微信或登录账号；此记录只说明该包在本次环境的行为，不代表所有版本均有相同运行条件或支持更老的 Linux 系统。

| 项目 | 记录 |
| --- | --- |
| 包版本 / 架构 | `wechat 4.1.13.26 amd64` |
| 下载来源 | `https://dldir1.qq.com/weixin/Universal/Linux/WeChatLinux_x86_64.deb` |
| 包 SHA-256 | `a0db8caeee8d1e6cfe0aa5ce58710270f8d74a90ff4920f53a3fc22aa0956fe9` |
| 运行目录 | 包内 `opt/wechat`，保留全部配套文件 |
| OCR 入口 / 协议 | `opt/wechat/wxocr` / 微信 4 |
| 调用链 | SDK → production worker → `libmmmojo.so` → `wxocr` |
| 文字输入 | `MaaFramework OCR 12345` 和 `微信文字识别测试` 两行合成图 |
| 热调用检查 | 连续 30 次，混合 640×180、900×200、1200×240、1400×280；BGR、BGRA、RGB、GRAY；包含带 padding 的非连续 ROI |
| 结果 | 30 次均成功，期望中英文命中；行框和分数正常返回；空白图成功返回零行 |
| 清理 | 每次识别结束没有残留 PNG；销毁 engine 后私有临时目录与子进程清理完成 |
| 故障恢复 | 分别结束 SDK worker、仅结束 `wxocr`，首调报告 worker 错误，下一调成功；暂停本测试的 `wxocr` 后发送真实图片，观察到在途 PNG，500 ms deadline 回收旧进程树，下一调成功 |
| 回收检查 | 用 PID 与启动时间检查每一代旧进程；均无仍运行的旧进程，超时后的 PNG 与 engine 临时目录清理完成 |

本次混合尺寸调用的热调用 P50 为 `419.672 ms`、P95 为 `1025.456 ms`（首张之外的 29 次调用）。该数字包含图像传输、PNG 与 OCR 开销，仅记录这台开发机器的单轮观测，不是性能保证，也不足以比较其他 OCR 引擎。

0.1.1 在相同官方 runtime 上补测四种像素格式、空图及三种故障恢复。暂停 `wxocr` 并观察到在途 PNG 后，500 ms deadline 返回 `WCO_TIMEOUT`；在下一次调用之前检查，PNG 已删除，随后识别成功，旧进程与临时目录也已回收。0.1.0 的失败图片清理要等到重试或销毁 engine，0.1.1 修复了这一间隔。

Windows x64 的构建和受控测试不等于真实微信 runtime 验证。上述合成图结果也不能推断游戏 UI、低对比度文字、复杂排版的准确率，或数千次调用的长期稳定性。

## 重跑真实 runtime 冒烟测试

脚本依赖 Python 3 和 Pillow；SDK 自身不依赖 Python。无需把腾讯包放进源码目录。

```sh
python -m pip install Pillow
python examples/python/smoke.py \
  --sdk /absolute/sdk/lib/libWeChatOCR.so \
  --runtime-dir /absolute/wechat-runtime \
  --worker /absolute/sdk/bin/wechatocr-worker \
  --iterations 30 \
  --exercise-recovery \
  --output /absolute/path/runtime-report.json
```

脚本自动生成已知中英文文字图，测试多尺寸、格式、非连续 ROI 和空图，记录延迟与清理状态。可用 `--font` 指定支持中文的字体。仓库中的 [`known-text.png`](../examples/python/known-text.png) 是本项目生成的参考输入，不含腾讯资源。

`--exercise-recovery` 仅在 Linux 实现，会结束当前脚本创建的私有 worker 或 `wxocr` 子进程，并触发在途 deadline，验证后续调用和进程清理。它不会结束其他微信进程。Windows 可以省略该参数，通过 `--ocr` 显式指定 OCR 入口。

## 路径与部署

- runtime 目录与 OCR 入口是独立配置项；使用绝对路径。
- 可以显式指定 worker 位置。部署时将 SDK、worker 及其系统依赖一并提供。
- Windows SDK、worker 和 runtime 都需要匹配 x64 架构。
- Windows 发布包要求 Windows 10 x64 或更新系统；SDK 与 worker 使用静态 MSVC CRT。
- Linux 发布包在 Ubuntu 24.04 构建，部署要求 glibc 2.39 或更新。源码构建可调整宿主基线，但不能降低腾讯 runtime 自身要求。
- 不从未知目录自动选择“最新版”，也不自动下载或更新腾讯文件。
- 临时图片目录需要可写；输入图片可能含敏感内容，应按应用的数据要求选择目录。
- runtime 加载可能还需要其原有操作系统依赖。SDK 无法替代这些依赖。

登录状态、网络连接、微信安装状态是否影响某个 runtime，必须对具体版本实测。当前没有提供能保证任意版本脱离这些条件运行的声明。

## 错误处理

配置错误、runtime 缺失、版本或协议不兼容、OCR 内部错误、worker 退出、调用超时应分别诊断。一次成功但未检测到文字的请求可以返回零行；它与加载或通信失败不同。

应用可以在错误后再次调用以启动新的 worker，但应保留总任务 deadline。SDK 不因“没有识别到文字”而自动切换其他 OCR 引擎。

微信返回的置信度是原始值。不要默认它与 PaddleOCR 等引擎的阈值等价；未经过样本校准的分数不能被当成跨引擎概率。

## 验证清单

对每一个准备发布支持声明的 runtime，至少记录：操作系统、架构、微信版本、OCR 插件版本、实际协议、必需文件与来源，以及相关文件的 SHA-256。不得把腾讯程序或模型提交到本仓库。

验证以下行为：

1. 冷启动并完成初始化；分别记录需要的安装、登录和网络条件。
2. 从内存截图识别已知文字；检查 UTF-8、行框、分数和图片尺寸。
3. 检查 BGRA、RGB、BGR、灰度和带 padding 的 stride。
4. 使用中文、空格和长路径；检查临时目录文件能够正常清理。
5. 请求运行时结束 vendor OCR 进程；确认调用有界失败，后续调用可恢复。
6. 人为触发 timeout；确认 worker 与子进程回收，不留下悬挂请求和图片。
7. 连续识别至少数千次，记录进程数量、内存、临时文件和稳定性。
8. 记录冷启动、热调用 P50/P95 延迟，并与其他引擎使用同一批图片比较。
9. 对 MaaFramework 使用场景检查 ROI 坐标还原、文字匹配和点击定位。

兼容性记录应区分“构建通过”“受控测试通过”“真实 runtime 冒烟通过”和“长期稳定性验证通过”。发布工作流自动提供前两类验证；真实 runtime 记录单独维护，不能从 CI 结果外推未经测试的腾讯版本。
