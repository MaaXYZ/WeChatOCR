# Runtime 配置

`wco_create(NULL, &engine)` 默认发现本机微信并启动 OCR。SDK 和 worker 随应用部署；腾讯 runtime 与模型由本机安装或另行提供。

默认查找 Windows 注册表、常见安装目录及当前用户的 OCR 插件目录；Linux 查找标准安装目录。Toolkit 也可枚举候选，并接受自定义搜索目录；接口见 [Toolkit.h](../include/WeChatOCR/Toolkit.h)。

只查找便携安装目录：

```c
WcoStatus create_portable(WcoEngine** engine) {
    const char* roots[] = {"/absolute/portable-wechat"};
    WcoDiscoveryOptions discovery;
    wco_discovery_options_init(&discovery);
    discovery.flags = 0;
    discovery.search_roots = roots;
    discovery.search_root_count = 1;
    return wco_create_auto(NULL, &discovery, engine);
}
```

要手动指定路径，用 `wco_options_init()` 初始化 `WcoOptions`，同时填写 `runtime_dir` 和 `ocr_path`，再调用 `wco_create()`。Windows 微信 3 的入口是 `WeChatOCR.exe`，微信 4 是 `wxocr.dll`；Linux 是 `wxocr`。保留配套目录结构。用 `wco_engine_get_runtime()` 可获取实际选中的路径。

找不到 runtime 时，创建函数返回错误，使用 `wco_last_error()` 查看原因。安装对应微信组件、增加搜索目录，或显式指定路径后重试；自动发现不会下载文件。

Windows 发布包要求 Windows 10 x64；Linux 发布包在 Ubuntu 24.04 构建，要求 glibc 2.39+。腾讯 runtime 自身的系统依赖也需要满足。

## 已验证

Windows 本机微信 `4.1.15.13` / OCR 插件 `8096` 自动发现并实测成功：中英文、30 次多尺寸/格式/非连续 ROI、空图、worker 和 OCR 子进程崩溃恢复、在途超时恢复，以及进程和临时文件清理。支持模型位于插件根目录或模型子目录的布局。

Linux 官方 `wechat 4.1.13.26 amd64` 解包后实测成功，未安装或登录：中英文、30 次多尺寸/格式/非连续 ROI、空图，以及崩溃和在途超时恢复。测试环境为 Debian 13 x86_64 / GCC 14。

来源：[腾讯官方 Debian 包](https://dldir1.qq.com/weixin/Universal/Linux/WeChatLinux_x86_64.deb)；SHA-256：`a0db8caeee8d1e6cfe0aa5ce58710270f8d74a90ff4920f53a3fc22aa0956fe9`。复现命令见 [testing.md](testing.md)。
