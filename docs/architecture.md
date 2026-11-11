# Architecture

The application links to a small C ABI library. The library starts a private worker, sends framed binary messages through local pipes, and reads structured replies. Only the worker loads the vendor runtime and communicates with its OCR process.

```text
Host process                         Private process tree
┌──────────────────────────┐         ┌────────────────────────┐
│ Application              │   IPC   │ wechatocr-worker       │
│   └─ WeChatOCR C ABI     │◀───────▶│   └─ mmmojo            │
└──────────────────────────┘         │       └─ vendor OCR    │
                                    └────────────────────────┘
```

## Public boundary

Opaque engine and result handles keep C++ types out of the ABI. The public API uses fixed-width values, UTF-8 strings and explicit image dimensions/stride. Struct size and ABI version fields allow validation before accessing configuration.

The SDK owns result storage. Text pointers borrowed from a result are valid until result destruction. Host code must not free them using another allocator. The synchronous image API borrows pixels for the duration of the call.

Coordinates are relative to the image passed to the SDK. A host that crops an ROI or resizes its screenshot is responsible for its corresponding coordinate transform. The Maa adapter performs the ROI transform for its own inputs.

## Image transport

Memory input is the public contract. The worker currently encodes or receives a lossless PNG, writes it in a private temporary directory, and passes an absolute filename to the vendor protocol. A temporary file remains available while the vendor can still read it. A timed-out process tree is terminated before its files are removed.

Some observed vendor protocols contain a byte-data field. Its presence alone does not establish usable memory transport. A future verified implementation can use it without changing the public image API.

## Process and request lifecycle

An engine initializes its worker once and reuses it across calls. Calls on an engine are serialized. A call deadline includes the time waiting to enter the engine, writing a request, and reading its reply. Startup has a separate configured deadline.

The library treats EOF, malformed replies, worker exit and deadline expiry as explicit failures. It terminates an unhealthy worker and its children. A subsequent call may start a fresh worker. It does not silently turn an infrastructure failure into a successful empty OCR result.

The IPC protocol associates a reply with its request. A replaced worker has a new communication channel; stale output from an old process cannot be used as the new process's response.

The host must stop submitting operations and wait for in-flight operations before destroying an engine handle. Concurrent calls may share an engine, but concurrent use and destruction of the same handle is not supported.

## Why a separate worker?

The reference native implementation documents persistent vendor threads after stopping the mmmojo environment and warns that unloading its module can crash. Process isolation gives the SDK a bounded way to recover those resources. Vendor globals, DLL versions and unload behavior stay outside the application process.

The worker is a private SDK implementation detail. Applications call functions; no public CLI protocol, HTTP server or listening network service is required.

## Vendor adapters

The vendor adapter dynamically resolves mmmojo functions and implements only the protocol fields needed for initialization and OCR requests/results. No Tencent library is linked into the SDK at build time. Runtime and protocol selection are explicit and validated; unknown combinations are errors.

Output preserves text geometry and the original score. Confidence values are not calibrated across engines. Detection plus recognition does not automatically provide a true recognition-only operation or a detector-independent batch recognition mode.

## MaaFramework boundary

The optional adapter uses MaaFramework's public custom recognition registration API. It handles ROI extraction, text filtering and result conversion while using the same SDK engine. The SDK core has no MaaFramework dependency.

Integrating an engine into MaaFramework's built-in OCR session factory is a separate framework change. Such integration must account for session capabilities, model validation, OCR batch eligibility and cache identity; a Pipeline field alone is insufficient.

## Verification scope

Automated tests exercise API validation, image/IPC encoding, protocol parsing and controlled worker behavior. CI does not contain Tencent programs or models. Runtime compatibility, operating-system requirements, OCR quality and long-running behavior require real-runtime tests; see [runtime.md](runtime.md).
