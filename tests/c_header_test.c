#include "WeChatOCR/WeChatOCR.h"

#include <string.h>

int main(void) {
    WcoOptions options = {0};
    WcoImageView image = {0};
    WcoLine line = {0};
    WcoCapabilities capabilities = {0};
    WcoResultInfo info = {0};
    WcoEngine* engine = NULL;
    options.struct_size = (uint32_t)sizeof(options);
    options.abi_version = WCO_ABI_VERSION;
    image.struct_size = (uint32_t)sizeof(image);
    line.struct_size = (uint32_t)sizeof(line);
    capabilities.struct_size = (uint32_t)sizeof(capabilities);
    info.struct_size = (uint32_t)sizeof(info);
    if (strcmp(wco_version(), WCO_VERSION) != 0) return 1;
    if (wco_create(NULL, &engine) != WCO_INVALID_ARGUMENT || engine != NULL) return 2;
    if (wco_get_capabilities(NULL, &capabilities) != WCO_INVALID_ARGUMENT) return 3;
    if (wco_result_get_info(NULL, &info) != WCO_INVALID_ARGUMENT) return 4;
    if (wco_result_get_line(NULL, 0, &line) != WCO_INVALID_ARGUMENT) return 5;
    if (wco_result_line_count(NULL) != 0) return 6;
    if (!wco_last_error() || !wco_status_string(WCO_OK)) return 7;
    wco_result_destroy(NULL);
    wco_destroy(NULL);
    return options.struct_size && image.struct_size ? 0 : 8;
}
