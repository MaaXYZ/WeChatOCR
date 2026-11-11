# Tests

SDK and process-boundary tests run without a vendor runtime:

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

For a real runtime, install the optional Python dependency Pillow and run:

```sh
python examples/python/smoke.py \
  --sdk /absolute/sdk/lib/libWeChatOCR.so \
  --search-root /absolute/wechat-runtime \
  --worker /absolute/sdk/bin/wechatocr-worker \
  --iterations 30 --exercise-recovery --output runtime-report.json
```

Omit `--search-root` to discover the local installation; custom roots may be repeated. The script creates English/Chinese images and checks formats, ROIs, empty input and cleanup. `--exercise-recovery` is Linux-only. Use `--font` for a Chinese font; explicit paths remain available as `--runtime-dir` plus `--ocr`. These checks do not measure game-image accuracy or long-term stability.
