// SPDX-License-Identifier: AGPL-3.0-only
#include "WeChatOCR/WeChatOCR.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
void require(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
std::filesystem::path utf8_path(std::string_view value)
{
    return std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(value.data()), value.size()));
}
std::string utf8(const std::filesystem::path& path)
{
    const auto value = path.u8string(); return {reinterpret_cast<const char*>(value.data()), value.size()};
}
struct Sandbox {
    std::filesystem::path base, runtime, temps;
    Sandbox(const std::filesystem::path& library)
    {
        base = std::filesystem::temp_directory_path() / ("wco-foreign-test-" + std::to_string(std::random_device{}()));
        runtime = base / utf8_path("runtime 空格"); temps = base / "temporary";
        std::filesystem::create_directories(runtime); std::filesystem::create_directory(temps);
        std::ofstream(temps / "user-owned.txt") << "keep user temporary files";
#ifdef _WIN32
        std::filesystem::copy_file(library, runtime / "mmmojo_64.dll");
        std::ofstream(runtime / "WeChatOCR.exe"); std::ofstream(runtime / "wxocr.dll"); std::ofstream(base / "weixin.exe");
#else
        std::filesystem::copy_file(library, runtime / "libmmmojo.so"); std::ofstream(runtime / "wxocr");
#endif
    }
    ~Sandbox() { std::error_code ignored; std::filesystem::remove_all(base, ignored); }
    void mode(const std::string& value) { std::ofstream(runtime / "fake-mode") << value; }
};
using Engine = std::unique_ptr<WcoEngine, decltype(&wco_destroy)>;
using Result = std::unique_ptr<WcoResult, decltype(&wco_result_destroy)>;

Engine engine(Sandbox& box, const std::filesystem::path& worker, uint32_t protocol, WcoStatus expected = WCO_OK)
{
    const std::string runtime = utf8(box.runtime), temp = utf8(box.temps), worker_path = utf8(worker);
#ifdef _WIN32
    const std::string entry = utf8(box.runtime / (protocol == 3 ? "WeChatOCR.exe" : "wxocr.dll"));
#else
    const std::string entry = utf8(box.runtime / "wxocr");
#endif
    WcoOptions options{sizeof(WcoOptions), WCO_ABI_VERSION, runtime.c_str(), entry.c_str(),
                       worker_path.c_str(), temp.c_str(), protocol, 500, 16};
    WcoEngine* raw = nullptr;
    const auto status = wco_create(&options, &raw);
    require(status == expected, "unexpected creation status " + std::to_string(status) + ": " + wco_last_error());
    require((status == WCO_OK) == (raw != nullptr), "creation output contract");
    return Engine(raw, wco_destroy);
}

Result recognize(WcoEngine* instance, uint32_t format, const std::vector<uint8_t>& data,
                 uint64_t stride, WcoStatus expected = WCO_OK, uint32_t timeout = 1000)
{
    WcoImageView image{sizeof(WcoImageView), format, data.data(), data.size(), 2, 2, stride};
    WcoResult* raw = nullptr;
    const auto status = wco_recognize(instance, &image, timeout, &raw);
    require(status == expected, "unexpected recognition status " + std::to_string(status) + ": " + wco_last_error());
    require((status == WCO_OK) == (raw != nullptr), "recognition output contract");
    return Result(raw, wco_result_destroy);
}

void line(const Result& result, const std::string& text, bool score = true)
{
    require(wco_result_line_count(result.get()) == 1, "expected one decoded OCR line");
    WcoResultInfo info{sizeof(WcoResultInfo),0,0,0};
    require(wco_result_get_info(result.get(), &info) == WCO_OK && info.width == 2 && info.height == 2 && !info.vendor_error,
            "decoded image metadata");
    WcoLine item{}; item.struct_size = sizeof(item);
    require(wco_result_get_line(result.get(), 0, &item) == WCO_OK, "line accessor");
    require(std::string(item.text, size_t(item.text_length)) == text, "PNG pixels changed in the real worker path");
    require(item.has_confidence == uint32_t(score) && (!score || item.confidence == .875f), "confidence presence/value");
    require(item.quad[4] == 1.5f && item.quad[7] == 1.5f, "rotated quad was flattened");
}

void temporary_removed(Sandbox& box)
{
    std::string path; std::getline(std::ifstream(box.runtime / "last-image-path"), path);
    require(!path.empty() && !std::filesystem::exists(utf8_path(path)), "temporary PNG survived its response");
    for (const auto& directory : std::filesystem::directory_iterator(box.temps)) {
        if (directory.path().filename() == "user-owned.txt") continue;
        require(std::filesystem::is_empty(directory.path()), "private temporary directory still contains files");
    }
    std::string user_file; std::getline(std::ifstream(box.temps / "user-owned.txt"), user_file);
    require(user_file == "keep user temporary files", "worker cleanup modified the user's temporary base");
}

void private_directories_removed(Sandbox& box)
{
    for (const auto& file : std::filesystem::directory_iterator(box.temps))
        require(file.path().filename() == "user-owned.txt", "engine destruction left its private temporary directory");
    require(std::filesystem::is_regular_file(box.temps / "user-owned.txt"), "engine destruction removed user files");
}

void tests(const std::filesystem::path& worker, const std::filesystem::path& library, uint32_t version)
{
    Sandbox box(library); box.mode("ok");
    auto instance = engine(box, worker, version);
    const std::string rgb = "像素:ff000000ff000000ffffffff";
    const std::vector<uint8_t> bgr{0,0,255, 0,255,0, 47,48, 255,0,0, 255,255,255, 49,50};
    line(recognize(instance.get(), WCO_PIXEL_BGR, bgr, 8), rgb);
    temporary_removed(box);
    line(recognize(instance.get(), WCO_PIXEL_RGB, {255,0,0, 0,255,0, 0,0,255, 255,255,255}, 6), rgb);
    line(recognize(instance.get(), WCO_PIXEL_BGRA, {0,0,255,0, 0,255,0,73, 255,0,0,149, 255,255,255,255}, 8), rgb);
    line(recognize(instance.get(), WCO_PIXEL_GRAY, {0,64,128,255}, 2), "像素:004080ff");
    instance.reset();
    private_directories_removed(box);

#ifdef _WIN32
    box.mode("locked-input"); instance = engine(box, worker, version);
    line(recognize(instance.get(), WCO_PIXEL_BGR, bgr, 8), rgb);
    temporary_removed(box);
    instance.reset();
    private_directories_removed(box);
    for (const uint32_t timeout : {50u, 1000u}) {
        box.mode("locked-input-persistent"); instance = engine(box, worker, version);
        recognize(instance.get(), WCO_PIXEL_BGR, bgr, 8,
                  timeout == 50 ? WCO_TIMEOUT : WCO_WORKER_ERROR, timeout);
        temporary_removed(box);
        box.mode("ok");
        line(recognize(instance.get(), WCO_PIXEL_BGR, bgr, 8), rgb);
        temporary_removed(box);
        instance.reset();
        private_directories_removed(box);
    }
#endif

    for (const std::string mode : {"no-score", "empty", "vendor-error", "hang", "disconnect", "malformed", "send-fail"}) {
        box.mode(mode); instance = engine(box, worker, version);
        if (mode == "no-score") line(recognize(instance.get(), WCO_PIXEL_BGR, bgr, 8), rgb, false);
        else if (mode == "empty") require(wco_result_line_count(recognize(instance.get(), WCO_PIXEL_BGR, bgr, 8).get()) == 0, "empty OCR result");
        else {
            const WcoStatus expected = mode == "hang" ? WCO_TIMEOUT
                : mode == "vendor-error" ? WCO_OCR_FAILED : WCO_WORKER_ERROR;
            recognize(instance.get(), WCO_PIXEL_BGR, bgr, 8, expected, 150);
            // Check before recovery or destruction gets a chance to clean up
            // the image left behind by a terminated, in-flight worker.
            temporary_removed(box);
            box.mode("ok");
            if (mode != "vendor-error") {
                line(recognize(instance.get(), WCO_PIXEL_BGR, bgr, 8), rgb);
                temporary_removed(box);
            }
        }
        instance.reset();
        private_directories_removed(box);
    }
    box.mode("startup-fail"); engine(box, worker, version, WCO_RUNTIME_INCOMPATIBLE);
    box.mode("startup-hang"); engine(box, worker, version, WCO_TIMEOUT);
    private_directories_removed(box);
}
} // namespace

int main(int argc, char** argv)
{
    try {
        require(argc == 3, "usage: runtime_worker_test WORKER FAKE_MMMOJO_LIBRARY");
        const auto worker = std::filesystem::absolute(utf8_path(argv[1]));
        const auto library = std::filesystem::absolute(utf8_path(argv[2]));
        tests(worker, library, 4);
#ifdef _WIN32
        tests(worker, library, 3);
#endif
        std::cout << "SDK -> production worker -> foreign ABI -> PNG/protobuf tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
