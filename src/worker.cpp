#include "WeChatOCR/WeChatOCR.h"
#include "png.h"
#include "worker_process.h"
#include "runtime_backend.h"
#include "wire.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {
struct FileCloser { void operator()(FILE* file) const { if (file) std::fclose(file); } };
using File = std::unique_ptr<FILE, FileCloser>;
File private_output() {
#ifdef _WIN32
    if (_setmode(_fileno(stdin), _O_BINARY) < 0 || _setmode(_fileno(stdout), _O_BINARY) < 0) return {};
    const int fd = _dup(_fileno(stdout));
    if (fd < 0) return {};
    if (!SetHandleInformation(reinterpret_cast<HANDLE>(_get_osfhandle(fd)), HANDLE_FLAG_INHERIT, 0)) {
        _close(fd); return {};
    }
    FILE* file = _fdopen(fd, "wb");
    if (!file) { _close(fd); return {}; }
    File result(file);
    if (_dup2(_fileno(stderr), _fileno(stdout)) < 0
        || !SetStdHandle(STD_OUTPUT_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stdout))))) return {};
#else
    const int fd = dup(STDOUT_FILENO);
    if (fd < 0) return {};
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) { close(fd); return {}; }
    FILE* file = fdopen(fd, "wb");
    if (!file) { close(fd); return {}; }
    File result(file);
    if (dup2(STDERR_FILENO, STDOUT_FILENO) < 0) return {};
#endif
    return result;
}
bool send(FILE* output, uint64_t request, const wco::wire::Writer& payload) {
    if (payload.data.size() > wco::wire::max_payload) return false;
    auto header = wco::wire::header(wco::wire::reply, static_cast<uint32_t>(payload.data.size()), request);
    return std::fwrite(header.data(), 1, header.size(), output) == header.size() &&
        std::fwrite(payload.data.data(), 1, payload.data.size(), output) == payload.data.size() && std::fflush(output) == 0;
}
void status(wco::wire::Writer& writer, WcoStatus code, const std::string& error) {
    writer.u32(static_cast<uint32_t>(code)); writer.string(error);
}
struct ImageFile {
    std::filesystem::path path;
    ~ImageFile() { if (!path.empty()) { std::error_code error; std::filesystem::remove(path, error); } }
};
std::string utf8(const std::filesystem::path& path) {
    auto s = path.u8string(); return {reinterpret_cast<const char*>(s.data()), s.size()};
}
}

int main() {
    std::string error;
    if (!wco::configure_worker_parent_death(error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 2; }
    auto output = private_output();
    if (!output) return 2;
    std::unique_ptr<wco::RuntimeBackend> backend;
    std::filesystem::path temp;
    for (;;) {
        std::vector<uint8_t> raw_header(wco::wire::header_size);
        const auto n = std::fread(raw_header.data(), 1, raw_header.size(), stdin);
        if (!n && std::feof(stdin)) return 0;
        if (n != raw_header.size()) return 3;
        uint64_t request = 0;
        wco::wire::Writer response;
        try {
            const auto header = wco::wire::parse_header(raw_header); request = header.request;
            std::vector<uint8_t> payload(header.size);
            if (std::fread(payload.data(), 1, payload.size(), stdin) != payload.size()) return 3;
            wco::wire::Reader reader(payload);
            if (header.kind == wco::wire::hello && !backend) {
                wco::RuntimeConfig config;
                config.runtime_dir = reader.string(); config.ocr_path = reader.string();
                temp = std::filesystem::u8path(reader.string()); config.protocol = reader.u32();
                if (!reader.done() || !temp.is_absolute() || !std::filesystem::is_directory(temp))
                    throw std::runtime_error("invalid worker startup configuration");
                backend = std::make_unique<wco::RuntimeBackend>();
                if (backend->start(config, error)) status(response, WCO_OK, "");
                else status(response, WCO_RUNTIME_INCOMPATIBLE, error);
            } else if (header.kind == wco::wire::recognize && backend) {
                const uint32_t width = reader.u32(), height = reader.u32(), stride = reader.u32(), format = reader.u32();
                const uint32_t channels = format == WCO_PIXEL_GRAY ? 1 : format == WCO_PIXEL_BGRA ? 4 :
                    (format == WCO_PIXEL_BGR || format == WCO_PIXEL_RGB) ? 3 : 0;
                if (!channels || !width || !height || width > 32768 || height > 32768 ||
                    uint64_t(width) * channels != stride || uint64_t(stride) * height > 64u * 1024u * 1024u)
                    throw std::runtime_error("invalid worker image dimensions");
                const auto pixels = reader.bytes(size_t(stride) * height);
                if (!reader.done()) throw std::runtime_error("trailing image data");
                const auto png = wco::encode_png(width, height, format, pixels);
                ImageFile image{temp / ("image-" + std::to_string(request) + ".png")};
                {
                    std::ofstream file(image.path, std::ios::binary | std::ios::trunc);
                    file.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
                    file.close();
                    if (!file) throw std::runtime_error("cannot write private temporary PNG");
                }
                wco::RuntimeResult result;
                if (!backend->recognize(utf8(image.path), result, error)) status(response, WCO_WORKER_ERROR, error);
                else if (result.vendor_error) status(response, WCO_OCR_FAILED, "vendor OCR error " + std::to_string(result.vendor_error));
                else if (result.width != width || result.height != height) status(response, WCO_OCR_FAILED, "vendor returned unexpected image dimensions");
                else {
                    status(response, WCO_OK, "");
                    response.u32(static_cast<uint32_t>(result.vendor_error)); response.u32(width); response.u32(height);
                    response.u32(static_cast<uint32_t>(result.lines.size()));
                    for (const auto& line : result.lines) {
                        for (float v : line.quad) response.f32(v);
                        response.f32(line.score); response.u32(line.has_score ? 1 : 0); response.string(line.text);
                    }
                }
            } else throw std::runtime_error("unexpected worker request");
        } catch (const std::exception& e) {
            response.data.clear(); status(response, WCO_WORKER_ERROR, e.what());
        } catch (...) {
            response.data.clear(); status(response, WCO_INTERNAL_ERROR, "unexpected worker exception");
        }
        if (!send(output.get(), request, response)) return 3;
    }
}
