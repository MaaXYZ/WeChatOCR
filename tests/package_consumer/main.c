#include <WeChatOCR/WeChatOCR.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    if (strcmp(wco_version(), WCO_VERSION) != 0) return 1;
    printf("Installed WeChatOCR %s, ABI %u\n", wco_version(), WCO_ABI_VERSION);
    return 0;
}
