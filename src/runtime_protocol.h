// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "runtime_backend.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wco::protocol {

constexpr size_t max_message_bytes = 64 * 1024 * 1024;

struct Message {
    enum class Kind { ignored, ready, result } kind = Kind::ignored;
    bool supported = false;
    uint64_t task_id = 0;
    RuntimeResult result;
};

// Minimal protobuf wire implementation for the independently described WeChat
// OCR message protocol. This does not link protobuf or reuse generated sources.
std::vector<uint8_t> make_request(uint32_t version, uint64_t task_id,
                                  const std::string& image_path);
bool decode(uint32_t version, uint32_t request_id, const void* data, size_t size,
            Message& message, std::string& error);

} // namespace wco::protocol
