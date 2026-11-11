#include "WeChatOCR/WeChatOCR.h"

#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using Engine = std::unique_ptr<WcoEngine, decltype(&wco_destroy)>;
using Result = std::unique_ptr<WcoResult, decltype(&wco_result_destroy)>;
std::filesystem::path worker;

void require(bool condition, const char* expression, int line) {
    if (!condition) throw std::runtime_error("line " + std::to_string(line) + ": " + expression);
}

void require_status(WcoStatus actual, WcoStatus expected, int line) {
    if (actual == expected) return;
    throw std::runtime_error("line " + std::to_string(line) + ": expected " + wco_status_string(expected) +
                             ", received " + wco_status_string(actual) + " (" + wco_last_error() + ")");
}
#define REQUIRE(expression) require(bool(expression), #expression, __LINE__)
#define STATUS(expression, expected) require_status((expression), (expected), __LINE__)

std::string utf8(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

struct Runtime {
    std::filesystem::path directory;
    std::filesystem::path entry;
    std::string directory_string;
    std::string entry_string;
    std::string worker_string;
    WcoOptions options{};

    explicit Runtime(const char* mode) {
        static uint64_t sequence = 0;
        directory = std::filesystem::temp_directory_path() /
                    std::filesystem::u8path("wco-\xE6\xB5\x8B\xE8\xAF\x95 with spaces-") /
                    (std::to_string(Clock::now().time_since_epoch().count()) + "-" + std::to_string(sequence++));
        std::filesystem::create_directories(directory);
        entry = directory / mode;
        std::ofstream(entry, std::ios::binary) << "controlled runtime placeholder";
        directory_string = utf8(directory);
        entry_string = utf8(entry);
        worker_string = utf8(worker);
        options.struct_size = sizeof(options);
        options.abi_version = WCO_ABI_VERSION;
        options.runtime_dir = directory_string.c_str();
        options.ocr_path = entry_string.c_str();
        options.worker_path = worker_string.c_str();
        options.protocol = 4;
        options.startup_timeout_ms = 2000;
    }

    ~Runtime() {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }

    Engine create() {
        WcoEngine* engine = nullptr;
        STATUS(wco_create(&options, &engine), WCO_OK);
        REQUIRE(engine != nullptr);
        return Engine(engine, &wco_destroy);
    }

    void wait_entered() const {
        auto marker = entry;
        marker += ".entered";
        const auto deadline = Clock::now() + std::chrono::seconds(2);
        while (Clock::now() < deadline) {
            if (std::filesystem::exists(marker)) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        throw std::runtime_error("controlled worker never received its recognition request");
    }
};

struct Image {
    std::array<uint8_t, 16> bytes{1, 2, 3, 4, 5, 6, 0xee, 0xee, 7, 8, 9, 10, 11, 12, 0xdd, 0xdd};
    WcoImageView view{};
    Image() {
        view.struct_size = sizeof(view);
        view.format = WCO_PIXEL_BGR;
        view.data = bytes.data();
        view.data_size = bytes.size();
        view.width = 2;
        view.height = 2;
        view.stride = 8;
    }
};

Result recognize(WcoEngine* engine, const WcoImageView& image, uint32_t timeout = 2000) {
    WcoResult* result = nullptr;
    STATUS(wco_recognize(engine, &image, timeout, &result), WCO_OK);
    REQUIRE(result != nullptr);
    return Result(result, &wco_result_destroy);
}

void test_api_validation() {
    WcoEngine* engine = reinterpret_cast<WcoEngine*>(uintptr_t(1));
    STATUS(wco_create(nullptr, &engine), WCO_INVALID_ARGUMENT);
    REQUIRE(engine == nullptr);
    STATUS(wco_create(nullptr, nullptr), WCO_INVALID_ARGUMENT);
    Runtime runtime("success");
    auto options = runtime.options;
    options.struct_size = sizeof(uint32_t);
    STATUS(wco_create(&options, &engine), WCO_INVALID_ARGUMENT);
    options = runtime.options;
    options.abi_version = WCO_ABI_VERSION + 1;
    STATUS(wco_create(&options, &engine), WCO_ABI_MISMATCH);
    REQUIRE(engine == nullptr);
    options = runtime.options;
    options.runtime_dir = "relative/path";
    STATUS(wco_create(&options, &engine), WCO_INVALID_ARGUMENT);
    options = runtime.options;
    options.protocol = 2;
    STATUS(wco_create(&options, &engine), WCO_INVALID_ARGUMENT);
    options = runtime.options;
    const auto missing = utf8(runtime.directory / "does-not-exist");
    options.ocr_path = missing.c_str();
    STATUS(wco_create(&options, &engine), WCO_RUNTIME_MISSING);
    options = runtime.options;
    options.worker_path = missing.c_str();
    STATUS(wco_create(&options, &engine), WCO_WORKER_ERROR);
    REQUIRE(std::string(wco_last_error()).size() > 0);
    REQUIRE(std::string(wco_version()) == WCO_VERSION);
    REQUIRE(std::string(wco_status_string(999)) == "Unknown status");
    wco_destroy(nullptr);
    wco_result_destroy(nullptr);
}

void test_pixels_and_results() {
    Runtime runtime("success");
    auto engine = runtime.create();
    Image image;
    auto result = recognize(engine.get(), image.view);
    REQUIRE(wco_result_line_count(result.get()) == 1);
    WcoResultInfo info{};
    info.struct_size = sizeof(info);
    STATUS(wco_result_get_info(result.get(), &info), WCO_OK);
    REQUIRE(info.width == 2 && info.height == 2 && info.vendor_error == 0);
    WcoLine line{};
    line.struct_size = sizeof(line);
    STATUS(wco_result_get_line(result.get(), 0, &line), WCO_OK);
    const std::string expected = "\xE5\xBC\x80\xE5\xA7\x8B\xE2\x9C\x93:0102030405060708090a0b0c";
    REQUIRE(std::string(line.text, static_cast<size_t>(line.text_length)) == expected);
    REQUIRE(line.has_confidence == 1 && std::abs(line.confidence - 0.875f) < 0.00001f);
    REQUIRE(line.quad[0] == 0 && line.quad[2] == 2 && line.quad[5] == 2);
    const auto borrowed = line.text;
    auto second = recognize(engine.get(), image.view);
    REQUIRE(std::string(borrowed) == expected);
    engine.reset();
    REQUIRE(std::string(borrowed) == expected);
    STATUS(wco_result_get_line(result.get(), 1, &line), WCO_INVALID_ARGUMENT);
    line.struct_size = 0;
    STATUS(wco_result_get_line(result.get(), 0, &line), WCO_INVALID_ARGUMENT);
    info.struct_size = 0;
    STATUS(wco_result_get_info(result.get(), &info), WCO_INVALID_ARGUMENT);
}

void test_formats_and_capabilities() {
    Runtime runtime("success");
    auto engine = runtime.create();
    WcoCapabilities capabilities{};
    capabilities.struct_size = sizeof(capabilities);
    STATUS(wco_get_capabilities(engine.get(), &capabilities), WCO_OK);
    REQUIRE(capabilities.abi_version == WCO_ABI_VERSION);
    REQUIRE(capabilities.detect_and_recognize == 1);
    REQUIRE(capabilities.recognize_only == 0 && capabilities.native_batch == 0 && capabilities.native_memory_input == 0);
    for (const uint32_t format : {WCO_PIXEL_GRAY, WCO_PIXEL_RGB, WCO_PIXEL_BGRA}) {
        Image image;
        image.view.format = format;
        auto result = recognize(engine.get(), image.view);
        REQUIRE(wco_result_line_count(result.get()) == 1);
    }
}

void test_invalid_images() {
    Runtime runtime("success");
    auto engine = runtime.create();
    Image image;
    const auto reject = [&](WcoImageView view) {
        WcoResult* result = reinterpret_cast<WcoResult*>(uintptr_t(1));
        STATUS(wco_recognize(engine.get(), &view, 100, &result), WCO_INVALID_ARGUMENT);
        REQUIRE(result == nullptr);
    };
    auto view = image.view; view.struct_size = 0; reject(view);
    view = image.view; view.format = 99; reject(view);
    view = image.view; view.data = nullptr; reject(view);
    view = image.view; view.width = 0; reject(view);
    view = image.view; view.height = 0; reject(view);
    view = image.view; view.width = std::numeric_limits<uint32_t>::max(); reject(view);
    view = image.view; view.stride = 5; reject(view);
    view = image.view; view.data_size = 13; reject(view);
    view = image.view; view.stride = std::numeric_limits<uint64_t>::max(); reject(view);
    view = image.view; view.height = 3; view.stride = std::numeric_limits<uint64_t>::max() / 2; reject(view);
    view = image.view; view.width = 32768; view.height = 32768; view.stride = uint64_t(32768) * 3;
    view.data_size = std::numeric_limits<uint64_t>::max(); reject(view);
    WcoResult* result = nullptr;
    STATUS(wco_recognize(engine.get(), &image.view, 0, &result), WCO_INVALID_ARGUMENT);
    STATUS(wco_recognize(engine.get(), &image.view, 100, nullptr), WCO_INVALID_ARGUMENT);
    STATUS(wco_recognize(nullptr, &image.view, 100, &result), WCO_INVALID_ARGUMENT);
    REQUIRE(recognize(engine.get(), image.view) != nullptr);
}

void test_empty_and_vendor_error() {
    Image image;
    Runtime empty("empty");
    auto engine = empty.create();
    auto result = recognize(engine.get(), image.view);
    REQUIRE(wco_result_line_count(result.get()) == 0);
    Runtime vendor_error("vendor_error");
    auto other = vendor_error.create();
    WcoResult* failed = nullptr;
    STATUS(wco_recognize(other.get(), &image.view, 1000, &failed), WCO_OCR_FAILED);
    REQUIRE(failed == nullptr);
}

void test_bad_workers() {
    Image image;
    for (const char* mode : {"crash", "malformed", "oversize", "truncated", "wrong_request",
                             "bad_utf8", "bad_dimensions", "nan_score"}) {
        Runtime runtime(mode);
        auto engine = runtime.create();
        WcoResult* result = nullptr;
        const auto start = Clock::now();
        STATUS(wco_recognize(engine.get(), &image.view, 1000, &result), WCO_WORKER_ERROR);
        REQUIRE(result == nullptr);
        REQUIRE(Clock::now() - start < std::chrono::seconds(3));
    }
}

void test_startup_failures() {
    Runtime failure("startup_error");
    WcoEngine* engine = nullptr;
    STATUS(wco_create(&failure.options, &engine), WCO_RUNTIME_INCOMPATIBLE);
    REQUIRE(engine == nullptr);
    Runtime hung("startup_hang");
    hung.options.startup_timeout_ms = 80;
    const auto start = Clock::now();
    STATUS(wco_create(&hung.options, &engine), WCO_TIMEOUT);
    REQUIRE(engine == nullptr);
    REQUIRE(Clock::now() - start < std::chrono::seconds(3));
}

void test_timeout_and_recovery() {
    Runtime runtime("recover");
    auto engine = runtime.create();
    Image image;
    WcoResult* result = nullptr;
    const auto start = Clock::now();
    STATUS(wco_recognize(engine.get(), &image.view, 80, &result), WCO_TIMEOUT);
    REQUIRE(result == nullptr);
    REQUIRE(Clock::now() - start < std::chrono::seconds(3));
    REQUIRE(wco_result_line_count(recognize(engine.get(), image.view).get()) == 1);
}

struct Outcome { WcoStatus status; WcoResult* result; };

void test_queue_busy() {
    Runtime runtime("slow");
    runtime.options.max_pending_requests = 1;
    auto engine = runtime.create();
    Image image;
    auto active = std::async(std::launch::async, [&] {
        Outcome outcome{};
        outcome.status = wco_recognize(engine.get(), &image.view, 2000, &outcome.result);
        return outcome;
    });
    runtime.wait_entered();
    WcoResult* rejected = nullptr;
    STATUS(wco_recognize(engine.get(), &image.view, 1000, &rejected), WCO_BUSY);
    REQUIRE(rejected == nullptr);
    const auto outcome = active.get();
    Result result(outcome.result, &wco_result_destroy);
    STATUS(outcome.status, WCO_OK);
    REQUIRE(result != nullptr);
}

void test_queue_timeout() {
    Runtime runtime("slow");
    runtime.options.max_pending_requests = 2;
    auto engine = runtime.create();
    Image image;
    auto active = std::async(std::launch::async, [&] {
        Outcome outcome{};
        outcome.status = wco_recognize(engine.get(), &image.view, 2000, &outcome.result);
        return outcome;
    });
    runtime.wait_entered();
    WcoResult* rejected = nullptr;
    const auto start = Clock::now();
    STATUS(wco_recognize(engine.get(), &image.view, 30, &rejected), WCO_TIMEOUT);
    REQUIRE(rejected == nullptr);
    REQUIRE(Clock::now() - start < std::chrono::milliseconds(250));
    const auto outcome = active.get();
    Result result(outcome.result, &wco_result_destroy);
    STATUS(outcome.status, WCO_OK);
    REQUIRE(result != nullptr);
    REQUIRE(recognize(engine.get(), image.view) != nullptr);
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: sdk_test /absolute/path/to/fake_worker\n";
        return 2;
    }
    worker = std::filesystem::absolute(std::filesystem::u8path(argv[1]));
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"C ABI validation", test_api_validation},
        {"non-contiguous image, UTF-8 and result lifetime", test_pixels_and_results},
        {"image formats and capabilities", test_formats_and_capabilities},
        {"invalid images and integer overflow", test_invalid_images},
        {"empty OCR and vendor error", test_empty_and_vendor_error},
        {"crash and malformed workers", test_bad_workers},
        {"startup error and timeout", test_startup_failures},
        {"timeout followed by recovery", test_timeout_and_recovery},
        {"bounded request queue", test_queue_busy},
        {"queue deadline preserves active request", test_queue_timeout},
    };
    unsigned failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    return failures ? 1 : 0;
}
