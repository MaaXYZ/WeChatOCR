#include "WeChatOCR/WeChatOCR.h"
#include "wire.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <thread>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {
bool read_bytes(uint8_t* data, size_t size) {
    std::cin.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(size));
    return static_cast<size_t>(std::cin.gcount()) == size;
}

void write_reply(uint64_t request, const wco::wire::Writer& payload) {
    const auto header = wco::wire::header(wco::wire::reply, static_cast<uint32_t>(payload.data.size()), request);
    std::cout.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
    std::cout.write(reinterpret_cast<const char*>(payload.data.data()), static_cast<std::streamsize>(payload.data.size()));
    std::cout.flush();
}

void mark(const std::filesystem::path& file) {
    std::ofstream stream(file, std::ios::binary);
    stream << "entered";
}

std::string describe_pixels(std::span<const uint8_t> bytes) {
    std::ostringstream text;
    text << "\xE5\xBC\x80\xE5\xA7\x8B\xE2\x9C\x93:";
    for (const auto byte : bytes) text << std::hex << std::setfill('0') << std::setw(2) << unsigned(byte);
    return text.str();
}
} // namespace

int main() {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::string mode;
    std::filesystem::path ocr_path;
    bool requires_restart = false;
    try {
        while (true) {
            std::array<uint8_t, wco::wire::header_size> header_bytes{};
            if (!read_bytes(header_bytes.data(), header_bytes.size())) return 0;
            const auto header = wco::wire::parse_header(header_bytes);
            std::vector<uint8_t> payload(header.size);
            if (!read_bytes(payload.data(), payload.size())) return 2;
            wco::wire::Reader reader(payload);
            wco::wire::Writer response;
            if (header.kind == wco::wire::hello) {
                const auto runtime_dir = reader.string();
                ocr_path = std::filesystem::u8path(reader.string());
                const auto temp_dir = reader.string();
                const auto protocol = reader.u32();
                if (!reader.done() || runtime_dir.empty() || protocol > 4) return 3;
                (void)temp_dir;
                mode = ocr_path.filename().string();
                if (mode == "startup_hang") {
                    std::this_thread::sleep_for(std::chrono::seconds(30));
                    return 0;
                }
                response.u32(mode == "startup_error" ? WCO_RUNTIME_INCOMPATIBLE : WCO_OK);
                response.string(mode == "startup_error" ? "incompatible controlled runtime" : "");
                write_reply(header.request, response);
                continue;
            }
            if (header.kind != wco::wire::recognize) return 4;
            const auto width = reader.u32();
            const auto height = reader.u32();
            const auto stride = reader.u32();
            const auto format = reader.u32();
            const auto pixels = reader.bytes(static_cast<size_t>(stride) * height);
            const uint32_t channels = format == WCO_PIXEL_BGRA ? 4 : format == WCO_PIXEL_GRAY ? 1 : 3;
            if (!reader.done() || stride != width * channels) return 5;
            auto entered = ocr_path;
            entered += ".entered";
            mark(entered);
            if (mode == "hang") {
                std::this_thread::sleep_for(std::chrono::seconds(30));
                return 0;
            }
            if (mode == "recover") {
                auto once = ocr_path;
                once += ".once";
                if (!std::filesystem::exists(once)) {
                    mark(once);
                    std::this_thread::sleep_for(std::chrono::seconds(30));
                    return 0;
                }
            }
            if (mode == "slow") std::this_thread::sleep_for(std::chrono::milliseconds(300));
            if (mode == "crash") return 23;
            if (mode == "malformed") {
                std::array<char, wco::wire::header_size> bad{};
                std::cout.write(bad.data(), static_cast<std::streamsize>(bad.size()));
                std::cout.flush();
                continue;
            }
            if (mode == "oversize" || mode == "truncated") {
                const auto size = mode == "oversize" ? wco::wire::max_payload + 1 : 100u;
                const auto bad = wco::wire::header(wco::wire::reply, size, header.request);
                std::cout.write(reinterpret_cast<const char*>(bad.data()), static_cast<std::streamsize>(bad.size()));
                std::cout.flush();
                return 0;
            }
            if (mode == "error_trailing" || mode == "bad_error_utf8") {
                response.u32(WCO_OCR_FAILED);
                response.string(mode == "bad_error_utf8" ? std::string("\xC0\xAF", 2) : "controlled OCR failure");
                if (mode == "error_trailing") response.u32(123);
                write_reply(header.request, response);
                continue;
            }
            if (mode == "worker_error_recover") {
                auto once = ocr_path;
                once += ".once";
                if (requires_restart || !std::filesystem::exists(once)) {
                    mark(once);
                    requires_restart = true;
                    response.u32(WCO_WORKER_ERROR);
                    response.string("runtime transport requires a new worker");
                    write_reply(header.request, response);
                    continue;
                }
            }
            response.u32(WCO_OK);
            response.string("");
            response.u32(mode == "vendor_error" ? 7 : 0);
            response.u32(mode == "bad_dimensions" ? width + 1 : width);
            response.u32(height);
            response.u32(mode == "empty" ? 0 : 1);
            if (mode != "empty") {
                const float quad[] = {0, 0, float(width), 0, float(width), float(height), 0, float(height)};
                for (const float value : quad) response.f32(value);
                response.f32(mode == "nan_score" ? std::numeric_limits<float>::quiet_NaN() : 0.875f);
                response.u32(1);
                response.string(mode == "bad_utf8" ? std::string("\xC0\xAF", 2) : describe_pixels(pixels));
            }
            write_reply(header.request + (mode == "wrong_request" ? 1 : 0), response);
        }
    } catch (const std::exception& error) {
        std::cerr << "fake worker: " << error.what() << '\n';
        return 10;
    }
}
