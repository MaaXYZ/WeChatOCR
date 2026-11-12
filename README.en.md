# WeChatOCR

[简体中文](README.md)

A native WeChat OCR SDK with a C ABI: pass memory images and receive UTF-8 text, geometry and confidence. Use it independently or with MaaFramework.

Download the SDK from [Releases](https://github.com/MaaXYZ/WeChatOCR/releases). Install WeChat locally or supply its runtime separately; vendor files are not included.

Platforms: Windows 10 x64 and Linux x64; Linux release packages require glibc 2.39+. Real-runtime tests cover Windows WeChat 4.1.15.13 / OCR plugin 8096 and Linux WeChat 4.1.13.26.

## Use

Create an engine with automatic local discovery. See [runtime setup](docs/runtime.md) for custom locations. This compilable C function processes an application-provided image:

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

`WcoImageView` accepts BGR, RGB, BGRA and grayscale with row stride; BGRA alpha is ignored. Reuse the engine for repeated calls. Read `wco_last_error()` on failure and release results through the SDK.

See the [MaaFramework example](examples/maa) and discovery API in [Toolkit.h](include/WeChatOCR/Toolkit.h).

## Build

Requires CMake 3.20+ and a C++20 compiler:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix stage
```

In your application's CMake project:

```cmake
find_package(WeChatOCR CONFIG REQUIRED)
target_link_libraries(your_application PRIVATE WeChatOCR::WeChatOCR)
```

Add the installation prefix to `CMAKE_PREFIX_PATH`; deploy the shared library and worker. See [architecture](docs/architecture.md) and [runtime tests](docs/testing.md).

## Acknowledgements and license

Thanks to [blueveryday/WeChatOCR](https://github.com/blueveryday/WeChatOCR) for the usage reference, and [swigger/wechat-ocr](https://github.com/swigger/wechat-ocr) for exploring native invocation, mmmojo and protocol behavior. This project is independently implemented and includes neither their binaries nor source without an explicit applicable license.

Licensed under [AGPL-3.0](LICENSE). Tencent components remain subject to their own licenses and terms.
