# WeChatOCR

[English](README.en.md)

面向程序调用的微信 OCR 原生 SDK，提供 C ABI 动态库和隔离的常驻 worker。独立于 MaaFramework，也可通过可选适配器接入 MaaFramework 自定义识别。

```text
应用 / MaaFramework
    │ C API
    ▼
WeChatOCR 动态库
    │ 私有本地 IPC
    ▼
wechatocr-worker
    │ mmmojo / protobuf
    ▼
使用者提供的微信 OCR runtime
```

**0.1.x 为实验版本。** 已使用官方 Linux x64 微信 `4.1.13.26` runtime 完成真实 OCR 冒烟测试，包含中英文、多尺寸、像素格式和非连续 ROI 输入。Windows 真实 runtime、游戏样本准确率和长期稳定性仍需验证；具体记录见 [runtime 文档](docs/runtime.md#已验证记录)。

## 功能

- C++20 实现、稳定 C ABI，无 OpenCV、Python 或 protobuf 构建依赖。
- 接收内存中的 BGR、RGB、BGRA、灰度图片，显式传入宽、高、stride；BGRA 的 alpha 通道忽略。
- 返回 UTF-8 文本、位置与原始置信度；坐标相对传入图片。
- 初始化一次、多次识别；同步调用带 deadline。
- 将微信组件装入私有 worker；超时或断连后回收进程，后续调用可以恢复。
- 可选 MaaFramework Custom Recognition 适配器。

公开接口接受内存图片，当前 runtime 适配会在私有临时目录生成 PNG，再向微信组件发送绝对路径。完成或回收对应进程后清理文件。此版本不承诺微信端零落盘输入。

## 下载

从 [Releases](https://github.com/MaaXYZ/WeChatOCR/releases) 下载对应平台的 SDK，使用 `SHA256SUMS` 校验完整性。库、worker 和应用程序的架构需要匹配。

Windows 发布包以 Windows 10 x64 为最低系统版本，SDK 与 worker 静态链接 MSVC CRT。Linux 发布包在 Ubuntu 24.04 构建，要求 glibc 2.39 或更新；其他环境可以从源码构建，腾讯 runtime 自身的系统依赖仍需满足。

发布包不包含腾讯的 exe、dll、so 或 OCR 模型。请另行准备有权使用的本地 runtime，详见 [runtime 配置与验证](docs/runtime.md)。

## 从源码构建

要求 CMake 3.20 或更新版本、支持 C++20 的编译器。Windows 使用 MSVC x64；Linux 使用 GCC 或 Clang。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix stage
```

Windows 可以在配置时加 `-A x64`。安装目录中包括公共头文件、动态库、worker 和 CMake package。

在 CMake 项目中使用：

```cmake
find_package(WeChatOCR CONFIG REQUIRED)
target_link_libraries(your_application PRIVATE WeChatOCR::WeChatOCR)
```

将安装目录加入 `CMAKE_PREFIX_PATH`。部署时同时提供 SDK 动态库与 worker；runtime 的路径由宿主显式配置。

## 接口与生命周期

公共接口位于 [`include/WeChatOCR/WeChatOCR.h`](include/WeChatOCR/WeChatOCR.h)。基本调用顺序：

```text
wco_create(options, &engine)
    → wco_recognize(engine, image, timeout_ms, &result)
    → 读取每一行的文本 / 坐标 / 分数
    → wco_result_destroy(result)
    → 重复识别，或 wco_destroy(engine)
```

结果由 SDK 分配并由 SDK 释放；结果中的借用字符串随结果对象销毁失效。同步调用返回前，调用方需要保持输入图片内存有效。调用方应检查每个状态码；“无文字”与 runtime 加载失败、进程退出、请求超时属于不同情况。

同一 engine 的识别调用串行执行；排队与执行共同受调用 deadline 限制。销毁 engine 前，宿主需要停止对该句柄发起新调用，并等待已经发起的调用返回。

原始置信度来自微信引擎，与其他 OCR 引擎的阈值没有直接等价关系。应用切换引擎时应重新检查阈值与样本命中率。

## MaaFramework

可选适配器通过 MaaFramework 的 Custom Recognition C API 注册识别器，使用同一个原生 SDK，不经 CLI。使用方式见 [`examples/maa`](examples/maa)。

适配器只影响显式使用它的 Custom 节点。MaaFramework 内置 `recognition: "OCR"` 的引擎选择属于框架侧集成，不会由安装本 SDK 自动改变。

## 开发与发布

架构、进程边界和错误处理见 [architecture](docs/architecture.md)。CI 在 Windows 和 Linux 上构建、运行自动化测试并验证安装。推送 `v*` tag 后，发布流程在两个平台的测试均通过时上传安装包和 SHA-256 校验和。

CI 不下载或运行腾讯组件。真实 runtime 的冒烟测试、版本矩阵与性能测量遵循 [runtime 验证清单](docs/runtime.md#验证清单)。

使用 [`examples/python/smoke.py`](examples/python/smoke.py) 可以在本机重跑合成文字图、空图和恢复检查；该脚本可选依赖 Pillow，不属于 SDK 运行依赖。

## 参考与致谢

感谢以下项目对微信 OCR 调用方式的探索与分享：

- [blueveryday/WeChatOCR](https://github.com/blueveryday/WeChatOCR)：本项目需求和使用方式的重要参考。
- [swigger/wechat-ocr](https://github.com/swigger/wechat-ocr)：原生调用链、mmmojo 接口及协议行为的重要参考。

本项目独立实现 SDK 接口、进程隔离、通信与适配逻辑；不附带上述仓库中的二进制，不将其未明确授权的源码纳入本项目。引用和致谢不代表腾讯或参考项目对本项目的认可。

## 许可证

本仓库使用 [AGPL-3.0](LICENSE)。腾讯组件不属于本仓库，须遵守其各自适用的许可和使用条件。
