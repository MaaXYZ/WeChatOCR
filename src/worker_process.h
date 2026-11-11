#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace wco {

// Private, single-caller transport. A Process owns the entire worker process
// group/job; stop() also terminates runtime descendants created by the worker.
class Process {
public:
    using Deadline = std::chrono::steady_clock::time_point;

    Process();
    ~Process();
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    Process(Process&&) = delete;
    Process& operator=(Process&&) = delete;

    bool start(const std::filesystem::path& worker,
               const std::vector<std::string>& args, std::string& error);
    bool write_all(const uint8_t* data, size_t size, Deadline deadline,
                   std::string& error);
    bool read_all(uint8_t* data, size_t size, Deadline deadline,
                  std::string& error);
    void stop() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Call at the beginning of the worker, before starting threads or the vendor
// runtime. Process::start supplies the expected parent identity in the worker
// environment. Linux monitors its private stdin pipe for parent disconnection
// and terminates its process group; Windows uses the parent's kill-on-close job.
bool configure_worker_parent_death(std::string& error);

} // namespace wco
