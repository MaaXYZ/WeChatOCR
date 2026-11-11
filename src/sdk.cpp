#include "WeChatOCR/WeChatOCR.h"
#include "process.h"
#include "wire.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {
using Clock = std::chrono::steady_clock;
thread_local std::string last_error;
WcoStatus fail(WcoStatus status, std::string message) { last_error = std::move(message); return status; }
std::filesystem::path path(const std::string& s) { return std::filesystem::u8path(s); }
std::string utf8(const std::filesystem::path& p) {
    const auto s = p.u8string(); return {reinterpret_cast<const char*>(s.data()), s.size()};
}
std::string private_temp(const std::filesystem::path& base) {
    std::random_device random;
    for (unsigned i = 0; i < 64; ++i) {
        auto candidate = base / ("wechatocr-" + std::to_string(random()) + "-" + std::to_string(random()));
        if (!std::filesystem::create_directory(candidate)) continue;
        std::error_code error;
        std::filesystem::permissions(candidate, std::filesystem::perms::owner_all, error);
        if (error) { std::filesystem::remove(candidate); throw std::runtime_error("cannot secure temporary directory"); }
        return utf8(candidate);
    }
    throw std::runtime_error("cannot allocate temporary directory");
}
std::filesystem::path default_worker() {
    std::filesystem::path module;
#ifdef _WIN32
    HMODULE handle = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&wco_version), &handle)) return {};
    std::vector<wchar_t> buffer(32768);
    const auto n = GetModuleFileNameW(handle, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!n || n == buffer.size()) return {};
    module = std::filesystem::path(std::wstring(buffer.data(), n)).parent_path();
    return module / "wechatocr-worker.exe";
#else
    Dl_info info{};
    if (!dladdr(reinterpret_cast<const void*>(&wco_version), &info) || !info.dli_fname) return {};
    module = std::filesystem::absolute(info.dli_fname).parent_path();
    auto nearby = module / "wechatocr-worker";
    if (std::filesystem::is_regular_file(nearby)) return nearby;
    return module.parent_path() / "bin" / "wechatocr-worker";
#endif
}
bool valid_utf8(const std::string& text) {
    size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<uint8_t>(text[i++]);
        if (lead < 128) { if (!lead) return false; continue; }
        unsigned count; uint32_t code;
        if (lead >= 0xc2 && lead <= 0xdf) { count = 1; code = lead & 31; }
        else if (lead >= 0xe0 && lead <= 0xef) { count = 2; code = lead & 15; }
        else if (lead >= 0xf0 && lead <= 0xf4) { count = 3; code = lead & 7; }
        else return false;
        if (count > text.size() - i) return false;
        for (unsigned j = 0; j < count; ++j) {
            const auto c = static_cast<uint8_t>(text[i++]);
            if ((c & 0xc0) != 0x80) return false;
            code = (code << 6) | (c & 63);
        }
        if ((count == 1 && code < 128) || (count == 2 && code < 2048) ||
            (count == 3 && code < 65536) || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
    }
    return true;
}
}

struct ResultLine { float quad[8]; float confidence; uint32_t has_confidence; std::string text; };
struct WcoResult { uint32_t width = 0, height = 0; int32_t vendor_error = 0; std::vector<ResultLine> lines; };
struct WcoEngine {
    std::string runtime_dir, ocr_path, temp_dir;
    std::filesystem::path worker;
    uint32_t protocol = 0, startup_timeout = 10000, max_pending = 16;
    std::atomic<uint32_t> pending{0};
    std::timed_mutex mutex;
    wco::Process process;
    bool ready = false;
    uint64_t next_request = 1;
    ~WcoEngine() {
        process.stop();
        if (!temp_dir.empty()) { std::error_code error; std::filesystem::remove_all(path(temp_dir), error); }
    }

    WcoStatus exchange(uint32_t kind, const std::vector<uint8_t>& payload, Clock::time_point deadline,
                       std::vector<uint8_t>& reply) {
        const uint64_t request = next_request++;
        auto header = wco::wire::header(kind, static_cast<uint32_t>(payload.size()), request);
        std::string error;
        const auto io_error = [&]() {
            ready = false; process.stop();
            return fail(Clock::now() >= deadline ? WCO_TIMEOUT : WCO_WORKER_ERROR, std::move(error));
        };
        if (!process.write_all(header.data(), header.size(), deadline, error) ||
            !process.write_all(payload.data(), payload.size(), deadline, error) ||
            !process.read_all(header.data(), header.size(), deadline, error)) return io_error();
        try {
            const auto h = wco::wire::parse_header(header);
            if (h.kind != wco::wire::reply || h.request != request) throw std::runtime_error("worker response identity mismatch");
            reply.resize(h.size);
            if (!process.read_all(reply.data(), reply.size(), deadline, error)) return io_error();
        } catch (const std::exception& e) {
            ready = false; process.stop(); return fail(WCO_WORKER_ERROR, e.what());
        }
        return WCO_OK;
    }
    WcoStatus start(Clock::time_point deadline) {
        process.stop(); ready = false;
        for (const auto& file : std::filesystem::directory_iterator(path(temp_dir))) std::filesystem::remove_all(file.path());
        std::string error;
        if (!process.start(worker, {}, error)) return fail(WCO_WORKER_ERROR, std::move(error));
        wco::wire::Writer config;
        config.string(runtime_dir); config.string(ocr_path); config.string(temp_dir); config.u32(protocol);
        std::vector<uint8_t> reply;
        const auto status = exchange(wco::wire::hello, config.data, deadline, reply);
        if (status != WCO_OK) return status;
        try {
            wco::wire::Reader r(reply);
            auto result = static_cast<WcoStatus>(r.u32()); auto message = r.string();
            if (!r.done() || result < WCO_OK || result > WCO_INTERNAL_ERROR) throw std::runtime_error("invalid startup response");
            if (result != WCO_OK) { process.stop(); return fail(result, std::move(message)); }
            ready = true; return WCO_OK;
        } catch (const std::exception& e) {
            process.stop(); return fail(WCO_WORKER_ERROR, e.what());
        }
    }
};

extern "C" {
const char* WCO_CALL wco_version(void) { return WCO_VERSION; }
const char* WCO_CALL wco_last_error(void) { return last_error.c_str(); }
const char* WCO_CALL wco_status_string(WcoStatus status) {
    static constexpr const char* messages[] = {"OK", "Invalid argument", "ABI mismatch", "Runtime missing",
        "Runtime incompatible", "Worker error", "Timeout", "Busy", "OCR failed", "I/O error", "Out of memory", "Internal error"};
    return status >= 0 && status <= WCO_INTERNAL_ERROR ? messages[status] : "Unknown status";
}
WcoStatus WCO_CALL wco_create(const WcoOptions* options, WcoEngine** engine) {
    last_error.clear();
    if (!engine) return fail(WCO_INVALID_ARGUMENT, "engine output is NULL");
    *engine = nullptr;
    if (!options || options->struct_size < sizeof(WcoOptions)) return fail(WCO_INVALID_ARGUMENT, "invalid options struct_size");
    if (options->abi_version != WCO_ABI_VERSION) return fail(WCO_ABI_MISMATCH, "unsupported ABI version");
    if (!options->runtime_dir || !options->ocr_path || options->protocol > 4 || options->protocol == 1 || options->protocol == 2)
        return fail(WCO_INVALID_ARGUMENT, "runtime_dir, ocr_path and protocol are required");
    try {
        auto e = std::make_unique<WcoEngine>();
        e->runtime_dir = options->runtime_dir; e->ocr_path = options->ocr_path;
        if (!valid_utf8(e->runtime_dir) || !valid_utf8(e->ocr_path)) return fail(WCO_INVALID_ARGUMENT, "runtime paths must be UTF-8");
        if (!path(e->runtime_dir).is_absolute() || !path(e->ocr_path).is_absolute())
            return fail(WCO_INVALID_ARGUMENT, "runtime_dir and ocr_path must be absolute paths");
        if (!std::filesystem::is_directory(path(e->runtime_dir)) || !std::filesystem::is_regular_file(path(e->ocr_path)))
            return fail(WCO_RUNTIME_MISSING, "runtime directory or OCR entry file does not exist");
        e->worker = options->worker_path && *options->worker_path ? path(options->worker_path) : default_worker();
        if (!e->worker.is_absolute() || !std::filesystem::is_regular_file(e->worker))
            return fail(WCO_WORKER_ERROR, "worker path must identify an existing absolute executable");
        auto temp_base = std::filesystem::temp_directory_path();
        if (options->temp_dir && *options->temp_dir) {
            const std::string requested = options->temp_dir;
            if (!valid_utf8(requested) || !path(requested).is_absolute() || !std::filesystem::is_directory(path(requested)))
                return fail(WCO_INVALID_ARGUMENT, "temp_dir must be an existing absolute directory");
            temp_base = path(requested);
        }
        e->temp_dir = private_temp(temp_base);
        e->protocol = options->protocol;
        e->startup_timeout = options->startup_timeout_ms ? options->startup_timeout_ms : 10000;
        e->max_pending = options->max_pending_requests ? options->max_pending_requests : 16;
        const auto result = e->start(Clock::now() + std::chrono::milliseconds(e->startup_timeout));
        if (result != WCO_OK) return result;
        *engine = e.release(); return WCO_OK;
    } catch (const std::bad_alloc&) { return fail(WCO_OUT_OF_MEMORY, "allocation failed"); }
      catch (const std::filesystem::filesystem_error& e) { return fail(WCO_IO_ERROR, e.what()); }
      catch (const std::exception& e) { return fail(WCO_INTERNAL_ERROR, e.what()); }
      catch (...) { return fail(WCO_INTERNAL_ERROR, "unexpected exception"); }
}
WcoStatus WCO_CALL wco_recognize(WcoEngine* engine, const WcoImageView* image, uint32_t timeout_ms, WcoResult** result) {
    last_error.clear();
    if (!result) return fail(WCO_INVALID_ARGUMENT, "result output is NULL");
    *result = nullptr;
    if (!engine || !image || image->struct_size < sizeof(WcoImageView) || !timeout_ms || !image->data ||
        !image->width || !image->height || image->width > 32768 || image->height > 32768)
        return fail(WCO_INVALID_ARGUMENT, "invalid engine, image or timeout");
    const uint32_t channels = image->format == WCO_PIXEL_GRAY ? 1 : image->format == WCO_PIXEL_BGRA ? 4 :
        (image->format == WCO_PIXEL_BGR || image->format == WCO_PIXEL_RGB) ? 3 : 0;
    const uint64_t row = uint64_t(image->width) * channels;
    if (!channels || image->stride < row || row * image->height > 64u * 1024u * 1024u ||
        image->stride > std::numeric_limits<uint64_t>::max() / image->height ||
        image->stride * (image->height - 1) + row > image->data_size)
        return fail(WCO_INVALID_ARGUMENT, "invalid pixel format, stride or image size");
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    if (engine->pending.fetch_add(1, std::memory_order_relaxed) >= engine->max_pending) {
        engine->pending.fetch_sub(1, std::memory_order_relaxed); return fail(WCO_BUSY, "request queue is full");
    }
    struct Pending { std::atomic<uint32_t>& n; ~Pending() { n.fetch_sub(1, std::memory_order_relaxed); } } pending{engine->pending};
    std::unique_lock<std::timed_mutex> lock(engine->mutex, std::defer_lock);
    if (!lock.try_lock_until(deadline)) return fail(WCO_TIMEOUT, "deadline expired while queueing");
    try {
        if (!engine->ready) {
            const auto status = engine->start(std::min(deadline, Clock::now() + std::chrono::milliseconds(engine->startup_timeout)));
            if (status != WCO_OK) return status;
        }
        wco::wire::Writer w;
        w.u32(image->width); w.u32(image->height); w.u32(static_cast<uint32_t>(row)); w.u32(image->format);
        w.data.reserve(16 + static_cast<size_t>(row * image->height));
        for (uint32_t y = 0; y < image->height; ++y)
            w.bytes({image->data + static_cast<size_t>(image->stride * y), static_cast<size_t>(row)});
        if (Clock::now() >= deadline) return fail(WCO_TIMEOUT, "deadline expired while copying image");
        std::vector<uint8_t> reply;
        auto status = engine->exchange(wco::wire::recognize, w.data, deadline, reply);
        if (status != WCO_OK) return status;
        wco::wire::Reader r(reply);
        status = static_cast<WcoStatus>(r.u32()); const auto error = r.string();
        if (status < WCO_OK || status > WCO_INTERNAL_ERROR) throw std::runtime_error("invalid recognition status");
        if (status != WCO_OK) return fail(status, error);
        auto output = std::make_unique<WcoResult>();
        output->vendor_error = static_cast<int32_t>(r.u32()); output->width = r.u32(); output->height = r.u32();
        const uint32_t count = r.u32();
        if (count > 100000 || output->width != image->width || output->height != image->height)
            throw std::runtime_error("invalid OCR dimensions or line count");
        output->lines.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            ResultLine line{};
            for (float& v : line.quad) { v = r.f32(); if (!std::isfinite(v) || std::abs(v) > 1e7f) throw std::runtime_error("invalid OCR coordinate"); }
            line.confidence = r.f32(); line.has_confidence = r.u32(); line.text = r.string();
            if (line.has_confidence > 1 || !std::isfinite(line.confidence) || !valid_utf8(line.text))
                throw std::runtime_error("invalid OCR text or confidence");
            output->lines.push_back(std::move(line));
        }
        if (!r.done()) throw std::runtime_error("unexpected trailing OCR data");
        if (output->vendor_error) return fail(WCO_OCR_FAILED, "vendor OCR error " + std::to_string(output->vendor_error));
        if (Clock::now() >= deadline) return fail(WCO_TIMEOUT, "deadline expired while decoding result");
        *result = output.release(); return WCO_OK;
    } catch (const std::bad_alloc&) { return fail(WCO_OUT_OF_MEMORY, "allocation failed"); }
      catch (const std::exception& e) { engine->ready = false; engine->process.stop(); return fail(WCO_WORKER_ERROR, e.what()); }
      catch (...) { engine->ready = false; engine->process.stop(); return fail(WCO_INTERNAL_ERROR, "unexpected exception"); }
}
WcoStatus WCO_CALL wco_get_capabilities(const WcoEngine* engine, WcoCapabilities* caps) {
    last_error.clear();
    if (!engine || !caps || caps->struct_size < sizeof(WcoCapabilities)) return fail(WCO_INVALID_ARGUMENT, "invalid capabilities output");
    *caps = {sizeof(WcoCapabilities), WCO_ABI_VERSION, 1, 0, 0, 0}; return WCO_OK;
}
uint32_t WCO_CALL wco_result_line_count(const WcoResult* result) {
    return result ? static_cast<uint32_t>(result->lines.size()) : 0;
}
WcoStatus WCO_CALL wco_result_get_info(const WcoResult* result, WcoResultInfo* info) {
    last_error.clear();
    if (!result || !info || info->struct_size < sizeof(WcoResultInfo)) return fail(WCO_INVALID_ARGUMENT, "invalid result info output");
    *info = {sizeof(WcoResultInfo), result->width, result->height, result->vendor_error}; return WCO_OK;
}
WcoStatus WCO_CALL wco_result_get_line(const WcoResult* result, uint32_t index, WcoLine* line) {
    last_error.clear();
    if (!result || !line || line->struct_size < sizeof(WcoLine) || index >= result->lines.size())
        return fail(WCO_INVALID_ARGUMENT, "invalid result line or index");
    const auto& source = result->lines[index];
    line->has_confidence = source.has_confidence; line->confidence = source.confidence;
    std::copy(std::begin(source.quad), std::end(source.quad), line->quad);
    line->text = source.text.c_str(); line->text_length = source.text.size(); return WCO_OK;
}
void WCO_CALL wco_result_destroy(WcoResult* result) { delete result; }
void WCO_CALL wco_destroy(WcoEngine* engine) { delete engine; }
}
