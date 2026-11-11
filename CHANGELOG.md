# Changelog

## 0.1.0 — 2026-11-11

Initial experimental release.

- Native shared library with a versioned C ABI and explicit engine/result ownership.
- Memory image input and structured UTF-8 text, geometry and confidence output.
- An isolated worker for private WeChat runtime communication.
- Request deadlines and process recovery after a timeout or disconnection.
- Windows x64 and Linux x64 build, test and package workflows.
- Optional MaaFramework custom recognition adapter.
- Documentation and acknowledgements for the reference projects.

Tencent executables, libraries and OCR models are not distributed. Automated tests cover the SDK and protocol behavior; compatibility and accuracy with a real Tencent runtime require separate runtime validation.
