# Architecture

```text
Application / MaaFramework
    └─ SDK C ABI ── local pipes ── worker ── mmmojo ── WeChat OCR
```

The SDK owns engine and result handles. It accepts pixel buffers with explicit stride; results contain UTF-8 text, image-relative geometry and vendor confidence. Calls on one engine are serialized. Keep pixels alive until the call returns, release results through the SDK, and join callers before destroying their engine.

Only the worker loads vendor components. It writes a private temporary PNG for the vendor's path-based protocol. Pipe I/O and queueing share the request deadline; a failed worker or disconnected runtime is recycled before a later request. Timeout cleanup removes the old process tree and temporary image files.

Toolkit discovers local runtime candidates and initializes the same explicit `WcoOptions` used for manual paths. MaaFramework's native integration selects this SDK through its OCR session; the optional Custom adapter uses the public registration API. The SDK core has no MaaFramework dependency. Reproducible checks are in [testing.md](testing.md).
