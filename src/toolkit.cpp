#include "WeChatOCR/Toolkit.h"
#include "api_internal.h"
#include "discovery.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

struct WcoRuntimeList { std::vector<wco::RuntimeCandidate> candidates; };

namespace {
using Clock = std::chrono::steady_clock;
using wco::valid_utf8;
std::filesystem::path path(const char* text) {
    return std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(text)));
}
WcoStatus discovery_options(const WcoDiscoveryOptions* input, wco::DiscoveryOptions& output) {
    if (!input) return WCO_OK;
    if (input->struct_size < sizeof(WcoDiscoveryOptions) || (input->flags & ~WCO_DISCOVER_SYSTEM) ||
        input->search_root_count > 64 || (input->search_root_count && !input->search_roots))
        return wco::set_error(WCO_INVALID_ARGUMENT, "invalid discovery options");
    output.scan_system = (input->flags & WCO_DISCOVER_SYSTEM) != 0;
    for (uint32_t i = 0; i < input->search_root_count; ++i) {
        const auto* root = input->search_roots[i];
        if (!root || !*root || !valid_utf8(root) || !path(root).is_absolute())
            return wco::set_error(WCO_INVALID_ARGUMENT, "search roots must be absolute UTF-8 paths");
        output.search_roots.push_back(path(root));
    }
    return WCO_OK;
}
bool native_candidate(const wco::RuntimeCandidate& candidate) {
    std::error_code error;
    const auto entry = path(candidate.ocr_path.c_str()).filename();
#ifdef _WIN32
    // The internal scanner also recognizes ELF fixtures on Windows. A mixed
    // portable directory can contain both Mojo libraries; the entry point must
    // belong to this OS as well as its runtime library.
    return entry.has_extension() &&
        std::filesystem::is_regular_file(path(candidate.runtime_dir.c_str()) / "mmmojo_64.dll", error);
#else
    return entry == "wxocr" && candidate.protocol == 4 &&
        std::filesystem::is_regular_file(path(candidate.runtime_dir.c_str()) / "libmmmojo.so", error);
#endif
}
WcoStatus options_status(const WcoOptions* options) {
    if (!options) return WCO_OK;
    if (options->struct_size < sizeof(WcoOptions)) return wco::set_error(WCO_INVALID_ARGUMENT, "invalid options struct_size");
    if (options->abi_version != WCO_ABI_VERSION) return wco::set_error(WCO_ABI_MISMATCH, "unsupported ABI version");
    if (options->protocol != 0 && options->protocol != 3 && options->protocol != 4)
        return wco::set_error(WCO_INVALID_ARGUMENT, "protocol must be 0, 3 or 4");
    const bool runtime = options->runtime_dir && *options->runtime_dir;
    const bool ocr = options->ocr_path && *options->ocr_path;
    if (runtime != ocr) return wco::set_error(WCO_INVALID_ARGUMENT, "set both runtime paths or omit both");
    for (const auto* override_path : {options->worker_path, options->temp_dir}) {
        if (override_path && *override_path && (!valid_utf8(override_path) || !path(override_path).is_absolute()))
            return wco::set_error(WCO_INVALID_ARGUMENT, "worker_path and temp_dir must be absolute UTF-8 paths");
    }
    if (options->temp_dir && *options->temp_dir && !std::filesystem::is_directory(path(options->temp_dir)))
        return wco::set_error(WCO_INVALID_ARGUMENT, "temp_dir must identify an existing directory");
    return WCO_OK;
}
}

extern "C" {
void WCO_CALL wco_options_init(WcoOptions* options) {
    if (options) *options = {sizeof(WcoOptions), WCO_ABI_VERSION, nullptr, nullptr, nullptr, nullptr, 0, 10000, 16};
}
void WCO_CALL wco_discovery_options_init(WcoDiscoveryOptions* options) {
    if (options) *options = {sizeof(WcoDiscoveryOptions), WCO_DISCOVER_SYSTEM, nullptr, 0};
}
WcoStatus WCO_CALL wco_discover_runtimes(const WcoDiscoveryOptions* options, WcoRuntimeList** runtimes) {
    wco::clear_error();
    if (!runtimes) return wco::set_error(WCO_INVALID_ARGUMENT, "runtime list output is NULL");
    *runtimes = nullptr;
    try {
        wco::DiscoveryOptions config;
        const auto status = discovery_options(options, config);
        if (status != WCO_OK) return status;
        auto list = std::make_unique<WcoRuntimeList>();
        list->candidates = wco::discover_runtimes(config);
        std::erase_if(list->candidates, [](const auto& candidate) { return !native_candidate(candidate); });
        *runtimes = list.release(); return WCO_OK;
    } catch (const std::bad_alloc&) { return wco::set_error(WCO_OUT_OF_MEMORY, "discovery allocation failed"); }
      catch (const std::exception& e) { return wco::set_error(WCO_IO_ERROR, e.what()); }
      catch (...) { return wco::set_error(WCO_INTERNAL_ERROR, "runtime discovery failed"); }
}
uint32_t WCO_CALL wco_runtime_list_count(const WcoRuntimeList* runtimes) {
    return runtimes ? static_cast<uint32_t>(runtimes->candidates.size()) : 0;
}
WcoStatus WCO_CALL wco_runtime_list_get(const WcoRuntimeList* runtimes, uint32_t index, WcoRuntimeInfo* runtime) {
    wco::clear_error();
    if (!runtimes || !runtime || runtime->struct_size < sizeof(WcoRuntimeInfo) || index >= runtimes->candidates.size())
        return wco::set_error(WCO_INVALID_ARGUMENT, "invalid runtime list index or output");
    const auto& item = runtimes->candidates[index];
    *runtime = {sizeof(WcoRuntimeInfo), item.protocol, item.runtime_dir.c_str(), item.ocr_path.c_str(),
                item.version.c_str(), item.source.c_str()};
    return WCO_OK;
}
void WCO_CALL wco_runtime_list_destroy(WcoRuntimeList* runtimes) { delete runtimes; }
WcoStatus WCO_CALL wco_validate_runtime(const WcoRuntimeInfo* runtime) {
    wco::clear_error();
    if (!runtime || runtime->struct_size < sizeof(WcoRuntimeInfo) || !runtime->runtime_dir || !runtime->ocr_path ||
        !*runtime->runtime_dir || !*runtime->ocr_path || !valid_utf8(runtime->runtime_dir) || !valid_utf8(runtime->ocr_path))
        return wco::set_error(WCO_INVALID_ARGUMENT, "invalid runtime information");
    try {
        if (!path(runtime->runtime_dir).is_absolute() || !path(runtime->ocr_path).is_absolute())
            return wco::set_error(WCO_INVALID_ARGUMENT, "runtime paths must be absolute");
        if (!std::filesystem::is_directory(path(runtime->runtime_dir)) || !std::filesystem::is_regular_file(path(runtime->ocr_path)))
            return wco::set_error(WCO_RUNTIME_MISSING, "runtime directory or OCR entry does not exist");
        wco::RuntimeCandidate candidate;
        candidate.runtime_dir = runtime->runtime_dir; candidate.ocr_path = runtime->ocr_path; candidate.protocol = runtime->protocol;
        std::string error;
        if (!wco::validate_runtime(candidate, error)) return wco::set_error(WCO_RUNTIME_INCOMPATIBLE, error);
        if (!native_candidate(candidate))
            return wco::set_error(WCO_RUNTIME_INCOMPATIBLE, "runtime entry point does not match this operating system");
        return WCO_OK;
    } catch (const std::bad_alloc&) { return wco::set_error(WCO_OUT_OF_MEMORY, "validation allocation failed"); }
      catch (const std::exception& e) { return wco::set_error(WCO_IO_ERROR, e.what()); }
      catch (...) { return wco::set_error(WCO_INTERNAL_ERROR, "runtime validation failed"); }
}
WcoStatus WCO_CALL wco_create_auto(const WcoOptions* options, const WcoDiscoveryOptions* discovery, WcoEngine** engine) {
    wco::clear_error();
    if (!engine) return wco::set_error(WCO_INVALID_ARGUMENT, "engine output is NULL");
    *engine = nullptr;
    try {
        const auto validation = options_status(options);
        if (validation != WCO_OK) return validation;
        if (options && options->runtime_dir && *options->runtime_dir) return wco_create(options, engine);
        WcoOptions defaults; wco_options_init(&defaults);
        const auto& overrides = options ? *options : defaults;
        const auto budget = overrides.startup_timeout_ms ? overrides.startup_timeout_ms : 10000;
        const auto deadline = Clock::now() + std::chrono::milliseconds(budget);
        WcoRuntimeList* raw_list = nullptr;
        const auto status = wco_discover_runtimes(discovery, &raw_list);
        if (status != WCO_OK) return status;
        const std::unique_ptr<WcoRuntimeList> list(raw_list);
        if (Clock::now() >= deadline) return wco::set_error(WCO_TIMEOUT, "runtime discovery/startup deadline exceeded");
        std::vector<const wco::RuntimeCandidate*> candidates;
        for (const auto& item : list->candidates)
            if (!overrides.protocol || overrides.protocol == item.protocol) candidates.push_back(&item);
        if (candidates.empty()) return wco::set_error(WCO_RUNTIME_MISSING,
            "No WeChat OCR runtime found; install WeChat or provide runtime_dir and ocr_path");
        WcoStatus last_status = WCO_RUNTIME_INCOMPATIBLE;
        std::string last_message;
        for (size_t i = 0; i < candidates.size(); ++i) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
            if (remaining <= 0) return wco::set_error(WCO_TIMEOUT, "runtime discovery/startup deadline exceeded");
            const auto& item = *candidates[i];
            WcoOptions attempt = overrides;
            attempt.runtime_dir = item.runtime_dir.c_str(); attempt.ocr_path = item.ocr_path.c_str(); attempt.protocol = item.protocol;
            // Reserve time to try other installations if one runtime hangs.
            const auto slice = i + 1 == candidates.size() ? remaining : std::min<int64_t>(remaining, std::max<uint32_t>(250, budget / 2));
            attempt.startup_timeout_ms = static_cast<uint32_t>(slice);
            WcoEngine* created = nullptr;
            last_status = wco_create(&attempt, &created);
            std::unique_ptr<WcoEngine, decltype(&wco_destroy)> selected(created, &wco_destroy);
            if (last_status == WCO_OK) {
                const auto metadata = wco::set_runtime_metadata(created, item.version, item.source);
                if (metadata != WCO_OK) return metadata;
                *engine = selected.release(); return WCO_OK;
            }
            last_message = wco_last_error();
            if (last_status == WCO_INVALID_ARGUMENT || last_status == WCO_ABI_MISMATCH ||
                last_status == WCO_OUT_OF_MEMORY || last_status == WCO_IO_ERROR) return last_status;
        }
        return wco::set_error(last_status, last_message);
    } catch (const std::bad_alloc&) { return wco::set_error(WCO_OUT_OF_MEMORY, "automatic startup allocation failed"); }
      catch (const std::exception& e) { return wco::set_error(WCO_IO_ERROR, e.what()); }
      catch (...) { return wco::set_error(WCO_INTERNAL_ERROR, "automatic runtime startup failed"); }
}
WcoStatus WCO_CALL wco_image_view_init(WcoImageView* image, uint32_t format, const void* data, uint64_t data_size,
                                     uint32_t width, uint32_t height, uint64_t stride) {
    wco::clear_error();
    if (!image) return wco::set_error(WCO_INVALID_ARGUMENT, "image output is NULL");
    const uint32_t channels = format == WCO_PIXEL_GRAY ? 1 : format == WCO_PIXEL_BGRA ? 4 :
        (format == WCO_PIXEL_BGR || format == WCO_PIXEL_RGB) ? 3 : 0;
    WcoImageView view{sizeof(WcoImageView), format, static_cast<const uint8_t*>(data), data_size, width, height,
                      stride ? stride : uint64_t(width) * channels};
    *image = {};
    if (!wco::valid_image(&view)) return wco::set_error(WCO_INVALID_ARGUMENT, "invalid image dimensions, format, stride or byte bounds");
    *image = view; return WCO_OK;
}
}
