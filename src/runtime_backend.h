// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace wco {

struct RuntimeConfig {
    std::string runtime_dir;
    std::string ocr_path;
    // 0: infer from the platform/entry point; 3: WeChat 3; 4: WeChat 4.
    uint32_t protocol = 0;
};

struct RuntimeLine {
    // Top left, top right, bottom right, bottom left, in image pixels.
    float quad[8] {};
    float score = 0;
    bool has_score = false;
    std::string text;
};

struct RuntimeResult {
    int32_t vendor_error = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<RuntimeLine> lines;
};

// Private worker implementation. Do not load the vendor runtime in the SDK
// process: its background threads cannot safely be unloaded. The owner must
// enforce a process deadline, including around start(), recognize() and teardown.
class RuntimeBackend {
public:
    RuntimeBackend();
    ~RuntimeBackend();
    RuntimeBackend(const RuntimeBackend&) = delete;
    RuntimeBackend& operator=(const RuntimeBackend&) = delete;

    bool start(const RuntimeConfig& config, std::string& error);
    // Calls are serialized. A vendor error is a successfully received response:
    // the caller must examine result.vendor_error separately from this bool.
    bool recognize(const std::string& absolute_image_path,
                   RuntimeResult& result, std::string& error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace wco
