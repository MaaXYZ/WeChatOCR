# WeChatOCR

[简体中文](README.md)

A native SDK for calling WeChat OCR from applications, with a C ABI shared library and an isolated persistent worker. It is independent of MaaFramework; an optional adapter registers a MaaFramework custom recognizer.

```text
Application / MaaFramework
    │ C API
    ▼
WeChatOCR shared library
    │ Private local IPC
    ▼
wechatocr-worker
    │ mmmojo / protobuf
    ▼
User-provided WeChat OCR runtime
```

**Version 0.1.0 is experimental.** A real-runtime smoke test passed with the official Linux x64 WeChat `4.1.13.26` runtime, covering English/Chinese text, multiple image sizes, pixel formats and non-contiguous ROIs. Windows vendor runtimes, game-image accuracy and long-term stability still need validation. See the [runtime record](docs/runtime.md#已验证记录).

## Features

- C++20 implementation and C ABI, without OpenCV, Python or protobuf build dependencies.
- BGR, RGB, BGRA and grayscale memory images, with explicit dimensions and stride. BGRA alpha is ignored.
- Structured UTF-8 text, geometry and original confidence values relative to the input image.
- Persistent initialization and synchronous calls with deadlines.
- Third-party components remain inside a private worker. The SDK can recover after a timeout or disconnection by replacing the process.
- Optional MaaFramework Custom Recognition adapter.

The public API accepts memory images. The current runtime adapter writes a PNG into a private temporary directory and passes its absolute path to the WeChat component. Files are removed after completion or process cleanup. This version does not claim zero-disk image transport to the vendor runtime.

## Download

Download your SDK archive from [Releases](https://github.com/MaaXYZ/WeChatOCR/releases) and verify it against `SHA256SUMS`. Match the architectures of the library, worker and application.

Windows release packages target Windows 10 x64 or newer; the SDK and worker link the MSVC CRT statically. Linux packages are built on Ubuntu 24.04 and require glibc 2.39 or newer. Build from source for other environments, and also satisfy the vendor runtime's own system dependencies.

Tencent executables, libraries and OCR models are not included. Supply a local runtime you are entitled to use; see [runtime setup and validation](docs/runtime.md).

## Build

Requirements: CMake 3.20 or newer and a C++20 compiler. Use MSVC x64 on Windows, or GCC/Clang on Linux.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix stage
```

Add `-A x64` when configuring with Visual Studio. The installation includes public headers, the shared library, worker and CMake package.

```cmake
find_package(WeChatOCR CONFIG REQUIRED)
target_link_libraries(your_application PRIVATE WeChatOCR::WeChatOCR)
```

Add the installation prefix to `CMAKE_PREFIX_PATH`. Deploy the SDK library and worker together; configure runtime paths explicitly in the host application.

## API and ownership

The public interface is [`include/WeChatOCR/WeChatOCR.h`](include/WeChatOCR/WeChatOCR.h). The lifecycle is:

```text
wco_create(options, &engine)
    → wco_recognize(engine, image, timeout_ms, &result)
    → inspect text / geometry / confidence for each line
    → wco_result_destroy(result)
    → recognize another image, or wco_destroy(engine)
```

The SDK allocates and releases results. Borrowed strings remain valid until their result object is destroyed. Keep input pixels valid until the synchronous call returns. Check every status code: an empty successful result differs from runtime loading failure, process exit or timeout.

Calls on one engine execute serially. Queueing and execution share the call deadline. Before destroying an engine, stop submitting calls and wait for outstanding calls to return.

Confidence is the vendor's original score. It is not directly interchangeable with another OCR engine's threshold; validate sample results when switching providers.

## MaaFramework

The optional adapter registers a custom recognizer using MaaFramework's C API and calls this SDK directly. See [`examples/maa`](examples/maa).

Only Custom nodes explicitly configured to use the adapter are affected. Installing the SDK does not change MaaFramework's built-in `recognition: "OCR"` engine selection.

## Development and release

See [architecture](docs/architecture.md) for process boundaries and error handling. CI builds, tests and installs on Windows and Linux. Pushing a `v*` tag publishes archives and SHA-256 checksums only after both platform test jobs succeed.

CI does not download or execute Tencent components. Run the separate [runtime validation checklist](docs/runtime.md#验证清单) for vendor compatibility and performance.

Use [`examples/python/smoke.py`](examples/python/smoke.py) to repeat synthetic text, blank-image and recovery checks locally. Pillow is an optional dependency of this developer script, not an SDK runtime dependency.

## References and acknowledgements

Thanks to these projects for exploring and documenting WeChat OCR integration:

- [blueveryday/WeChatOCR](https://github.com/blueveryday/WeChatOCR), an important reference for the use case and calling workflow.
- [swigger/wechat-ocr](https://github.com/swigger/wechat-ocr), an important reference for native invocation, mmmojo interfaces and protocol behavior.

The SDK API, process isolation, IPC and adapter logic in this repository are independently implemented. We do not include binaries from those repositories or import source code without an explicit applicable license. Acknowledgement does not imply endorsement by Tencent or the reference projects.

## License

This repository is licensed under [AGPL-3.0](LICENSE). Tencent components are separate and remain subject to their applicable licenses and terms.
