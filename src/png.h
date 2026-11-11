#pragma once
#include <cstdint>
#include <span>
#include <vector>

namespace wco {
// Dependency-free lossless PNG; stored DEFLATE blocks trade size for predictable
// encoding cost. Input rows are packed. BGRA input alpha is ignored.
std::vector<uint8_t> encode_png(uint32_t width, uint32_t height, uint32_t format,
                                std::span<const uint8_t> pixels);
}
