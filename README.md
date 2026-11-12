# WeChatOCR-SDK

[English](README.en.md)

通用的微信 OCR 原生 SDK，提供 C ABI 动态库，供应用传入内存图像并获取 UTF-8 文本、文字框和置信度。

从 [Releases](https://github.com/MaaXYZ/WeChatOCR-SDK/releases) 下载 SDK。微信 runtime 由本机安装或自行提供，不包含在发布包中。

支持 Windows 10 x64、Linux x64；Linux 发布包要求 glibc 2.39+。已实测 Windows 微信 4.1.15.13 / OCR 插件 8096，以及 Linux 微信 4.1.13.26。

## 与 PP-OCRv6 的本机对比

2026-11-12，在 Windows 11 / Ryzen 7 9800X3D / RTX 4080 SUPER 上，通过 MaaFramework 同一入口测试。准确率使用 50 个游戏 ROI 和 48 个合成文字样本，按完整文本匹配统计；耗时包含检测与识别，为预热后的 P50。

| 方案 | 完整文本正确率（98 条） | 游戏 ROI P50 | 1280×720 整屏 P50 |
| --- | ---: | ---: | ---: |
| PP-OCRv6 tiny CPU | 73.5% | 1.5 ms | 50.7 ms |
| PP-OCRv6 tiny DirectML | 73.5% | 26.8 ms | 128.7 ms |
| PP-OCRv6 small CPU | 93.9% | 5.6 ms | 143.5 ms |
| PP-OCRv6 small DirectML | 93.9% | 29.6 ms | 196.2 ms |
| 微信 OCR | 93.9% | 109.0 ms | 174.8 ms |

PPOCR CPU 每个模型配置 4 线程，DirectML 显式使用 RTX 4080 SUPER；微信使用 runtime 默认设置，未强制其内部计算设备。ROI 图像预先裁好，均不使用 `expected` 或 `replace` 修正结果。

这批样本中，small CPU 最均衡，tiny CPU 最快；微信与 small 的游戏 ROI 都正确识别了 47/50 个，整屏已标注目标命中为微信 44/50、small 41/50。DirectML 经设备核对和固定尺寸复测，在当前调用链下延迟仍高于 CPU。这是本机小样本结果，不代表通用 OCR 排行。

[完整报告](https://github.com/MaaXYZ/WeChatOCR-SDK/blob/main/benchmarks/2026-11-12-windows/README.md) 包含 P95、字符错误率 CER、冷启动、内存及测试方法，并提供[逐条对比 HTML（下载后打开）](https://github.com/MaaXYZ/WeChatOCR-SDK/blob/main/benchmarks/2026-11-12-windows/report.html)和[原始数据与脚本快照](https://github.com/MaaXYZ/WeChatOCR-SDK/raw/refs/heads/main/benchmarks/2026-11-12-windows/evidence.zip)。

## 接入

默认发现本机微信，直接创建 engine；自定义目录见 [runtime 配置](docs/runtime.md)。以下 C 函数可处理应用提供的截图：

```c
#include <stdio.h>
#include <WeChatOCR/Toolkit.h>

WcoStatus recognize_rgb(const void* pixels, uint64_t size, uint32_t width, uint32_t height) {
    WcoImageView image;
    WcoStatus status = wco_image_view_init(&image, WCO_PIXEL_RGB, pixels, size, width, height, 0);
    if (status != WCO_OK) return status;
    WcoEngine* engine = NULL;
    status = wco_create(NULL, &engine);
    if (status != WCO_OK) return status;
    WcoResult* result = NULL;
    status = wco_recognize(engine, &image, 10000, &result);
    if (status == WCO_OK) {
        for (uint32_t i = 0; i < wco_result_line_count(result); ++i) {
            WcoLine line = {0};
            line.struct_size = sizeof(line);
            if (wco_result_get_line(result, i, &line) == WCO_OK)
                puts(line.text);
        }
    }
    wco_result_destroy(result);
    wco_destroy(engine);
    return status;
}
```

`WcoImageView` 支持 BGR、RGB、BGRA、灰度及带 padding 的 stride；BGRA 忽略 alpha。重复识别时复用 engine，出错读取 `wco_last_error()`。结果由 SDK 释放。

Python 调用与运行验证见 [示例](examples/python/smoke.py)。SDK 自动发现接口见 [Toolkit.h](include/WeChatOCR/Toolkit.h)。

## 构建

需要 CMake 3.20+、C++20 编译器：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix stage
```

应用的 CMake 配置：

```cmake
find_package(WeChatOCR CONFIG REQUIRED)
target_link_libraries(your_application PRIVATE WeChatOCR::WeChatOCR)
```

将安装目录加入 `CMAKE_PREFIX_PATH`，部署动态库与 worker。设计见 [architecture](docs/architecture.md)，真实 runtime 测试见 [testing](docs/testing.md)。

## 致谢与许可

感谢 [blueveryday/WeChatOCR](https://github.com/blueveryday/WeChatOCR) 的使用方式参考，以及 [swigger/wechat-ocr](https://github.com/swigger/wechat-ocr) 对原生调用链、mmmojo 与协议行为的探索。本项目独立实现，不附带参考仓库的二进制或未明确授权的源码。

本仓库使用 [AGPL-3.0](LICENSE)；腾讯组件遵循其各自的许可与使用条件。
