#include "process.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <csignal>
#include <unistd.h>
#endif

namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

std::filesystem::path executable_path(const char* arg0)
{
#ifdef _WIN32
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    require(length && length < buffer.size(), "cannot find test executable");
    buffer.resize(length);
    return buffer;
#else
    return std::filesystem::absolute(arg0);
#endif
}

int child(int argc, char** argv)
{
    std::string error;
    if (!wco::configure_worker_parent_death(error)) {
        std::cerr << error << '\n'; return 2;
    }
    const std::string mode = argc > 2 ? argv[2] : "";
    if (mode == "echo" || mode == "ready_echo") {
        if (mode == "ready_echo") {
            std::cout.put('!');
            std::cout.flush();
        }
        std::array<char, 128> bytes{};
        std::cin.read(bytes.data(), bytes.size());
        if (!std::cin) return 3;
        std::cout.write(bytes.data(), bytes.size());
        std::cout.flush();
        return std::cout ? 0 : 4;
    }
    if (mode == "args") {
        for (int i = 3; i < argc; ++i) {
            const std::string arg = argv[i];
            const uint32_t count = static_cast<uint32_t>(arg.size());
            std::cout.write(reinterpret_cast<const char*>(&count), sizeof(count));
            std::cout.write(arg.data(), count);
        }
        std::cout.flush();
        return 0;
    }
    if (mode == "flood") {
        std::vector<char> bytes(2 * 1024 * 1024);
        for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<char>(i % 251);
        std::cout.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        std::cout.flush();
        return 0;
    }
    if (mode == "exit") return 0;
#ifdef __linux__
    if (mode == "tree") {
        const pid_t descendant = fork();
        if (descendant < 0) return 5;
        if (!descendant) {
            for (;;) pause();
        }
        const uint64_t pids[]{static_cast<uint64_t>(getpid()), static_cast<uint64_t>(descendant)};
        std::cout.write(reinterpret_cast<const char*>(pids), sizeof(pids));
        std::cout.flush();
        for (;;) pause();
    }
#endif
    if (mode == "idle") {
        std::this_thread::sleep_for(30s);
        return 0;
    }
    return 6;
}

#ifdef __linux__
bool terminated(uint64_t pid)
{
    std::ifstream status("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    std::getline(status, line);
    if (!status && line.empty()) return true;
    const auto end = line.rfind(')');
    return end != std::string::npos && end + 2 < line.size() && line[end + 2] == 'Z';
}

void require_terminated(uint64_t pid)
{
    const auto deadline = Clock::now() + 1500ms;
    while (!terminated(pid) && Clock::now() < deadline) std::this_thread::sleep_for(5ms);
    require(terminated(pid), "worker descendant survived cleanup: " + std::to_string(pid));
}

int exit_without_cleanup(const std::filesystem::path& executable)
{
    wco::Process process;
    std::string error;
    uint64_t pids[2]{};
    if (!process.start(executable, {"--child", "tree"}, error)
        || !process.read_all(reinterpret_cast<uint8_t*>(pids), sizeof(pids), Clock::now() + 3s, error)) {
        std::cerr << error << '\n'; return 7;
    }
    std::cout.write(reinterpret_cast<const char*>(pids), sizeof(pids));
    std::cout.flush();
    // Simulate abrupt host termination: Process::~Process never runs.
    std::_Exit(0);
}
#endif

void tests(const std::filesystem::path& executable)
{
    wco::Process process;
    std::string error;
    std::array<uint8_t, 128> sent{}, received{};
    for (size_t i = 0; i < sent.size(); ++i) sent[i] = static_cast<uint8_t>(i * 7);

    require(process.start(executable, {"--child", "echo"}, error), error);
    require(process.write_all(sent.data(), sent.size(), Clock::now() + 3s, error), error);
    require(process.read_all(received.data(), received.size(), Clock::now() + 3s, error), error);
    require(received == sent, "binary payload changed in transit");
    process.stop();
    process.stop();

    // A host is allowed to create an engine on a short-lived thread. Watching
    // that thread's death (e.g. with Linux PDEATHSIG) would incorrectly kill it.
    bool started = false, ready = false;
    std::thread creator([&] {
        started = process.start(executable, {"--child", "ready_echo"}, error);
        uint8_t marker = 0;
        if (started) ready = process.read_all(&marker, 1, Clock::now() + 3s, error) && marker == '!';
    });
    creator.join();
    require(started && ready, "worker did not become ready on creator thread: " + error);
    require(process.write_all(sent.data(), sent.size(), Clock::now() + 3s, error), error);
    require(process.read_all(received.data(), received.size(), Clock::now() + 3s, error), error);
    require(received == sent, "creator thread exit terminated the worker");
    process.stop();

    const std::vector<std::string> expected{"", "a b", "quote\"here", "ends\\", "\\\\\"", "$(shell);&literal"};
    std::vector<std::string> args{"--child", "args"};
    args.insert(args.end(), expected.begin(), expected.end());
    require(process.start(executable, args, error), error);
    for (const auto& expected_arg : expected) {
        uint32_t count = 0;
        require(process.read_all(reinterpret_cast<uint8_t*>(&count), sizeof(count), Clock::now() + 3s, error), error);
        require(count <= 1024, "corrupt argument length");
        std::string actual(count, '\0');
        require(process.read_all(reinterpret_cast<uint8_t*>(actual.data()), actual.size(), Clock::now() + 3s, error), error);
        require(actual == expected_arg, "worker argument quoting changed a value");
    }
    process.stop();

    require(process.start(executable, {"--child", "flood"}, error), error);
    std::vector<uint8_t> flood(2 * 1024 * 1024);
    require(process.read_all(flood.data(), flood.size(), Clock::now() + 5s, error), error);
    for (size_t i = 0; i < flood.size(); ++i) require(flood[i] == i % 251, "large read was corrupted");
    process.stop();

    require(process.start(executable, {"--child", "idle"}, error), error);
    auto before = Clock::now();
    require(!process.read_all(received.data(), 1, before + 100ms, error), "idle read did not time out");
    require(error.find("deadline") != std::string::npos, "read returned the wrong failure: " + error);
    require(Clock::now() - before < 1500ms, "read deadline did not bound the wait");
    process.stop();

    require(process.start(executable, {"--child", "idle"}, error), error);
    std::vector<uint8_t> oversized(8 * 1024 * 1024, 42);
    before = Clock::now();
    require(!process.write_all(oversized.data(), oversized.size(), before + 100ms, error), "blocked write did not time out");
    require(error.find("deadline") != std::string::npos, "write returned the wrong failure: " + error);
    require(Clock::now() - before < 1500ms, "write deadline did not bound the wait");
    before = Clock::now();
    process.stop();
    require(Clock::now() - before < 1500ms, "stop was not bounded");

    require(process.start(executable, {"--child", "exit"}, error), error);
    require(!process.read_all(received.data(), 1, Clock::now() + 3s, error), "EOF was not reported");
    // In particular, this must not deliver SIGPIPE to the embedding process.
    require(!process.write_all(sent.data(), sent.size(), Clock::now() + 3s, error), "write to an exited worker succeeded");
    process.stop();

    require(!process.start(executable.parent_path() / "missing-wco-worker-4351", {}, error), "missing executable started");
    require(!error.empty(), "start failure has no diagnostic");
    require(!process.read_all(received.data(), 1, Clock::now() + 1s, error), "inactive process could be read");
    require(!process.start(executable, {std::string("nul\0arg", 7)}, error), "NUL argument was accepted");
    require(process.start(executable, {"--child", "idle"}, error), error);
    require(!process.read_all(received.data(), 1, Clock::now() - 1ms, error), "expired deadline was ignored");
    process.stop();

#ifdef __linux__
    uint64_t pids[2]{};
    require(process.start(executable, {"--child", "tree"}, error), error);
    require(process.read_all(reinterpret_cast<uint8_t*>(pids), sizeof(pids), Clock::now() + 3s, error), error);
    process.stop();
    require_terminated(pids[0]);
    require_terminated(pids[1]);

    require(process.start(executable, {"--exit-without-cleanup"}, error), error);
    require(process.read_all(reinterpret_cast<uint8_t*>(pids), sizeof(pids), Clock::now() + 3s, error), error);
    require(!process.read_all(received.data(), 1, Clock::now() + 3s, error), "abrupt parent exit did not close stdout");
    process.stop();
    require_terminated(pids[0]);
    require_terminated(pids[1]);
#endif
}
} // namespace

int main(int argc, char** argv)
{
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    try {
        if (argc > 1 && std::string(argv[1]) == "--child") return child(argc, argv);
        const auto executable = executable_path(argv[0]);
#ifdef __linux__
        if (argc > 1 && std::string(argv[1]) == "--exit-without-cleanup") return exit_without_cleanup(executable);
#endif
        tests(executable);
        std::cout << "process tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "process test failed: " << e.what() << '\n';
        return 1;
    }
}
