// SPDX-License-Identifier: AGPL-3.0-only
#include "WeChatOCR/Toolkit.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}
void status(WcoStatus actual, WcoStatus expected, const char* operation) {
    require(actual == expected, std::string(operation) + ": expected " + wco_status_string(expected) +
            ", received " + wco_status_string(actual) + " (" + wco_last_error() + ")");
}
std::filesystem::path utf8_path(std::string_view value) {
    return std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(value.data()), value.size()));
}
std::string utf8(const std::filesystem::path& value) {
    const auto text = value.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

void binary_fixture(const std::filesystem::path& destination) {
    std::filesystem::create_directories(destination.parent_path());
    std::array<uint8_t, 512> header{};
#ifdef _WIN32
    header[0] = 'M'; header[1] = 'Z'; header[0x3c] = 0x80;
    header[0x80] = 'P'; header[0x81] = 'E';
    header[0x84] = 0x64; header[0x85] = 0x86;
    header[0x86] = 1; header[0x94] = 0xf0;
    header[0x98] = 0x0b; header[0x99] = 2;
#else
    header[0] = 0x7f; header[1] = 'E'; header[2] = 'L'; header[3] = 'F';
    header[4] = 2; header[5] = 1; header[6] = 1;
    header[16] = 2; header[18] = 62; header[20] = 1; header[52] = 64;
#endif
    std::ofstream file(destination, std::ios::binary);
    file.write(reinterpret_cast<const char*>(header.data()), header.size());
    require(bool(file), "cannot create binary discovery fixture");
}

struct Sandbox {
    std::filesystem::path base, root, runtime, entry, model;
    explicit Sandbox(const std::string& runtime_name = "4.0.1.12") {
        const auto identifier = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
            + "-" + std::to_string(std::random_device{}());
        base = std::filesystem::temp_directory_path() / ("wco-toolkit-" + identifier);
        root = base / utf8_path("custom runtime 根 with spaces");
#ifdef _WIN32
        runtime = root / "Weixin" / runtime_name;
        entry = root / "Plugins" / "WeChatOcr" / "8011" / "extracted" / "wxocr.dll";
        binary_fixture(runtime / "mmmojo_64.dll");
        binary_fixture(runtime.parent_path() / "weixin.exe");
        model = entry.parent_path() / "Model" / "test.xnet";
#else
        runtime = root / runtime_name;
        entry = runtime / "wxocr";
        binary_fixture(runtime / "libmmmojo.so");
        binary_fixture(runtime / "libwxocr.so");
        model = runtime / "ocr_model" / "test.xnet";
#endif
        binary_fixture(entry);
        std::filesystem::create_directories(model.parent_path());
        std::ofstream(model, std::ios::binary) << "discovery fixture model";
    }
    ~Sandbox() {
        std::error_code error;
        std::filesystem::remove_all(base, error);
    }
};

using List = std::unique_ptr<WcoRuntimeList, decltype(&wco_runtime_list_destroy)>;
using Engine = std::unique_ptr<WcoEngine, decltype(&wco_destroy)>;
using Result = std::unique_ptr<WcoResult, decltype(&wco_result_destroy)>;

struct Scan {
    std::vector<std::string> storage;
    std::vector<const char*> roots;
    WcoDiscoveryOptions options{};
    explicit Scan(std::initializer_list<std::filesystem::path> paths) {
        wco_discovery_options_init(&options);
        options.flags = 0; // Never let tests select an incidental host install.
        for (const auto& path : paths) storage.push_back(utf8(path));
        for (const auto& path : storage) roots.push_back(path.c_str());
        options.search_roots = roots.data();
        options.search_root_count = static_cast<uint32_t>(roots.size());
    }
    List discover() const {
        WcoRuntimeList* raw = nullptr;
        status(wco_discover_runtimes(&options, &raw), WCO_OK, "discover custom roots");
        require(raw != nullptr, "successful discovery must return an owned list, including when empty");
        return List(raw, wco_runtime_list_destroy);
    }
};

WcoRuntimeInfo candidate(const List& list, uint32_t index = 0) {
    WcoRuntimeInfo info{}; info.struct_size = sizeof(info);
    status(wco_runtime_list_get(list.get(), index, &info), WCO_OK, "get candidate");
    require(info.runtime_dir && info.ocr_path && info.version && info.source, "candidate has NULL strings");
    require(info.protocol == 4, "fixture discovered with the wrong protocol");
    return info;
}

void recognize(WcoEngine* engine) {
    const std::array<uint8_t, 12> pixels{1,2,3,4,5,6,7,8,9,10,11,12};
    WcoImageView image{};
    status(wco_image_view_init(&image, WCO_PIXEL_BGR, pixels.data(), pixels.size(), 2, 2, 0), WCO_OK, "init image");
    WcoResult* raw = nullptr;
    status(wco_recognize(engine, &image, 2000, &raw), WCO_OK, "recognize auto-selected runtime");
    Result result(raw, wco_result_destroy);
    require(result && wco_result_line_count(result.get()) == 1, "auto engine returned no line");
}

void test_defaults_and_images() {
    WcoOptions options{};
    wco_options_init(&options);
    require(options.struct_size == sizeof(options) && options.abi_version == WCO_ABI_VERSION, "options ABI defaults");
    require(!options.runtime_dir && !options.ocr_path && !options.worker_path && options.protocol == 0,
            "options defaults should enable automatic runtime and worker selection");
    WcoDiscoveryOptions discovery{};
    wco_discovery_options_init(&discovery);
    require(discovery.struct_size == sizeof(discovery) && discovery.flags == WCO_DISCOVER_SYSTEM &&
            !discovery.search_roots && !discovery.search_root_count, "discovery defaults");
    wco_options_init(nullptr);
    wco_discovery_options_init(nullptr);

    std::array<uint8_t, 16> pixels{};
    WcoImageView image{};
    status(wco_image_view_init(&image, WCO_PIXEL_BGR, pixels.data(), 12, 2, 2, 0), WCO_OK, "packed image");
    require(image.struct_size == sizeof(image) && image.stride == 6 && image.data == pixels.data(), "packed image metadata");
    for (const auto& [format, channels] : std::array<std::pair<uint32_t, uint32_t>, 3>{
             {{WCO_PIXEL_BGRA, 4}, {WCO_PIXEL_RGB, 3}, {WCO_PIXEL_GRAY, 1}}}) {
        status(wco_image_view_init(&image, format, pixels.data(), pixels.size(), 2, 2, 0), WCO_OK, "image format initialization");
        require(image.format == format && image.stride == 2 * channels, "incorrect image format/stride metadata");
    }
    status(wco_image_view_init(&image, WCO_PIXEL_BGR, pixels.data(), 14, 2, 2, 8), WCO_OK, "padded ROI image");
    require(image.stride == 8 && image.data_size == 14, "padded image metadata");
    status(wco_image_view_init(&image, WCO_PIXEL_BGR, pixels.data(), 13, 2, 2, 8), WCO_INVALID_ARGUMENT, "short ROI buffer");
    status(wco_image_view_init(&image, WCO_PIXEL_BGR, pixels.data(), pixels.size(), 2, 2,
                               std::numeric_limits<uint64_t>::max()), WCO_INVALID_ARGUMENT, "overflowed stride");
    status(wco_image_view_init(&image, 999, pixels.data(), pixels.size(), 2, 2, 0), WCO_INVALID_ARGUMENT, "invalid format");
    status(wco_image_view_init(nullptr, WCO_PIXEL_BGR, pixels.data(), 12, 2, 2, 0), WCO_INVALID_ARGUMENT, "NULL image output");
    status(wco_image_view_init(&image, WCO_PIXEL_BGR, nullptr, 12, 2, 2, 0), WCO_INVALID_ARGUMENT, "NULL pixels");
}

void test_lists_and_validation() {
    Sandbox box;
    Scan scan{box.root, box.root / "."};
    auto list = scan.discover();
    require(wco_runtime_list_count(list.get()) == 1, "equivalent custom roots were not deduplicated");
    auto info = candidate(list);
    require(std::filesystem::equivalent(utf8_path(info.runtime_dir), box.runtime) &&
            std::filesystem::equivalent(utf8_path(info.ocr_path), box.entry), "incorrect discovered paths");
    require(std::string(info.source).size() > 0, "discovery provenance is empty");
    status(wco_validate_runtime(&info), WCO_OK, "validate discovered candidate");
    const std::string remembered_runtime = info.runtime_dir;
    const std::string remembered_entry = info.ocr_path;
    // The list must own its strings, independently of caller input and later lists.
    for (auto& root : scan.storage) root.assign(2048, 'x');
    Sandbox other;
    { Scan other_scan{other.root}; auto other_list = other_scan.discover(); candidate(other_list); }
    require(info.runtime_dir == remembered_runtime && info.ocr_path == remembered_entry, "candidate lifetime depends on scan inputs or another list");

    WcoRuntimeInfo too_small{};
    status(wco_runtime_list_get(list.get(), 0, &too_small), WCO_INVALID_ARGUMENT, "short candidate output");
    status(wco_runtime_list_get(list.get(), 1, &info), WCO_INVALID_ARGUMENT, "candidate index past end");
    status(wco_validate_runtime(nullptr), WCO_INVALID_ARGUMENT, "NULL runtime validation");
    status(wco_validate_runtime(&too_small), WCO_INVALID_ARGUMENT, "short runtime validation");
    info = candidate(list);
    std::filesystem::remove(box.model);
    require(wco_validate_runtime(&info) != WCO_OK, "validation accepted missing model files");
    std::ofstream(box.model) << "restored model";
    status(wco_validate_runtime(&info), WCO_OK, "validate restored runtime");
    std::ofstream(box.entry, std::ios::binary | std::ios::trunc) << "incorrect architecture";
    require(wco_validate_runtime(&info) != WCO_OK, "validation accepted an invalid executable");
    require(wco_runtime_list_count(nullptr) == 0, "NULL list count");
    wco_runtime_list_destroy(nullptr);
}

void test_discovery_errors_and_empty() {
    Scan empty{};
    auto list = empty.discover();
    require(wco_runtime_list_count(list.get()) == 0, "custom-only empty scan consulted a host installation");
    WcoRuntimeList* raw = reinterpret_cast<WcoRuntimeList*>(uintptr_t(1));
    auto invalid = empty.options; invalid.struct_size = 0;
    status(wco_discover_runtimes(&invalid, &raw), WCO_INVALID_ARGUMENT, "short discovery options");
    require(raw == nullptr, "discovery failure did not clear its output");
    invalid = empty.options; invalid.flags = 0x80000000u;
    status(wco_discover_runtimes(&invalid, &raw), WCO_INVALID_ARGUMENT, "unknown discovery flags");
    invalid = empty.options; invalid.search_root_count = 1; invalid.search_roots = nullptr;
    status(wco_discover_runtimes(&invalid, &raw), WCO_INVALID_ARGUMENT, "NULL roots array");
    const char* invalid_root = "relative/runtime";
    invalid.search_roots = &invalid_root;
    status(wco_discover_runtimes(&invalid, &raw), WCO_INVALID_ARGUMENT, "relative custom root");
    invalid_root = "\xC0\xAF";
    status(wco_discover_runtimes(&invalid, &raw), WCO_INVALID_ARGUMENT, "invalid UTF-8 custom root");
    status(wco_discover_runtimes(&empty.options, nullptr), WCO_INVALID_ARGUMENT, "NULL discovery output");
    WcoEngine* engine = reinterpret_cast<WcoEngine*>(uintptr_t(1));
    status(wco_create_auto(nullptr, &empty.options, &engine), WCO_RUNTIME_MISSING, "NULL/default auto options without host scan");
    require(!engine, "failed auto creation did not clear its output");
    status(wco_create(nullptr, nullptr), WCO_INVALID_ARGUMENT, "NULL output with automatic defaults");
}

void test_auto_create_and_engine_info(const std::filesystem::path& worker) {
    Sandbox box;
    Scan scan{box.root};
    WcoOptions options{}; wco_options_init(&options);
    const std::string worker_path = utf8(worker);
    options.worker_path = worker_path.c_str(); options.startup_timeout_ms = 2000;
    WcoEngine* raw = nullptr;
    status(wco_create_auto(&options, &scan.options, &raw), WCO_OK, "automatic custom-root creation");
    Engine engine(raw, wco_destroy);
    WcoRuntimeInfo selected{}; selected.struct_size = sizeof(selected);
    status(wco_engine_get_runtime(engine.get(), &selected), WCO_OK, "selected runtime info");
    require(std::filesystem::equivalent(utf8_path(selected.runtime_dir), box.runtime), "engine reports a different runtime");
    const std::string remembered_runtime = selected.runtime_dir;
    auto list = scan.discover(); auto discovered = candidate(list);
    options.runtime_dir = discovered.runtime_dir; options.ocr_path = discovered.ocr_path; options.protocol = discovered.protocol;
    Scan no_roots{};
    WcoEngine* explicit_raw = nullptr;
    status(wco_create_auto(&options, &no_roots.options, &explicit_raw), WCO_OK, "explicit runtime overrides empty discovery");
    Engine explicit_engine(explicit_raw, wco_destroy);
    list.reset(); // The engine must copy candidate paths before the list dies.
    recognize(explicit_engine.get());
    recognize(engine.get());
    require(selected.runtime_dir == remembered_runtime, "engine info strings were invalidated by other calls");
    status(wco_engine_get_runtime(nullptr, &selected), WCO_INVALID_ARGUMENT, "NULL engine info");
    selected.struct_size = 0;
    status(wco_engine_get_runtime(engine.get(), &selected), WCO_INVALID_ARGUMENT, "short engine info output");
    options.runtime_dir = nullptr;
    // Keep a valid string alive while testing a partially explicit pair.
    const std::string entry_path = utf8(box.entry); options.ocr_path = entry_path.c_str();
    raw = nullptr;
    status(wco_create_auto(&options, &scan.options, &raw), WCO_INVALID_ARGUMENT, "partial explicit runtime pair");
    require(!raw, "partial explicit pair returned an engine");
}

void control_startup(const std::filesystem::path& runtime, const std::string& mode) {
    std::ofstream(runtime / "toolkit-worker-mode") << mode;
}
size_t startup_attempts(const Sandbox& box) {
    std::ifstream file(box.runtime / "toolkit-startup-entered");
    size_t count = 0;
    for (std::string line; std::getline(file, line);) ++count;
    return count;
}

void test_candidate_fallback_and_budget(const std::filesystem::path& worker) {
    Sandbox first("9.0.0"), second("8.0.0");
    Scan scan{first.root, second.root};
    auto list = scan.discover();
    require(wco_runtime_list_count(list.get()) >= 2, "fallback fixture needs multiple runtime candidates");
    const auto initial = candidate(list);
    control_startup(utf8_path(initial.runtime_dir), "startup_error");
    WcoOptions options{}; wco_options_init(&options);
    const std::string worker_path = utf8(worker);
    options.worker_path = worker_path.c_str(); options.startup_timeout_ms = 2000;
    WcoEngine* raw = nullptr;
    status(wco_create_auto(&options, &scan.options, &raw), WCO_OK, "fallback after incompatible candidate");
    Engine engine(raw, wco_destroy);
    require(startup_attempts(first) + startup_attempts(second) >= 1, "fallback never tried the rejected candidate");
    WcoRuntimeInfo selected{}; selected.struct_size = sizeof(selected);
    status(wco_engine_get_runtime(engine.get(), &selected), WCO_OK, "fallback selected runtime");
    require(std::string(selected.runtime_dir) != initial.runtime_dir, "fallback retained a rejected runtime");
    recognize(engine.get());
    engine.reset();

    std::filesystem::remove(first.runtime / "toolkit-startup-entered");
    std::filesystem::remove(second.runtime / "toolkit-startup-entered");
    control_startup(first.runtime, "startup_hang");
    control_startup(second.runtime, "startup_hang");
    options.startup_timeout_ms = 150;
    raw = nullptr;
    const auto before = std::chrono::steady_clock::now();
    status(wco_create_auto(&options, &scan.options, &raw), WCO_TIMEOUT, "shared auto-create deadline");
    require(!raw, "timed-out auto creation returned an engine");
    require(startup_attempts(first) + startup_attempts(second) == 1, "auto-create restarted a fresh startup budget for another candidate");
    require(std::chrono::steady_clock::now() - before < std::chrono::seconds(2), "auto-create budget did not bound startup attempts");
}
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: toolkit_test FAKE_WORKER");
        const auto worker = std::filesystem::absolute(utf8_path(argv[1]));
        test_defaults_and_images();
        test_lists_and_validation();
        test_discovery_errors_and_empty();
        test_auto_create_and_engine_info(worker);
        test_candidate_fallback_and_budget(worker);
        std::cout << "Toolkit defaults, discovery, ownership, validation and auto-create tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
