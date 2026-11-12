# Changelog

## 0.2.1 — 2026-11-12

- Discover Windows OCR plugins with `.xnet` models beside `wxocr.dll`, including plugin 8096.
- Retry Windows input-file sharing violations before replying, and recycle the worker if cleanup fails.
- Extend the real-runtime smoke test to check Windows process cleanup and crash/timeout recovery.
- Validate automatic discovery and real OCR with Windows WeChat 4.1.15.13 / plugin 8096.

## 0.2.0 — 2026-11-11

- Add Toolkit APIs for runtime discovery, validation, defaults and image views.
- Allow `wco_create(NULL, &engine)`; support portable roots and query the selected paths.
- Simplify the documentation and Python example. C ABI v1 remains compatible.

## 0.1.1 — 2026-11-11

- Remove temporary images immediately after terminating a failed or timed-out worker, before returning to the caller. Previously cleanup waited for the next request or engine destruction.
- Check the failure-to-retry interval in production-worker regression tests and the optional real-runtime smoke script.
- Keep C ABI v1 unchanged.

## 0.1.0 — 2026-11-11

Initial experimental release.

- Native shared library with a versioned C ABI and explicit engine/result ownership.
- Memory image input and structured UTF-8 text, geometry and confidence output.
- An isolated worker for private WeChat runtime communication.
- Request deadlines and process recovery after a timeout or disconnection.
- Windows x64 and Linux x64 build, test and package workflows.
- Optional MaaFramework custom recognition adapter.
- Documentation and acknowledgements for the reference projects.
- A real-runtime smoke test using the official Linux x64 WeChat 4.1.13.26 package: Chinese/English text, 30 mixed-size/format/ROI calls, blank-image handling, worker/vendor crash recovery, in-flight timeout recovery and process/file cleanup.
- A reproducible optional Python smoke script with synthetic input and process recovery checks.

Tencent executables, libraries and OCR models are not distributed. Automated tests cover the SDK and protocol behavior. The Linux runtime record is version-specific; Windows vendor runtimes, game-image accuracy and long-term stability remain outside the completed validation scope.
