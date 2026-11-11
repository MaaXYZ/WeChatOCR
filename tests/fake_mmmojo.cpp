// SPDX-License-Identifier: AGPL-3.0-only
// Test-only foreign-ABI simulator. This is never linked to a production target.
#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#define FAKE_EXPORT extern "C" __declspec(dllexport)
using NativeChar = wchar_t;
#else
#define FAKE_EXPORT extern "C" __attribute__((visibility("default")))
using NativeChar = char;
#endif

namespace {
using Bytes = std::vector<uint8_t>;
using ReadCallback = void (*)(uint32_t, const void*, void*);
using ConnectCallback = void (*)(bool, void*);
using SimpleCallback = void (*)(void*);
using LaunchFailure = void (*)(int, void*);
using ErrorCallback = void (*)(const void*, int, void*);
struct Environment {
    void* user = nullptr;
    ReadCallback push = nullptr, pull = nullptr, shared = nullptr;
    ConnectCallback connect = nullptr;
    SimpleCallback disconnect = nullptr, launched = nullptr;
    LaunchFailure launch_failure = nullptr;
    ErrorCallback error = nullptr;
    bool host = false;
    std::filesystem::path executable, runtime;
    std::string mode;
    uint32_t version = 4;
};
struct WriteInfo { int method; bool sync; uint32_t request; Bytes bytes; };
struct ReadInfo { Bytes bytes; };
std::filesystem::path utf8_path(std::string_view value)
{
    return std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(value.data()), value.size()));
}

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
void variable(Bytes& out, uint64_t value)
{
    while (value >= 128) { out.push_back(uint8_t(value) | 128); value >>= 7; }
    out.push_back(uint8_t(value));
}
void scalar(Bytes& out, uint32_t tag, uint64_t value) { variable(out, uint64_t(tag) * 8); variable(out, value); }
void bytes(Bytes& out, uint32_t tag, const Bytes& value)
{
    variable(out, uint64_t(tag) * 8 + 2); variable(out, value.size());
    out.insert(out.end(), value.begin(), value.end());
}
void text(Bytes& out, uint32_t tag, const std::string& value) { bytes(out, tag, Bytes(value.begin(), value.end())); }
void floating(Bytes& out, uint32_t tag, float value)
{
    uint32_t bits = 0; std::memcpy(&bits, &value, 4);
    variable(out, uint64_t(tag) * 8 + 5);
    for (unsigned i = 0; i != 4; ++i) out.push_back(uint8_t(bits >> (8 * i)));
}

// Independent test parser: only request varints/submessages are needed.
uint64_t number(const Bytes& b, size_t& p)
{
    uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
        require(p < b.size(), "truncated request integer");
        const auto v = b[p++]; value |= uint64_t(v & 127) << shift;
        if (!(v & 128)) return value;
    }
    throw std::runtime_error("oversized request integer");
}
struct Request { uint64_t task = 0; std::string path; bool options = false; };
Request request(const Bytes& b, bool nested = false)
{
    Request out;
    for (size_t p = 0; p != b.size();) {
        const auto key = number(b, p), tag = key >> 3, wire = key & 7;
        if (wire == 0) {
            const auto value = number(b, p);
            if (tag == 1 && !nested) out.task = value;
            if (tag == 2) out.task = value;
        } else {
            require(wire == 2, "unexpected request wire type");
            const auto count = number(b, p);
            require(count <= b.size() - p, "invalid request message length");
            Bytes part(b.begin() + p, b.begin() + p + size_t(count)); p += size_t(count);
            if (tag == 2 || (tag == 1 && nested)) out.path.assign(part.begin(), part.end());
            if (tag == 3 && !nested) out.path = request(part, true).path;
            if (tag == 6) out.options = part == Bytes({8,1,16,1,24,0});
        }
    }
    return out;
}

uint32_t big(const Bytes& b, size_t p)
{
    require(p + 4 <= b.size(), "truncated PNG integer");
    return uint32_t(b[p]) << 24 | uint32_t(b[p+1]) << 16 | uint32_t(b[p+2]) << 8 | b[p+3];
}
uint32_t crc(const uint8_t* data, size_t size)
{
    uint32_t value = ~uint32_t(0);
    for (size_t i = 0; i < size; ++i) {
        value ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            value = (value >> 1) ^ ((value & 1) ? 0xedb88320u : 0u);
    }
    return ~value;
}
struct Picture { uint32_t width = 0, height = 0; std::string pixels; };
Picture png(const std::string& path)
{
    require(utf8_path(path).is_absolute(), "OCR path is not absolute");
    std::ifstream file(utf8_path(path), std::ios::binary);
    const Bytes data((std::istreambuf_iterator<char>(file)), {});
    const Bytes signature{137,80,78,71,13,10,26,10};
    require(data.size() > 8 && std::equal(signature.begin(), signature.end(), data.begin()), "invalid PNG signature");
    Picture out; Bytes compressed; unsigned channels = 0; bool ended = false;
    for (size_t p = 8; p < data.size();) {
        require(data.size() - p >= 12, "truncated PNG chunk");
        const uint32_t count = big(data, p);
        require(count <= data.size() - p - 12, "invalid PNG chunk length");
        const std::string name(data.begin() + p + 4, data.begin() + p + 8);
        require(crc(data.data() + p + 4, size_t(count) + 4) == big(data, p + 8 + count), "invalid PNG CRC");
        if (name == "IHDR") {
            require(count == 13 && data[p+16] == 8 && (data[p+17] == 0 || data[p+17] == 2), "unsupported PNG header");
            out.width = big(data, p+8); out.height = big(data, p+12); channels = data[p+17] == 0 ? 1 : 3;
        } else if (name == "IDAT") compressed.insert(compressed.end(), data.begin()+p+8, data.begin()+p+8+count);
        else if (name == "IEND") ended = true;
        p += size_t(count) + 12;
    }
    require(ended && channels && compressed.size() >= 6, "incomplete PNG");
    require((compressed[0] & 15) == 8 && (compressed[0] * 256u + compressed[1]) % 31 == 0,
            "invalid zlib header");
    Bytes raster;
    size_t p = 2;
    bool last = false;
    while (!last) {
        require(p + 5 <= compressed.size() - 4, "truncated stored deflate block");
        const uint8_t flags = compressed[p++]; last = flags & 1;
        require((flags & 6) == 0, "test fixture only handles stored deflate blocks");
        const uint16_t size = uint16_t(compressed[p]) | uint16_t(compressed[p+1]) << 8;
        const uint16_t inverse = uint16_t(compressed[p+2]) | uint16_t(compressed[p+3]) << 8; p += 4;
        require(uint16_t(size ^ inverse) == 65535 && size <= compressed.size() - 4 - p, "invalid deflate block length");
        raster.insert(raster.end(), compressed.begin()+p, compressed.begin()+p+size); p += size;
    }
    require(p + 4 == compressed.size(), "unexpected zlib trailing data");
    uint32_t a = 1, b = 0;
    for (auto v : raster) { a = (a + v) % 65521; b = (b + a) % 65521; }
    require((b << 16 | a) == big(compressed, p), "invalid PNG Adler checksum");
    const size_t row = size_t(out.width) * channels;
    require(raster.size() == (row + 1) * out.height, "invalid PNG dimensions/raster size");
    constexpr char digits[] = "0123456789abcdef";
    for (uint32_t y = 0; y < out.height; ++y) {
        require(raster[y*(row+1)] == 0, "unexpected PNG row filter");
        for (size_t x = 0; x < row; ++x) {
            const uint8_t value = raster[y*(row+1)+x+1];
            out.pixels += digits[value >> 4]; out.pixels += digits[value & 15];
        }
    }
    return out;
}

void push(Environment* env, uint32_t id, Bytes payload)
{
    require(env->push, "missing push callback");
    env->push(id, new ReadInfo{std::move(payload)}, env->user);
}
void fail(Environment* env, const char* detail)
{
    if (env->error) env->error(detail, int(std::strlen(detail)), env->user);
}

Bytes response(Environment* env, uint64_t task, const Picture& pic)
{
    Bytes result;
    if (env->mode != "empty") {
        Bytes line, quad;
        const float corners[8]{0,0,2,0,1.5f,2,0,1.5f};
        for (unsigned i = 0; i < 4; ++i) {
            Bytes point; floating(point, 1, corners[i*2]); floating(point, 2, corners[i*2+1]);
            bytes(quad, i+1, point);
        }
        bytes(line, 1, quad); text(line, 2, "像素:" + pic.pixels);
        if (env->mode != "no-score") floating(line, 3, .875f);
        bytes(result, env->version == 3 ? 1 : 3, line);
    }
    scalar(result, env->version == 3 ? 2 : 4, pic.width);
    scalar(result, env->version == 3 ? 3 : 5, pic.height);
    Bytes out;
    scalar(out, env->version == 3 ? 2 : 1, task);
    if (env->mode == "vendor-error") scalar(out, env->version == 3 ? 3 : 2, 17);
    bytes(out, env->version == 3 ? 4 : 3, result);
    return out;
}
} // namespace

FAKE_EXPORT void InitializeMMMojo(int, const char* const*) {}
FAKE_EXPORT void* CreateMMMojoEnvironment() { return new Environment; }
FAKE_EXPORT void SetMMMojoEnvironmentCallbacks(void* handle, int type, ...)
{
    auto* env = static_cast<Environment*>(handle); va_list args; va_start(args, type);
    switch (type) {
    case 0: env->user = va_arg(args, void*); break;
    case 1: env->push = va_arg(args, ReadCallback); break;
    case 2: env->pull = va_arg(args, ReadCallback); break;
    case 3: env->shared = va_arg(args, ReadCallback); break;
    case 4: env->connect = va_arg(args, ConnectCallback); break;
    case 5: env->disconnect = va_arg(args, SimpleCallback); break;
    case 6: env->launched = va_arg(args, SimpleCallback); break;
    case 7: env->launch_failure = va_arg(args, LaunchFailure); break;
    case 8: env->error = va_arg(args, ErrorCallback); break;
    }
    va_end(args);
}
FAKE_EXPORT void SetMMMojoEnvironmentInitParams(void* handle, int type, ...)
{
    auto* env = static_cast<Environment*>(handle); va_list args; va_start(args, type);
    if (type == 0) env->host = va_arg(args, int) != 0;
    if (type == 2) env->executable = va_arg(args, const NativeChar*);
    va_end(args);
}
FAKE_EXPORT void AppendMMSubProcessSwitchNative(void* handle, const char* key, const NativeChar* value)
{
    if (std::string(key) == "user-lib-dir") static_cast<Environment*>(handle)->runtime = value;
}
FAKE_EXPORT void StartMMMojoEnvironment(void* handle)
{
    auto* env = static_cast<Environment*>(handle);
    try {
        require(env->host && !env->executable.empty(), "startup parameters missing");
        if (env->runtime.empty()) env->runtime = env->executable.parent_path();
        std::ifstream(env->runtime / "fake-mode") >> env->mode;
        env->version = env->executable.filename().string().find("WeChatOCR") != std::string::npos ? 3 : 4;
        if (env->mode == "startup-hang") return;
        if (env->mode == "startup-fail") { env->launch_failure(42, env->user); return; }
        std::puts("simulated foreign runtime stdout noise");
        std::fflush(stdout); // Must really reach the worker's redirected stream.
        env->connect(true, env->user);
        Bytes ready;
        if (env->version == 3) { scalar(ready, 1, 1); scalar(ready, 2, 1); }
        else scalar(ready, 1, env->mode == "unsupported" ? 0 : 1);
        push(env, env->version == 3 ? 1 : 10001, std::move(ready));
    } catch (const std::exception& error) { fail(env, error.what()); }
}
FAKE_EXPORT void StopMMMojoEnvironment(void*) {}
FAKE_EXPORT void RemoveMMMojoEnvironment(void* handle) { delete static_cast<Environment*>(handle); }
FAKE_EXPORT const void* GetMMMojoReadInfoRequest(const void* info, uint32_t* size)
{
    const auto& bytes = static_cast<const ReadInfo*>(info)->bytes;
    *size = uint32_t(bytes.size()); return bytes.data();
}
FAKE_EXPORT void RemoveMMMojoReadInfo(const void* info) { delete static_cast<const ReadInfo*>(info); }
FAKE_EXPORT void* CreateMMMojoWriteInfo(int method, bool sync, uint32_t id) { return new WriteInfo{method,sync,id,{}}; }
FAKE_EXPORT void* GetMMMojoWriteInfoRequest(void* info, size_t size)
{
    auto& out = static_cast<WriteInfo*>(info)->bytes; out.resize(size); return out.data();
}
FAKE_EXPORT void RemoveMMMojoWriteInfo(void* info) { delete static_cast<WriteInfo*>(info); }
FAKE_EXPORT bool SendMMMojoWriteInfo(void* handle, void* raw)
{
    std::unique_ptr<WriteInfo> info(static_cast<WriteInfo*>(raw)); // Consumed on both outcomes.
    auto* env = static_cast<Environment*>(handle);
    try {
        require(info->method == 1 && !info->sync, "invalid OCR message mode");
        require(info->request == (env->version == 3 ? 1u : 10010u), "invalid OCR request ID");
        const auto input = request(info->bytes);
        require(input.task >= 2 && input.task <= 0x7fffffff, "invalid OCR task ID");
        require(env->version == 3 || input.options, "missing explicit optional OCR flags");
        const auto picture = png(input.path);
        std::ofstream(env->runtime / "last-image-path") << input.path;
        if (env->mode == "hang") return true;
        if (env->mode == "send-fail") return false;
        if (env->mode == "disconnect") { env->disconnect(env->user); return true; }
        const uint32_t id = env->version == 3 ? 1 : 10011;
        if (env->mode == "malformed") push(env, id, Bytes{0xff});
        else {
            // A delayed/wrong task must not overwrite the active task result.
            Picture wrong_task = picture; wrong_task.pixels = "wrong-task";
            push(env, id, response(env, input.task + 100, wrong_task));
            push(env, id, response(env, input.task, picture));
        }
        return true;
    } catch (const std::exception& error) { fail(env, error.what()); return false; }
}
