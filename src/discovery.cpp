// SPDX-License-Identifier: AGPL-3.0-only
#include "discovery.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <set>
#include <string_view>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace wco {
namespace {
namespace fs = std::filesystem;
constexpr size_t max_roots = 64;
constexpr size_t max_directories = 4096;
constexpr size_t max_directory_entries = 2048;
constexpr size_t max_pairings_per_plugin = 8;
constexpr size_t max_candidates = 256;
constexpr size_t max_windows_runtimes = 128;
constexpr size_t max_plugins = 64;

fs::path from_utf8(std::string_view value)
{
#ifdef __cpp_char8_t
    return fs::path(std::u8string_view(reinterpret_cast<const char8_t*>(value.data()), value.size()));
#else
    return fs::u8path(value);
#endif
}
std::string utf8(const fs::path& value)
{
    const auto bytes = value.u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
std::string lower(std::string value)
{
    for (auto& byte : value)
        if (byte >= 'A' && byte <= 'Z') byte += 'a' - 'A';
    return value;
}
fs::path normalized(const fs::path& value)
{
    if (value.empty()) return {};
    std::error_code error;
    auto path = fs::absolute(value, error);
    if (error) return {};
    path = fs::weakly_canonical(path, error);
    return error ? fs::path{} : path;
}
std::string identity(const fs::path& value)
{
    const auto path = normalized(value);
    if (path.empty()) return {};
    auto text = utf8(path);
#ifdef _WIN32
    // Existing Windows paths resolve to a canonical spelling; ASCII case folding
    // also handles aliases supplied with a different drive/path letter case.
    text = lower(std::move(text));
#endif
    return text;
}
bool regular(const fs::path& value)
{
    std::error_code error; return fs::is_regular_file(value, error) && !error;
}
bool directory(const fs::path& value)
{
    std::error_code error; return fs::is_directory(value, error) && !error;
}

// Natural comparison without integer conversion/overflow: 4.10 sorts after 4.9.
int natural(std::string_view left, std::string_view right)
{
    size_t a = 0, b = 0;
    while (a < left.size() && b < right.size()) {
        const bool ad = left[a] >= '0' && left[a] <= '9';
        const bool bd = right[b] >= '0' && right[b] <= '9';
        if (ad && bd) {
            size_t ae = a, be = b;
            while (ae < left.size() && left[ae] >= '0' && left[ae] <= '9') ++ae;
            while (be < right.size() && right[be] >= '0' && right[be] <= '9') ++be;
            size_t az = a, bz = b;
            while (az < ae && left[az] == '0') ++az;
            while (bz < be && right[bz] == '0') ++bz;
            if (ae - az != be - bz) return ae - az < be - bz ? -1 : 1;
            const int order = left.substr(az, ae - az).compare(right.substr(bz, be - bz));
            if (order) return order < 0 ? -1 : 1;
            a = ae; b = be;
        } else {
            const auto ac = static_cast<unsigned char>(left[a]);
            const auto bc = static_cast<unsigned char>(right[b]);
            if (ac != bc) return ac < bc ? -1 : 1;
            ++a; ++b;
        }
    }
    return a == left.size() ? (b == right.size() ? 0 : -1) : 1;
}

std::string version_in_path(fs::path value)
{
    for (unsigned depth = 0; !value.empty() && depth < 5; ++depth, value = value.parent_path()) {
        auto name = utf8(value.filename());
        if (name.size() >= 2 && name.front() == '[' && name.back() == ']') name = name.substr(1, name.size()-2);
        if (name.empty() || name.front() < '0' || name.front() > '9') continue;
        size_t end = 0;
        while (end < name.size() && ((name[end] >= '0' && name[end] <= '9') || name[end] == '.' || name[end] == '_')) ++end;
        while (end && (name[end-1] == '.' || name[end-1] == '_')) --end;
        if (end && end <= 128) return name.substr(0,end);
    }
    return {};
}

std::vector<fs::path> children(const fs::path& value)
{
    std::vector<fs::path> paths;
    std::error_code error;
    fs::directory_iterator iterator(value, fs::directory_options::skip_permission_denied, error), end;
    for (size_t count = 0; !error && iterator != end && count < max_directory_entries;
         iterator.increment(error), ++count)
        paths.push_back(iterator->path());
    std::sort(paths.begin(), paths.end(), [](const auto& a, const auto& b) {
        const auto left = utf8(a.filename()), right = utf8(b.filename());
        const auto order = natural(left, right);
        return order ? order > 0 : left < right;
    });
    return paths;
}

fs::path named(const fs::path& root, const char* name)
{
    const auto direct = root / name;
    std::error_code error;
    if (fs::exists(direct, error) && !error) return direct;
    // This also allows Windows-layout fixtures on a case-sensitive host.
    const auto wanted = lower(name);
    for (const auto& child : children(root))
        if (lower(utf8(child.filename())) == wanted) return child;
    return direct;
}

bool has_xnet(const fs::path& path)
{
    if (!directory(path)) return false;
    for (const auto& file : children(path)) {
        std::error_code error;
        if (regular(file) && lower(utf8(file.filename())).find(".xnet") != std::string::npos
            && fs::file_size(file,error) > 0 && !error) return true;
    }
    return false;
}
bool windows_models(const fs::path& plugin)
{
    if (has_xnet(plugin)) return true;
    for (const char* name : {"Model", "models", "ocr_model"})
        if (has_xnet(named(plugin, name))) return true;
    return false;
}

uint16_t le16(const uint8_t* bytes) { return uint16_t(bytes[0]) | uint16_t(bytes[1]) << 8; }
uint32_t le32(const uint8_t* bytes)
{
    return uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 | uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 24;
}
bool binary(const fs::path& path, bool pe, std::string& error)
{
    if (!regular(path)) { error = "Missing runtime binary: " + utf8(path); return false; }
    std::ifstream file(path, std::ios::binary);
    std::array<uint8_t,64> header{};
    file.read(reinterpret_cast<char*>(header.data()), std::streamsize(header.size()));
    const auto available = file.gcount();
    if (pe) {
        if (available != 64 || header[0] != 'M' || header[1] != 'Z') {
            error = "Expected a PE64 AMD64 binary: " + utf8(path); return false;
        }
        const auto offset = le32(header.data()+0x3c);
        if (offset < 64 || offset > 16 * 1024 * 1024) {
            error = "Invalid PE header offset: " + utf8(path); return false;
        }
        file.clear(); file.seekg(offset);
        std::array<uint8_t,26> coff{};
        file.read(reinterpret_cast<char*>(coff.data()), std::streamsize(coff.size()));
        if (file.gcount() != 26 || coff[0] != 'P' || coff[1] != 'E' || coff[2] || coff[3]
            || le16(coff.data()+4) != 0x8664 || le16(coff.data()+20) < 2 || le16(coff.data()+24) != 0x20b) {
            error = "Expected a PE64 AMD64 binary: " + utf8(path); return false;
        }
    } else {
        if (available < 20 || header[0] != 0x7f || header[1] != 'E' || header[2] != 'L' || header[3] != 'F'
            || header[4] != 2 || header[5] != 1 || header[6] != 1 || le16(header.data()+18) != 62
            || (le16(header.data()+16) != 2 && le16(header.data()+16) != 3)) {
            error = "Expected a little-endian ELF64 x86_64 executable/shared library: " + utf8(path); return false;
        }
    }
    return true;
}

struct Root { fs::path path; std::string source; size_t group = 0; };
struct Found { fs::path path; std::string source, version; size_t group = 0; };
struct Plugin : Found { uint32_t protocol = 0; };

#ifdef _WIN32
fs::path environment(const wchar_t* key)
{
    const DWORD count = GetEnvironmentVariableW(key, nullptr, 0);
    if (!count || count > 32768) return {};
    std::wstring value(count, L'\0');
    const auto copied = GetEnvironmentVariableW(key, value.data(), count);
    if (!copied || copied >= count) return {};
    value.resize(copied); return value;
}
struct RegistryKey {
    HKEY value = nullptr;
    ~RegistryKey() { if (value) RegCloseKey(value); }
};
std::wstring registry_string(HKEY key, const wchar_t* name)
{
    DWORD type = 0, bytes = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS
        || (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < 2 || bytes > 65536 || bytes % 2) return {};
    std::wstring value(bytes / sizeof(wchar_t) + 1, L'\0');
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(value.data()), &bytes) != ERROR_SUCCESS) return {};
    value.resize(value.find(L'\0'));
    if (type == REG_EXPAND_SZ) {
        const auto count = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
        if (!count || count > 32768) return {};
        std::wstring expanded(count, L'\0');
        if (ExpandEnvironmentStringsW(value.c_str(), expanded.data(), count) != count) return {};
        expanded.resize(count - 1); value = std::move(expanded);
    }
    if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"') value = value.substr(1, value.size()-2);
    return value;
}

void registry_roots(std::vector<Root>& roots)
{
    const wchar_t* uninstall = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
    for (HKEY hive : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        for (REGSAM view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
            auto add = [&](const std::wstring& path) {
                if (path.empty() || roots.size() >= max_roots) return;
                fs::path root(path); if (regular(root)) root = root.parent_path();
                roots.push_back({root, "registry", 0});
            };
            for (const wchar_t* name : {L"Software\\Tencent\\WeChat", L"Software\\Tencent\\Weixin"}) {
                RegistryKey key;
                if (RegOpenKeyExW(hive, name, 0, KEY_READ | view, &key.value) != ERROR_SUCCESS) continue;
                for (const wchar_t* value : {L"InstallPath", L"InstallLocation", L"Path"}) add(registry_string(key.value, value));
            }
            RegistryKey list;
            if (RegOpenKeyExW(hive, uninstall, 0, KEY_READ | view, &list.value) != ERROR_SUCCESS) continue;
            for (DWORD i = 0; i < 1024 && roots.size() < max_roots; ++i) {
                std::array<wchar_t,1024> name{}; DWORD count = DWORD(name.size());
                const auto code = RegEnumKeyExW(list.value, i, name.data(), &count, nullptr, nullptr, nullptr, nullptr);
                if (code == ERROR_NO_MORE_ITEMS) break;
                if (code != ERROR_SUCCESS) continue;
                RegistryKey entry;
                if (RegOpenKeyExW(list.value, name.data(), 0, KEY_READ | view, &entry.value) != ERROR_SUCCESS) continue;
                auto display = registry_string(entry.value, L"DisplayName");
                std::wstring key_name(name.data());
                for (auto* value : {&display,&key_name})
                    for (auto& character : *value)
                        if (character >= L'A' && character <= L'Z') character += L'a' - L'A';
                if (display.find(L"wechat") == std::wstring::npos && display.find(L"weixin") == std::wstring::npos
                    && display.find(L"微信") == std::wstring::npos && key_name.find(L"wechat") == std::wstring::npos
                    && key_name.find(L"weixin") == std::wstring::npos) continue;
                add(registry_string(entry.value, L"InstallLocation"));
            }
        }
    }
}
#endif

std::vector<Root> roots_for(const DiscoveryOptions& options)
{
    std::vector<Root> roots;
    size_t group = 0;
    for (const auto& path : options.search_roots) {
        if (roots.size() >= max_roots) break;
        roots.push_back({path, "custom", ++group});
    }
    if (options.scan_system) {
#ifdef _WIN32
        registry_roots(roots);
        for (const wchar_t* variable : {L"ProgramFiles", L"ProgramFiles(x86)", L"ProgramW6432"}) {
            const auto root = environment(variable);
            if (!root.empty()) for (const char* app : {"Weixin", "WeChat"}) roots.push_back({root / "Tencent" / app, "program-files", 0});
        }
        for (const wchar_t* variable : {L"APPDATA", L"LOCALAPPDATA"}) {
            const auto root = environment(variable);
            if (root.empty()) continue;
            roots.push_back({root / "Tencent" / "WeChat" / "XPlugin" / "Plugins" / "WeChatOCR", "profile-plugin", 0});
            roots.push_back({root / "Tencent" / "xwechat" / "XPlugin" / "plugins" / "WeChatOcr", "profile-plugin", 0});
            roots.push_back({root / "Tencent" / "Weixin", "profile-install", 0});
            roots.push_back({root / "Programs" / "Tencent" / "Weixin", "profile-install", 0});
            roots.push_back({root / "Programs" / "Tencent" / "WeChat", "profile-install", 0});
        }
#else
        for (const char* path : {"/opt/wechat", "/usr/lib/wechat", "/usr/local/lib/wechat"})
            roots.push_back({path, "system", 0});
        if (const char* home = std::getenv("HOME")) {
            const auto root = from_utf8(home);
            for (const char* path : {".local/opt/wechat", ".local/lib/wechat", ".local/share/wechat"})
                roots.push_back({root / path, "user-install", 0});
        }
#endif
    }
    std::vector<Root> unique;
    std::set<std::string> seen;
    for (auto& root : roots) {
        if (unique.size() >= max_roots) break;
        try {
            root.path = normalized(root.path);
            if (!directory(root.path)) continue;
            const auto key = identity(root.path);
            if (!key.empty() && seen.insert(key).second) unique.push_back(std::move(root));
        } catch (const fs::filesystem_error&) {
            // One inaccessible/invalid root must not suppress the other roots.
        }
    }
    return unique;
}

std::string combined_version(const Found& runtime, const Found& plugin)
{
    if (runtime.version.empty()) return plugin.version;
    return plugin.version.empty() || runtime.version == plugin.version ? runtime.version : runtime.version + "+" + plugin.version;
}

bool candidate_less(const RuntimeCandidate& a, const RuntimeCandidate& b)
{
    if (a.protocol != b.protocol) return a.protocol > b.protocol;
    const int order = natural(a.version, b.version);
    if (order) return order > 0;
    if (a.runtime_dir != b.runtime_dir) return a.runtime_dir < b.runtime_dir;
    return a.ocr_path < b.ocr_path;
}
} // namespace

bool validate_runtime(const RuntimeCandidate& candidate, std::string& error)
{
    error.clear();
    try {
        if (candidate.runtime_dir.empty() || candidate.ocr_path.empty()
            || candidate.runtime_dir.find('\0') != std::string::npos || candidate.ocr_path.find('\0') != std::string::npos) {
            error = "Runtime and OCR paths must be nonempty paths without NUL characters"; return false;
        }
        const auto runtime = from_utf8(candidate.runtime_dir), entry = from_utf8(candidate.ocr_path);
        if (!runtime.is_absolute() || !entry.is_absolute() || !directory(runtime)) {
            error = "Runtime and OCR paths must be absolute; runtime directory must exist"; return false;
        }
        const auto name = lower(utf8(entry.filename()));
        if (name == "wxocr") {
            if (candidate.protocol != 4) { error = "Linux wxocr requires protocol 4"; return false; }
            if (normalized(entry.parent_path()) != normalized(runtime)) {
                error = "Linux wxocr must belong to its runtime directory"; return false;
            }
            if (!binary(entry, false, error) || !binary(runtime / "libmmmojo.so", false, error)
                || !binary(runtime / "libwxocr.so", false, error)) return false;
            if (!has_xnet(runtime / "ocr_model")) {
                error = "Missing Linux ocr_model directory containing an .xnet model: " + utf8(runtime); return false;
            }
        } else {
            if (candidate.protocol != 3 && candidate.protocol != 4) { error = "Windows OCR protocol must be 3 or 4"; return false; }
            if ((candidate.protocol == 3 && name != "wechatocr.exe") || (candidate.protocol == 4 && name != "wxocr.dll")) {
                error = "OCR entry filename does not match the requested protocol"; return false;
            }
            if (!binary(entry, true, error) || !binary(named(runtime, "mmmojo_64.dll"), true, error)) return false;
            if (candidate.protocol == 4 && !binary(named(runtime.parent_path(), "weixin.exe"), true, error)) {
                error = "WeChat 4 requires runtime.parent/weixin.exe (PE64 AMD64): " + error; return false;
            }
            if (!windows_models(entry.parent_path())) {
                error = "Missing OCR plugin .xnet model: " + utf8(entry.parent_path()); return false;
            }
        }
        return true;
    } catch (const std::exception& exception) {
        error = std::string("Cannot inspect runtime: ") + exception.what(); return false;
    }
}

std::vector<RuntimeCandidate> discover_runtimes(const DiscoveryOptions& options)
{
    std::vector<RuntimeCandidate> candidates;
    std::vector<Found> windows_runtimes;
    std::vector<Plugin> plugins;
    std::set<std::string> visited;
    std::set<std::string> seen_pairs;
    auto add = [&](RuntimeCandidate item) {
        if (candidates.size() >= max_candidates) return;
        std::string error;
        if (!validate_runtime(item, error)) return;
        item.runtime_dir = utf8(normalized(from_utf8(item.runtime_dir)));
        item.ocr_path = utf8(normalized(from_utf8(item.ocr_path)));
        const auto key = identity(from_utf8(item.runtime_dir)) + '\n' + identity(from_utf8(item.ocr_path))
            + '\n' + std::to_string(item.protocol);
        if (seen_pairs.insert(key).second) candidates.push_back(std::move(item));
    };
    try {
        for (const auto& root : roots_for(options)) {
            struct Pending { fs::path path; unsigned depth; };
            std::vector<Pending> queue{{root.path,0}};
            for (size_t index = 0; index < queue.size() && visited.size() < max_directories; ++index) {
                const auto current = queue[index];
                const auto key = identity(current.path);
                if (key.empty() || !visited.insert(key).second) continue;
                const auto contents = children(current.path);
                const auto child_named = [&](const char* name) {
                    const auto wanted = lower(name);
                    for (const auto& child : contents)
                        if (lower(utf8(child.filename())) == wanted) return child;
                    return current.path / name;
                };
                const auto version = version_in_path(current.path);
                if (regular(current.path / "wxocr") && regular(current.path / "libmmmojo.so"))
                    add({utf8(current.path), utf8(current.path / "wxocr"), version, root.source + ":direct", 4});
                if (windows_runtimes.size() < max_windows_runtimes && regular(child_named("mmmojo_64.dll")))
                    windows_runtimes.push_back({current.path,root.source,version,root.group});
                for (const auto& kind : {std::pair<const char*,uint32_t>{"WeChatOCR.exe",3}, {"wxocr.dll",4}}) {
                    const auto entry = child_named(kind.first);
                    if (plugins.size() < max_plugins && regular(entry) && windows_models(current.path)) {
                        Plugin item; item.path = entry; item.source = root.source;
                        item.version = version; item.group = root.group; item.protocol = kind.second;
                        plugins.push_back(std::move(item));
                    }
                }
                if (current.depth >= 6) continue;
                for (const auto& child : contents) {
                    std::error_code error;
                    const auto status = fs::symlink_status(child, error);
                    if (error || !fs::is_directory(status) || fs::is_symlink(status)) continue;
                    if (queue.size() >= max_directories) break;
                    queue.push_back({child,current.depth+1});
                }
            }
        }
        std::sort(windows_runtimes.begin(), windows_runtimes.end(), [](const Found& a, const Found& b) {
            const auto order = natural(a.version,b.version); return order ? order > 0 : utf8(a.path) < utf8(b.path);
        });
        std::sort(plugins.begin(), plugins.end(), [](const Plugin& a, const Plugin& b) {
            const auto order = natural(a.version,b.version); return order ? order > 0 : utf8(a.path) < utf8(b.path);
        });
        for (const auto& plugin : plugins) {
            if (candidates.size() >= max_candidates) break;
            auto ranked = windows_runtimes;
            std::stable_sort(ranked.begin(), ranked.end(), [&](const Found& a, const Found& b) {
                const auto score = [&](const Found& value) {
                    return value.path == plugin.path.parent_path() ? 0 : value.group == plugin.group ? 1 : 2;
                };
                return score(a) < score(b);
            });
            size_t paired = 0;
            for (const auto& runtime : ranked) {
                const bool host = regular(named(runtime.path.parent_path(), "weixin.exe"));
                if ((plugin.protocol == 4 && !host)
                    || (plugin.protocol == 3 && host && runtime.path != plugin.path.parent_path())) continue;
                const auto before = candidates.size();
                add({utf8(runtime.path),utf8(plugin.path),combined_version(runtime,plugin),
                     plugin.source + "+" + runtime.source + ":heuristic-pair", plugin.protocol});
                if (candidates.size() != before && ++paired >= max_pairings_per_plugin) break;
            }
        }
    } catch (const fs::filesystem_error&) {
        // A root can disappear/change permissions while discovery is running.
        // Preserve valid candidates already found; no scan may load binaries.
    }
    std::sort(candidates.begin(), candidates.end(), candidate_less);
    return candidates;
}
} // namespace wco
