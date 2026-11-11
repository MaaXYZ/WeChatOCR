// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace wco {

struct RuntimeCandidate {
    std::string runtime_dir;
    std::string ocr_path;
    // Inferred directory versions, when available: runtime[+plugin]. This is
    // discovery metadata, not a verified product/file version or compatibility.
    std::string version;
    std::string source;
    uint32_t protocol = 0;
};

struct DiscoveryOptions {
    std::vector<std::filesystem::path> search_roots;
    bool scan_system = true;
};

// Bounded filesystem/registry inspection only: never loads or executes binaries.
// Custom roots are searched to depth six; system roots are explicit known paths.
std::vector<RuntimeCandidate> discover_runtimes(const DiscoveryOptions& options);
bool validate_runtime(const RuntimeCandidate& candidate, std::string& error);

} // namespace wco
