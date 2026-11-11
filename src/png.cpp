#include "png.h"
#include "WeChatOCR/WeChatOCR.h"
#include <algorithm>
#include <stdexcept>

namespace wco {
namespace {
void be32(std::vector<uint8_t>& out, uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<uint8_t>(v >> shift));
}
uint32_t crc32(std::span<const uint8_t> bytes) {
    uint32_t crc = 0xffffffffu;
    for (uint8_t byte : bytes) {
        crc ^= byte;
        for (unsigned i = 0; i < 8; ++i) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}
void chunk(std::vector<uint8_t>& out, const char* kind, std::span<const uint8_t> bytes) {
    be32(out, static_cast<uint32_t>(bytes.size()));
    const size_t start = out.size();
    out.insert(out.end(), kind, kind + 4); out.insert(out.end(), bytes.begin(), bytes.end());
    be32(out, crc32(std::span<const uint8_t>(out).subspan(start)));
}
}
std::vector<uint8_t> encode_png(uint32_t width, uint32_t height, uint32_t format,
                                std::span<const uint8_t> pixels) {
    const uint32_t channels = format == WCO_PIXEL_GRAY ? 1 : format == WCO_PIXEL_BGRA ? 4 :
        (format == WCO_PIXEL_BGR || format == WCO_PIXEL_RGB) ? 3 : 0;
    if (!channels || !width || !height || width > 32768 || height > 32768 ||
        uint64_t(width) * height * channels != pixels.size() || pixels.size() > 64u * 1024u * 1024u)
        throw std::runtime_error("invalid PNG input");
    const uint32_t output_channels = channels == 1 ? 1 : 3;
    std::vector<uint8_t> raw;
    raw.reserve((size_t(width) * output_channels + 1) * height);
    for (uint32_t y = 0; y < height; ++y) {
        raw.push_back(0);
        for (uint32_t x = 0; x < width; ++x) {
            const auto offset = (size_t(y) * width + x) * channels;
            if (channels == 1) raw.push_back(pixels[offset]);
            else if (format == WCO_PIXEL_RGB) raw.insert(raw.end(), pixels.begin() + offset, pixels.begin() + offset + 3);
            else { raw.push_back(pixels[offset + 2]); raw.push_back(pixels[offset + 1]); raw.push_back(pixels[offset]); }
        }
    }
    std::vector<uint8_t> compressed{0x78, 0x01};
    compressed.reserve(raw.size() + raw.size() / 65535 * 5 + 16);
    uint32_t a = 1, b = 0;
    for (size_t offset = 0; offset < raw.size();) {
        const auto n = static_cast<uint16_t>(std::min<size_t>(65535, raw.size() - offset));
        compressed.push_back(offset + n == raw.size() ? 1 : 0);
        compressed.push_back(static_cast<uint8_t>(n)); compressed.push_back(static_cast<uint8_t>(n >> 8));
        const uint16_t inverse = static_cast<uint16_t>(~n);
        compressed.push_back(static_cast<uint8_t>(inverse)); compressed.push_back(static_cast<uint8_t>(inverse >> 8));
        compressed.insert(compressed.end(), raw.begin() + offset, raw.begin() + offset + n);
        for (size_t i = offset; i < offset + n; ++i) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
        offset += n;
    }
    be32(compressed, (b << 16) | a);
    std::vector<uint8_t> output{137, 80, 78, 71, 13, 10, 26, 10};
    std::vector<uint8_t> ihdr;
    be32(ihdr, width); be32(ihdr, height);
    ihdr.insert(ihdr.end(), {8, static_cast<uint8_t>(channels == 1 ? 0 : 2), 0, 0, 0});
    chunk(output, "IHDR", ihdr); chunk(output, "IDAT", compressed); chunk(output, "IEND", {});
    return output;
}
}
