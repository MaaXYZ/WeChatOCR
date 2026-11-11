#ifndef WECHAT_OCR_MAA_ADAPTER_H
#define WECHAT_OCR_MAA_ADAPTER_H

#include <MaaFramework/MaaAPI.h>
#include <WeChatOCR/WeChatOCR.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WcoMaaRegistration WcoMaaRegistration;

/* Registers a Custom recognition callback under name (NULL means WeChatOCR).
 * resource and engine are borrowed. Stop and join all tasks, unregister, then
 * destroy either borrowed object. Do not replace/remove this name externally.
 * A duplicate name is rejected. On failure *registration is NULL. */
WcoStatus WCO_CALL wco_maa_register(MaaResource* resource, WcoEngine* engine,
                                  const char* name, WcoMaaRegistration** registration);

/* No callback may overlap this call. Unregister before MaaResourceClear(),
 * MaaResourceDestroy() or wco_destroy(). Always releases the registration. */
WcoStatus WCO_CALL wco_maa_unregister(WcoMaaRegistration* registration);

/* Per-thread adapter diagnostic. Callback failures also appear in out_detail. */
const char* WCO_CALL wco_maa_last_error(void);

#ifdef __cplusplus
}
#endif
#endif
