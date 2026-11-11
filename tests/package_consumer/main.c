#include <WeChatOCR/WeChatOCR.h>
#include <WeChatOCR/Toolkit.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    WcoOptions options;
    WcoDiscoveryOptions discovery;
    wco_options_init(&options);
    wco_discovery_options_init(&discovery);
    if (options.struct_size != sizeof(options) || options.abi_version != WCO_ABI_VERSION ||
        discovery.struct_size != sizeof(discovery) || discovery.flags != WCO_DISCOVER_SYSTEM) return 2;
    if (strcmp(wco_version(), WCO_VERSION) != 0) return 1;
    printf("Installed WeChatOCR %s, ABI %u\n", wco_version(), WCO_ABI_VERSION);
    return 0;
}
