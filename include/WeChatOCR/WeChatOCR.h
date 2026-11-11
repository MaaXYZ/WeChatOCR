#ifndef WECHAT_OCR_H
#define WECHAT_OCR_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  if defined(WCO_BUILD)
#    define WCO_API __declspec(dllexport)
#  else
#    define WCO_API __declspec(dllimport)
#  endif
#  define WCO_CALL __cdecl
#else
#  define WCO_API __attribute__((visibility("default")))
#  define WCO_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define WCO_ABI_VERSION 1u
#define WCO_VERSION "0.1.1"

typedef struct WcoEngine WcoEngine;
typedef struct WcoResult WcoResult;
typedef int32_t WcoStatus;
enum {
    WCO_OK = 0,
    WCO_INVALID_ARGUMENT = 1,
    WCO_ABI_MISMATCH = 2,
    WCO_RUNTIME_MISSING = 3,
    WCO_RUNTIME_INCOMPATIBLE = 4,
    WCO_WORKER_ERROR = 5,
    WCO_TIMEOUT = 6,
    WCO_BUSY = 7,
    WCO_OCR_FAILED = 8,
    WCO_IO_ERROR = 9,
    WCO_OUT_OF_MEMORY = 10,
    WCO_INTERNAL_ERROR = 11
};

enum {
    WCO_PIXEL_BGR = 1,
    WCO_PIXEL_BGRA = 2,
    WCO_PIXEL_RGB = 3,
    WCO_PIXEL_GRAY = 4
};

typedef struct WcoOptions {
    uint32_t struct_size;
    uint32_t abi_version;
    /* All paths are UTF-8. runtime_dir and ocr_path must be absolute. */
    const char* runtime_dir;
    const char* ocr_path;
    /* NULL selects the worker installed beside the SDK. */
    const char* worker_path;
    /* NULL selects the system temporary directory. */
    const char* temp_dir;
    /* 0: auto, 3: WeChat 3 (Windows), 4: WeChat 4. */
    uint32_t protocol;
    /* Zero selects 10000 ms. Creation waits for a ready runtime. */
    uint32_t startup_timeout_ms;
    /* Zero selects 16. Includes the active call and waiting callers. */
    uint32_t max_pending_requests;
} WcoOptions;

typedef struct WcoImageView {
    uint32_t struct_size;
    /* BGRA alpha is ignored. Images are limited to 32768 px per axis and 64 MiB of packed pixels. */
    uint32_t format;
    const uint8_t* data;
    uint64_t data_size;
    uint32_t width;
    uint32_t height;
    /* Positive byte stride; padding and non-contiguous ROI rows are supported. */
    uint64_t stride;
} WcoImageView;

typedef struct WcoLine {
    uint32_t struct_size;
    uint32_t has_confidence;
    /* Clockwise: top-left, top-right, bottom-right, bottom-left. */
    float quad[8];
    /* Raw vendor rate; not calibrated against other OCR engines. */
    float confidence;
    /* Borrowed UTF-8, NUL-terminated; valid until result_destroy. */
    const char* text;
    uint64_t text_length;
} WcoLine;

typedef struct WcoResultInfo {
    uint32_t struct_size;
    uint32_t width;
    uint32_t height;
    int32_t vendor_error;
} WcoResultInfo;

typedef struct WcoCapabilities {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t detect_and_recognize;
    uint32_t recognize_only;
    uint32_t native_batch;
    uint32_t native_memory_input;
} WcoCapabilities;

WCO_API const char* WCO_CALL wco_version(void);
WCO_API const char* WCO_CALL wco_status_string(WcoStatus status);
/* Per-thread diagnostic; overwritten by the next fallible SDK call on this thread. */
WCO_API const char* WCO_CALL wco_last_error(void);
WCO_API WcoStatus WCO_CALL wco_create(const WcoOptions* options, WcoEngine** engine);
/* Thread-safe. timeout covers queueing, recovery, image transfer and recognition.
 * Zero timeout is invalid. On failure *result is NULL. Empty successful OCR is OK.
 * Input bytes need only remain valid until this synchronous call returns. */
WCO_API WcoStatus WCO_CALL wco_recognize(WcoEngine* engine, const WcoImageView* image,
                                       uint32_t timeout_ms, WcoResult** result);
WCO_API WcoStatus WCO_CALL wco_get_capabilities(const WcoEngine* engine, WcoCapabilities* capabilities);
WCO_API uint32_t WCO_CALL wco_result_line_count(const WcoResult* result);
WCO_API WcoStatus WCO_CALL wco_result_get_info(const WcoResult* result, WcoResultInfo* info);
WCO_API WcoStatus WCO_CALL wco_result_get_line(const WcoResult* result, uint32_t index, WcoLine* line);
WCO_API void WCO_CALL wco_result_destroy(WcoResult* result);
/* No calls may overlap destroy; join callers before destroying an engine. */
WCO_API void WCO_CALL wco_destroy(WcoEngine* engine);

#ifdef __cplusplus
}
#endif
#endif
