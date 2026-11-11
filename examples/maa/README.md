# MaaFramework

With a MaaFramework build that includes WeChatOCR support, configure a local Resource before loading it:

```python
from maa.resource import Resource

resource = Resource()
assert resource.configure_wechat_ocr()
```

This finds the SDK beside the loaded MaaFramework library and discovers the local WeChat runtime. For custom locations, pass a dictionary with `sdk_path`, or both `runtime_dir` and `ocr_path`. Configuration is local to the host; Remote/Agent Resource configuration is not supported.

Use a normal OCR node:

```json
{
  "FindStart": {
    "recognition": {
      "type": "OCR",
      "param": {"engine": "WeChatOCR", "expected": "开始"}
    },
    "action": {"type": "Click"}
  }
}
```

## Optional Custom adapter

For MaaFramework builds without native WeChatOCR support, build the adapter with a Maa SDK and its dependencies:

```sh
cmake -S . -B build -DWCO_BUILD_MAA_ADAPTER=ON \
  -DCMAKE_PREFIX_PATH="<Maa SDK>;<dependencies>"
cmake --build build --config Release --parallel
```

Link `WeChatOCR::MaaAdapter`, create a SDK engine, and register it:

```cpp
#include <WeChatOCR/MaaAdapter.h>

WcoStatus attach(MaaResource* resource, WcoEngine* engine,
                 WcoMaaRegistration** registration) {
    return wco_maa_register(resource, engine, "WeChatOCR", registration);
}
```

Use [`resource/pipeline/wechat.json`](resource/pipeline/wechat.json) for this Custom route. Stop and join tasks, call `wco_maa_unregister()`, then destroy the engine and Resource. A complete example is in [main.cpp](main.cpp); enable `WCO_BUILD_MAA_EXAMPLE` to build it.
