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
  --runtime-dir /absolute/wechat-runtime \
  --worker /absolute/sdk/bin/wechatocr-worker \
  --iterations 30 --exercise-recovery --output runtime-report.json
```

The script creates synthetic English/Chinese images and checks formats, ROIs, empty input and cleanup. `--exercise-recovery` is Linux-only and tests this script's worker/vendor crashes and an in-flight deadline. Use `--font` for a Chinese font, or omit recovery and pass `--ocr` on Windows. These checks do not measure game-image accuracy or long-term stability.
