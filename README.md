# WeChatOCR

[English](README.en.md)

微信 OCR 原生 SDK：C ABI 动态库，支持内存截图，返回 UTF-8 文本、文字框和置信度。可独立使用，也可接入 MaaFramework。

从 [Releases](https://github.com/MaaXYZ/WeChatOCR/releases) 下载 SDK。微信 runtime 由本机安装或自行提供，不包含在发布包中。

支持 Windows 10 x64、Linux x64；Linux 发布包要求 glibc 2.39+。已实测 Windows 微信 4.1.15.13 / OCR 插件 8096，以及 Linux 微信 4.1.13.26。

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

MaaFramework 接入见 [示例](examples/maa)。SDK 自动发现接口见 [Toolkit.h](include/WeChatOCR/Toolkit.h)。

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
