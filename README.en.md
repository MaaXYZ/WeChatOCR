# WeChatOCR-SDK

[简体中文](README.md)

A general-purpose native WeChat OCR SDK with a C ABI: applications pass in-memory images and receive UTF-8 text, geometry and confidence.

Download the SDK from [Releases](https://github.com/MaaXYZ/WeChatOCR-SDK/releases). Install WeChat locally or supply its runtime separately; vendor files are not included.

Platforms: Windows 10 x64 and Linux x64; Linux release packages require glibc 2.39+. Real-runtime tests cover Windows WeChat 4.1.15.13 / OCR plugin 8096 and Linux WeChat 4.1.13.26.

## Local comparison with PP-OCRv6

Tested on 2026-11-12 using the same MaaFramework entry point on Windows 11 / Ryzen 7 9800X3D / RTX 4080 SUPER. Accuracy is complete-text matching across 50 game ROIs and 48 generated text images. Latency includes detection and recognition and reports warmed P50.

| Configuration | Complete-text accuracy (98 samples) | Game ROI P50 | 1280×720 full-screen P50 |
| --- | ---: | ---: | ---: |
| PP-OCRv6 tiny CPU | 73.5% | 1.5 ms | 50.7 ms |
| PP-OCRv6 tiny DirectML | 73.5% | 26.8 ms | 128.7 ms |
| PP-OCRv6 small CPU | 93.9% | 5.6 ms | 143.5 ms |
| PP-OCRv6 small DirectML | 93.9% | 29.6 ms | 196.2 ms |
| WeChat OCR | 93.9% | 109.0 ms | 174.8 ms |

PPOCR CPU uses four threads per model; DirectML explicitly selects the RTX 4080 SUPER. WeChat uses vendor defaults without forcing its internal compute device. ROI images are pre-cropped; no `expected` or `replace` filters correct the output.

On these samples, small CPU offers the best balance and tiny CPU is fastest. WeChat and small each read 47/50 game ROIs correctly; full-screen annotated-target hits were 44/50 for WeChat and 41/50 for small. Device verification and fixed-size repeats confirmed higher DirectML latency than CPU in this pipeline. These local, limited samples are not a general OCR ranking.

The [full report](https://github.com/MaaXYZ/WeChatOCR-SDK/blob/main/benchmarks/2026-11-12-windows/README.en.md) includes P95, character error rate (CER), startup, memory and methodology, with [per-sample HTML (download and open locally)](https://github.com/MaaXYZ/WeChatOCR-SDK/blob/main/benchmarks/2026-11-12-windows/report.html) and [raw data and script snapshots](https://github.com/MaaXYZ/WeChatOCR-SDK/raw/refs/heads/main/benchmarks/2026-11-12-windows/evidence.zip).

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

See the [Python usage and runtime validation example](examples/python/smoke.py) and discovery API in [Toolkit.h](include/WeChatOCR/Toolkit.h).

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
