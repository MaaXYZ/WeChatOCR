// SPDX-License-Identifier: AGPL-3.0-only
#include "discovery.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace fs = std::filesystem;
void require(bool value, const std::string& reason) { if (!value) throw std::runtime_error(reason); }
std::string utf8(const fs::path& value)
{
    const auto bytes = value.u8string(); return {reinterpret_cast<const char*>(bytes.data()),bytes.size()};
}
fs::path path(std::string_view value)
{
    return fs::path(std::u8string_view(reinterpret_cast<const char8_t*>(value.data()),value.size()));
}
struct Sandbox {
    fs::path root;
    Sandbox()
    {
        root = fs::temp_directory_path() / ("wco-discovery-test-" + std::to_string(std::random_device{}()));
        fs::create_directories(root);
    }
    ~Sandbox() { std::error_code ignored; fs::remove_all(root,ignored); }
};
void file(const fs::path& target, const std::vector<uint8_t>& bytes)
{
    fs::create_directories(target.parent_path());
    std::ofstream out(target,std::ios::binary); out.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));
    require(bool(out),"cannot write discovery fixture");
}
std::vector<uint8_t> elf(uint16_t machine = 62)
{
    std::vector<uint8_t> bytes(64);
    bytes[0]=0x7f; bytes[1]='E'; bytes[2]='L'; bytes[3]='F'; bytes[4]=2; bytes[5]=1; bytes[6]=1;
    bytes[16]=3; bytes[18]=uint8_t(machine); bytes[19]=uint8_t(machine >> 8); return bytes;
}
std::vector<uint8_t> pe(uint16_t machine = 0x8664)
{
    std::vector<uint8_t> bytes(128);
    bytes[0]='M'; bytes[1]='Z'; bytes[0x3c]=64; bytes[64]='P'; bytes[65]='E';
    bytes[68]=uint8_t(machine); bytes[69]=uint8_t(machine >> 8); bytes[84]=2;
    bytes[88]=0x0b; bytes[89]=0x02; return bytes;
}
wco::RuntimeCandidate linux_runtime(const fs::path& root)
{
    file(root/"libmmmojo.so",elf()); file(root/"libwxocr.so",elf()); file(root/"wxocr",elf());
    file(root/"ocr_model"/"test.xnet",{1});
    return {utf8(fs::absolute(root)),utf8(fs::absolute(root/"wxocr")),{},"fixture",4};
}
wco::RuntimeCandidate windows_runtime(const fs::path& runtime, const fs::path& plugin, uint32_t version)
{
    file(runtime/"mmmojo_64.dll",pe());
    const auto entry = plugin/(version == 3 ? "WeChatOCR.exe" : "wxocr.dll");
    file(entry,pe()); file(plugin/"Model"/"test.xnet",{1});
    if (version == 4) file(runtime.parent_path()/"Weixin.exe",pe());
    return {utf8(fs::absolute(runtime)),utf8(fs::absolute(entry)),{},"fixture",version};
}
std::vector<wco::RuntimeCandidate> discover(std::vector<fs::path> roots)
{
    return wco::discover_runtimes({std::move(roots),false});
}
void valid(const wco::RuntimeCandidate& candidate)
{
    std::string error; const bool success = wco::validate_runtime(candidate,error);
    require(success,"valid fixture rejected: " + error); require(error.empty(),"success left a diagnostic");
}
void invalid(const wco::RuntimeCandidate& candidate, const std::string& expected)
{
    std::string error; require(!wco::validate_runtime(candidate,error),"invalid runtime accepted");
    require(error.find(expected) != std::string::npos,"unexpected diagnostic: " + error);
}

bool same_object(const std::string& left, const std::string& right)
{
    std::error_code error;
    return fs::equivalent(path(left),path(right),error) && !error;
}
std::string spelling(const std::string& value)
{
    std::error_code error;
    const auto canonical = fs::weakly_canonical(path(value),error);
    return value + " (canonical: " + (error ? error.message() : utf8(canonical)) + ")";
}
void windows_pairings(const std::vector<wco::RuntimeCandidate>& found,
                      const wco::RuntimeCandidate& expected3, const wco::RuntimeCandidate& expected4)
{
    // On Windows temp_directory_path()/absolute() may retain an 8.3 username
    // (RUNNER~1), whereas discovery weakly_canonical() expands the long name.
    // Compare actual filesystem objects, retaining the exact protocol/runtime
    // pairing requirement instead of comparing their interchangeable spellings.
    bool paired3 = false, paired4 = false;
    for (const auto& candidate : found) {
        if (same_object(candidate.ocr_path,expected3.ocr_path)) {
            require(candidate.protocol == 3 && same_object(candidate.runtime_dir,expected3.runtime_dir),
                    "WeChat 3 paired with an unrelated runtime: " + spelling(candidate.runtime_dir)
                    + "; expected " + spelling(expected3.runtime_dir));
            paired3 = true;
        }
        if (same_object(candidate.ocr_path,expected4.ocr_path)) {
            require(candidate.protocol == 4 && same_object(candidate.runtime_dir,expected4.runtime_dir),
                    "WeChat 4 parent/host relationship was lost: " + spelling(candidate.runtime_dir)
                    + "; expected " + spelling(expected4.runtime_dir));
            paired4 = true;
        }
    }
    if (!paired3 || !paired4) {
        std::ostringstream diagnostic;
        diagnostic << "Windows plugin pairing missing; expected3=" << spelling(expected3.ocr_path)
                   << "; expected4=" << spelling(expected4.ocr_path);
        for (const auto& candidate : found)
            diagnostic << "\nfound protocol=" << candidate.protocol << " entry=" << spelling(candidate.ocr_path)
                       << " runtime=" << spelling(candidate.runtime_dir);
        throw std::runtime_error(diagnostic.str());
    }
}

void fixtures()
{
    Sandbox box;
    const auto base = box.root/path("portable 中文 空格");
    const auto linux9 = linux_runtime(base/"linux"/"4.9.0");
    const auto linux10 = linux_runtime(base/"linux"/"4.10.0-beta");
    valid(linux9); valid(linux10);
    const auto win3 = windows_runtime(base/"WeChat"/"[3.9.8.25]",base/"plugins3"/"7061"/"extracted",3);
    const auto win4 = windows_runtime(base/"Weixin"/"4.10.0.26",base/"plugins4"/"8011"/"extracted",4);
    valid(win3); valid(win4);
    const auto found = discover({base});
    require(found.size() == 4,"portable layouts did not yield exactly four paired candidates");
    require(found[0].protocol == 4 && found.back().protocol == 3,"protocol/version preference");
    for (const auto& candidate : found) {
        valid(candidate);
        require(!candidate.version.empty(),"numeric directory version was lost");
    }
    windows_pairings(found,win3,win4);
    const auto linux_sorted = discover({base/"linux"});
    require(linux_sorted.size() == 2 && linux_sorted[0].version == "4.10.0" && linux_sorted[1].version == "4.9.0",
            "versions were sorted lexically instead of naturally");
    const auto repeated = discover({base,base,base/"linux",base/"linux"/"4.9.0"});
    require(repeated.size() == found.size(),"overlapping search roots duplicated candidates");
    for (size_t i = 0; i < found.size(); ++i)
        require(found[i].runtime_dir == repeated[i].runtime_dir && found[i].ocr_path == repeated[i].ocr_path,
                "candidate order is unstable");

    file(path(linux9.runtime_dir)/"libmmmojo.so",elf(183)); invalid(linux9,"ELF64 x86_64");
    file(path(linux9.runtime_dir)/"libmmmojo.so",elf());
    auto elf32 = elf(); elf32[4]=1; file(path(linux9.ocr_path),elf32); invalid(linux9,"ELF64 x86_64");
    file(path(linux9.ocr_path),elf());
    fs::remove(path(linux9.runtime_dir)/"libwxocr.so"); invalid(linux9,"libwxocr.so");
    file(path(linux9.runtime_dir)/"libwxocr.so",elf());
    fs::remove(path(linux9.runtime_dir)/"ocr_model"/"test.xnet"); invalid(linux9,"ocr_model");
    file(path(linux9.runtime_dir)/"ocr_model"/"test.xnet",{1});
    auto wrong_protocol = linux9; wrong_protocol.protocol=3; invalid(wrong_protocol,"protocol 4");
    auto relative = linux9; relative.runtime_dir="relative"; invalid(relative,"absolute");
    auto nul = linux9; nul.ocr_path += std::string("\0suffix",7); invalid(nul,"NUL");

    file(path(win3.ocr_path),pe(0x14c)); invalid(win3,"PE64 AMD64");
    file(path(win3.ocr_path),pe(0xaa64)); invalid(win3,"PE64 AMD64");
    auto invalid_pe = pe(); invalid_pe[0x3f]=0x7f; file(path(win3.ocr_path),invalid_pe); invalid(win3,"PE header offset");
    file(path(win3.ocr_path),pe());
    fs::remove(path(win4.runtime_dir).parent_path()/"Weixin.exe"); invalid(win4,"runtime.parent/weixin.exe");
    file(path(win4.runtime_dir).parent_path()/"Weixin.exe",pe());
    const auto models = path(win4.ocr_path).parent_path()/"Model";
    fs::rename(models,models.parent_path()/"ocr_model"); valid(win4);

    // Child symlinks must not recurse or cause aliases/loops to multiply results.
    std::error_code symlink_error;
    fs::create_directory_symlink(base,base/"cycle",symlink_error);
    if (!symlink_error) require(discover({base}).size() == 4,"symlink loop was traversed");

    // A supplied root itself may be a normal installation symlink.
    fs::create_directory_symlink(base,box.root/"root-alias",symlink_error);
    if (!symlink_error) {
        require(discover({base,box.root/"root-alias"}).size() == 4,"root aliases were not deduplicated");
        auto alias3 = win3, alias4 = win4;
        alias3.runtime_dir = utf8(box.root/"root-alias"/"WeChat"/"[3.9.8.25]");
        alias3.ocr_path = utf8(box.root/"root-alias"/"plugins3"/"7061"/"extracted"/"WeChatOCR.exe");
        alias4.runtime_dir = utf8(box.root/"root-alias"/"Weixin"/"4.10.0.26");
        alias4.ocr_path = utf8(box.root/"root-alias"/"plugins4"/"8011"/"extracted"/"wxocr.dll");
        const auto aliased = discover({box.root/"root-alias"});
        require(aliased.size() == 4,"alias-root discovery changed candidate count");
        // Regression for distinct input/canonical spellings on every platform
        // with symlink support, even when the host does not use short names.
        windows_pairings(aliased,alias3,alias4);
    }

    Sandbox deep;
    auto level = deep.root;
    for (unsigned i = 0; i < 6; ++i) level /= "nested";
    linux_runtime(level); linux_runtime(level/"too-deep");
    require(discover({deep.root}).size() == 1,"recursion did not stop at depth six");
    require(discover({deep.root/"missing"}).empty(),"missing root produced candidates");
    require(wco::discover_runtimes({{},false}).empty(),"disabled system scan still inspected the host");

    Sandbox pairs;
    wco::RuntimeCandidate last;
    for (unsigned i = 1; i <= 12; ++i)
        last = windows_runtime(pairs.root/"runtimes"/("3.9."+std::to_string(i)),pairs.root/"plugin"/"7061",3);
    const auto bounded = discover({pairs.root});
    require(bounded.size() == 8 && same_object(bounded.front().runtime_dir,last.runtime_dir),
            "plugin pairing bound/natural preference failed");
    for (const auto& candidate : bounded)
        require(candidate.source.find("heuristic-pair") != std::string::npos,"heuristic pairing was not labelled");
}
} // namespace

int main(int argc, char** argv)
{
    try {
        fixtures();
        if (argc > 1) {
            const auto actual = discover({path(argv[1])});
            require(!actual.empty(),"no runtime discovered at the supplied real-runtime path");
            for (const auto& candidate : actual) valid(candidate);
            std::cout << "Real-runtime discovery passed: " << actual.front().ocr_path << '\n';
        }
        std::cout << "bounded runtime discovery fixtures passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
