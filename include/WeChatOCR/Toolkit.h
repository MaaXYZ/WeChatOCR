#ifndef WECHAT_OCR_TOOLKIT_H
#define WECHAT_OCR_TOOLKIT_H

#include "WeChatOCR.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WcoRuntimeList WcoRuntimeList;

enum { WCO_DISCOVER_SYSTEM = 1u };

typedef struct WcoDiscoveryOptions {
    uint32_t struct_size;
    uint32_t flags;
    /* Optional absolute UTF-8 roots for portable or nonstandard installations. */
    const char* const* search_roots;
    uint32_t search_root_count;
} WcoDiscoveryOptions;

typedef struct WcoRuntimeInfo {
    uint32_t struct_size;
    uint32_t protocol;
    const char* runtime_dir;
    const char* ocr_path;
    /* Optional version inferred from directory names; empty when unknown. */
    const char* version;
    const char* source;
} WcoRuntimeInfo;

/* Initialize defaults, including struct_size. NULL is a no-op. */
WCO_API void WCO_CALL wco_options_init(WcoOptions* options);
WCO_API void WCO_CALL wco_discovery_options_init(WcoDiscoveryOptions* options);

/* Discovery only inspects files; it does not start WeChat or download files.
 * NULL options scans the current user's normal installations. An empty list is OK.
 * Candidate strings are borrowed from the list until runtime_list_destroy. */
WCO_API WcoStatus WCO_CALL wco_discover_runtimes(const WcoDiscoveryOptions* options, WcoRuntimeList** runtimes);
WCO_API uint32_t WCO_CALL wco_runtime_list_count(const WcoRuntimeList* runtimes);
WCO_API WcoStatus WCO_CALL wco_runtime_list_get(const WcoRuntimeList* runtimes, uint32_t index, WcoRuntimeInfo* runtime);
WCO_API void WCO_CALL wco_runtime_list_destroy(WcoRuntimeList* runtimes);
WCO_API WcoStatus WCO_CALL wco_validate_runtime(const WcoRuntimeInfo* runtime);

/* Explicit runtime_dir + ocr_path take priority. If both are omitted, discover
 * and try candidates within one startup_timeout_ms budget. NULL options uses
 * defaults; NULL discovery scans normal installations. */
WCO_API WcoStatus WCO_CALL wco_create_auto(const WcoOptions* options,
                                         const WcoDiscoveryOptions* discovery,
                                         WcoEngine** engine);
/* Strings are borrowed from the engine until wco_destroy. */
WCO_API WcoStatus WCO_CALL wco_engine_get_runtime(const WcoEngine* engine, WcoRuntimeInfo* runtime);

/* A zero stride selects packed rows. Checks dimensions, format and byte bounds. */
WCO_API WcoStatus WCO_CALL wco_image_view_init(WcoImageView* image, uint32_t format,
                                             const void* data, uint64_t data_size,
                                             uint32_t width, uint32_t height, uint64_t stride);

#ifdef __cplusplus
}
#endif
#endif
