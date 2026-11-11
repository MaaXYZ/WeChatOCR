# MaaFramework integration

The optional `WeChatOCR::MaaAdapter` target registers a native Custom recognition callback through `MaaResourceRegisterCustomRecognition`. Applications call the SDK directly; no CLI or HTTP server is involved. The SDK owns its internal OCR worker.

Build with `-DWCO_BUILD_MAA_ADAPTER=ON`, a MaaFramework SDK on `CMAKE_PREFIX_PATH`, and the `nlohmann_json`, Boost Regex, and OpenCV core/imgcodecs CMake packages. Set `-DWCO_BUILD_MAA_EXAMPLE=ON` to compile `main.cpp`, a small API demonstration. The adapter is a static library linked into the host application; the SDK remains a shared library.

```cpp
#include <WeChatOCR/MaaAdapter.h>

WcoOptions options{};
options.struct_size = sizeof(options);
options.abi_version = WCO_ABI_VERSION;
options.runtime_dir = absolute_runtime_dir;
options.ocr_path = absolute_ocr_path;

WcoEngine* engine = nullptr;
if (wco_create(&options, &engine) != WCO_OK) {
    // Report wco_last_error().
    return;
}
WcoMaaRegistration* registration = nullptr;
if (wco_maa_register(resource, engine, "WeChatOCR", &registration) != WCO_OK) {
    // Report wco_maa_last_error().
    wco_destroy(engine);
    return;
}

// Bind resource to a tasker and run resource/pipeline/wechat.json, or call
// MaaTaskerPostRecognition(tasker, "Custom", recognition_params_json, image).
// The registration and engine must outlive every callback.

// Stop and join ALL tasks before cleanup.
wco_maa_unregister(registration);
wco_destroy(engine);
// MaaResourceClear / MaaResourceDestroy must happen after unregister.
```

`resource/pipeline/wechat.json` is a Pipeline v2 example. It needs no `model/ocr` directory. The callback crops the ROI and returns boxes/quadrilaterals in full-image coordinates. It emits Maa's `$all` and `$filtered` result arrays; the framework applies the node's `index`, including negative indices. Current MaaFramework with Custom multi-result support is required.

Supported `custom_recognition_param` fields:

| Field | Default | Meaning |
| --- | --- | --- |
| `expected` | `[]` | UTF-8 regular expression or array; any match passes, using Boost wide regex as Maa OCR does |
| `replace` | `[]` | Ordered `[regex, replacement]` pairs, applied after trimming and before matching |
| `threshold` | `0` | Minimum raw vendor confidence; positive values reject results without confidence |
| `timeout_ms` | `10000` | Positive SDK recognition timeout, including its queue and recovery |
| `order_by` | `Horizontal` | `Horizontal`, `Vertical`, `Area`, `Length`, or `Expected`; put this inside `custom_recognition_param` |

`threshold` is applied before text replacement. WeChat confidence is not calibrated against other engines. Its absence is represented as JSON `null`, with `has_confidence: false`; it is never synthesized. Unknown parameters are errors. `only_rec`, native batching and `color_filter` are not implemented by this adapter.

For images, Maa's public C buffer API does not expose row stride. The adapter decodes the buffer's cached PNG in memory, then gives the SDK a BGR ROI view with its real stride. This supports non-contiguous images at the cost of image encoding/decoding; the native Maa OCRSession integration can pass `cv::Mat::step` directly.

On an operational failure the callback returns no hit and includes `status` and `error` in recognition detail. A successful empty OCR returns empty result arrays. Duplicate registration names are rejected. Do not replace or unregister the same name through other APIs while the registration exists.

The original WeChat runtime is supplied separately. Building or running this example does not establish permission to redistribute it.

Adapter contract tests use a real MaaFramework tasker/buffer runtime and a mock SDK, so no WeChat installation is required:

```sh
cmake -S tests/maa -B build-maa-tests -DCMAKE_PREFIX_PATH="<Maa SDK>;<dependencies>"
cmake --build build-maa-tests
ctest --test-dir build-maa-tests --output-on-failure
```

Use the same C++ toolchain/standard library as the Maa SDK and OpenCV binaries. MaaDeps Linux bundles use their supplied Clang/libc++ toolchain (`-DCMAKE_TOOLCHAIN_FILE=<MaaDeps>/cmake/maa-x64-linux-toolchain.cmake`). These tests check adapter contracts; they do not validate vendor runtime compatibility or OCR accuracy.
