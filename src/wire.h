#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace wco::wire {
constexpr uint32_t magic = 0x314f4357;
constexpr uint32_t version = 1;
constexpr uint32_t max_payload = 128u * 1024u * 1024u;
constexpr size_t header_size = 24;
enum Kind : uint32_t { hello = 1, recognize = 2, reply = 3 };

class Writer {
public:
    std::vector<uint8_t> data;
    void u32(uint32_t v) { for (unsigned i = 0; i < 4; ++i) data.push_back(static_cast<uint8_t>(v >> (8 * i))); }
    void u64(uint64_t v) { for (unsigned i = 0; i < 8; ++i) data.push_back(static_cast<uint8_t>(v >> (8 * i))); }
    void f32(float v) { u32(std::bit_cast<uint32_t>(v)); }
    void bytes(std::span<const uint8_t> v) { data.insert(data.end(), v.begin(), v.end()); }
    void string(const std::string& s) {
        if (s.size() > max_payload) throw std::length_error("wire string exceeds limit");
        u32(static_cast<uint32_t>(s.size()));
        bytes({reinterpret_cast<const uint8_t*>(s.data()), s.size()});
    }
};

class Reader {
    std::span<const uint8_t> data_;
    size_t offset_ = 0;
public:
    explicit Reader(std::span<const uint8_t> data) : data_(data) {}
    std::span<const uint8_t> bytes(size_t n) {
        if (n > data_.size() - offset_) throw std::runtime_error("truncated IPC message");
        auto result = data_.subspan(offset_, n); offset_ += n; return result;
    }
    uint32_t u32() { auto s = bytes(4); uint32_t v = 0; for (unsigned i = 0; i < 4; ++i) v |= uint32_t(s[i]) << (8 * i); return v; }
    uint64_t u64() { auto s = bytes(8); uint64_t v = 0; for (unsigned i = 0; i < 8; ++i) v |= uint64_t(s[i]) << (8 * i); return v; }
    float f32() { return std::bit_cast<float>(u32()); }
    std::string string() { auto s = bytes(u32()); return {reinterpret_cast<const char*>(s.data()), s.size()}; }
    bool done() const { return offset_ == data_.size(); }
};

struct Header { uint32_t kind; uint32_t size; uint64_t request; };
inline std::vector<uint8_t> header(uint32_t kind, uint32_t size, uint64_t request) {
    Writer w; w.u32(magic); w.u32(version); w.u32(kind); w.u32(size); w.u64(request); return std::move(w.data);
}
inline Header parse_header(std::span<const uint8_t> data) {
    Reader r(data);
    if (r.u32() != magic || r.u32() != version) throw std::runtime_error("incompatible worker protocol");
    Header h{r.u32(), r.u32(), r.u64()};
    if (h.size > max_payload) throw std::runtime_error("IPC payload exceeds limit");
    return h;
}
} // namespace wco::wire
