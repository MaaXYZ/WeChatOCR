#include "process.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstring>
#include <limits>
#include <thread>

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/stat.h>
extern char** environ;
#endif

namespace wco {
namespace {

using Clock = std::chrono::steady_clock;

int remaining_ms(Process::Deadline deadline)
{
    const auto left = deadline - Clock::now();
    if (left <= Clock::duration::zero()) return 0;
    const auto millis = std::chrono::ceil<std::chrono::milliseconds>(left).count();
    return static_cast<int>(std::min<int64_t>(millis, INT_MAX));
}

#ifdef _WIN32

std::string windows_error(const char* operation, DWORD code = GetLastError())
{
    return std::string(operation) + " failed (Windows error " + std::to_string(code) + ")";
}

void close_handle(HANDLE& handle) noexcept
{
    if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    handle = nullptr;
}

bool utf8_to_wide(const std::string& input, std::wstring& output)
{
    if (input.find('\0') != std::string::npos || input.size() > INT_MAX) return false;
    if (input.empty()) { output.clear(); return true; }
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                                        static_cast<int>(input.size()), nullptr, 0);
    if (!count) return false;
    output.resize(static_cast<size_t>(count));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                              static_cast<int>(input.size()), output.data(), count) == count;
}

// Quote according to the Windows CRT argv convention, including empty strings,
// quotes, and trailing backslashes. No argument is interpreted by a shell.
std::wstring quote_argument(const std::wstring& input)
{
    std::wstring result(1, L'"');
    size_t slashes = 0;
    for (wchar_t c : input) {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'"') {
            result.append(slashes * 2 + 1, L'\\');
            result.push_back(c);
        } else {
            result.append(slashes, L'\\');
            result.push_back(c);
        }
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

bool make_pipe(bool parent_reads, HANDLE& parent, HANDLE& child, std::string& error)
{
    static std::atomic<uint64_t> sequence{0};
    const auto name = L"\\\\.\\pipe\\wco-" + std::to_wstring(GetCurrentProcessId())
        + L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++sequence);
    const DWORD access = (parent_reads ? PIPE_ACCESS_INBOUND : PIPE_ACCESS_OUTBOUND)
        | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE;
    parent = CreateNamedPipeW(name.c_str(), access,
                             PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                             1, 65536, 65536, 0, nullptr);
    if (parent == INVALID_HANDLE_VALUE) {
        parent = nullptr;
        error = windows_error("CreateNamedPipe");
        return false;
    }
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    child = CreateFileW(name.c_str(), parent_reads ? GENERIC_WRITE : GENERIC_READ,
                        0, &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (child == INVALID_HANDLE_VALUE) {
        child = nullptr;
        error = windows_error("CreateFile(pipe)");
        close_handle(parent);
        return false;
    }
    // The local client has already connected, so this never has a pending wait.
    OVERLAPPED connected{};
    connected.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!connected.hEvent) {
        error = windows_error("CreateEvent");
        close_handle(parent);
        close_handle(child);
        return false;
    }
    const BOOL okay = ConnectNamedPipe(parent, &connected);
    const DWORD code = okay ? ERROR_SUCCESS : GetLastError();
    CloseHandle(connected.hEvent);
    if (!okay && code != ERROR_PIPE_CONNECTED) {
        error = windows_error("ConnectNamedPipe", code);
        close_handle(parent);
        close_handle(child);
        return false;
    }
    return true;
}

struct PendingPipeIO {
    OVERLAPPED operation{};
    std::array<uint8_t, 65536> bytes{};
    PendingPipeIO() { operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
    ~PendingPipeIO() { if (operation.hEvent) CloseHandle(operation.hEvent); }
};

bool pipe_io(HANDLE handle, bool reading, uint8_t* data, size_t size,
             Process::Deadline deadline, std::string& error,
             std::unique_ptr<PendingPipeIO>& cancelled)
{
    if (!handle) { error = "worker is not running"; return false; }
    if (cancelled) { error = "worker transport must be restarted after cancelled I/O"; return false; }
    auto pending = std::make_unique<PendingPipeIO>();
    if (!pending->operation.hEvent) { error = windows_error("CreateEvent"); return false; }
    while (size) {
        if (!remaining_ms(deadline)) { error = "I/O deadline exceeded"; return false; }
        const HANDLE event = pending->operation.hEvent;
        pending->operation = {};
        pending->operation.hEvent = event;
        ResetEvent(event);
        DWORD transferred = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(size, 65536));
        if (!reading) std::memcpy(pending->bytes.data(), data, chunk);
        const BOOL completed = reading
            ? ReadFile(handle, pending->bytes.data(), chunk, &transferred, &pending->operation)
            : WriteFile(handle, pending->bytes.data(), chunk, &transferred, &pending->operation);
        if (!completed) {
            const DWORD code = GetLastError();
            if (code != ERROR_IO_PENDING) {
                error = windows_error(reading ? "ReadFile" : "WriteFile", code);
                return false;
            }
            const DWORD waited = WaitForSingleObject(event, static_cast<DWORD>(remaining_ms(deadline)));
            if (waited != WAIT_OBJECT_0) {
                const DWORD wait_error = waited == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT;
                // Cancellation is asynchronous. Retain both OVERLAPPED and its
                // private buffer until stop() observes completion; never wait
                // past the caller's deadline or leave kernel I/O referencing
                // caller-owned memory after this method returns.
                CancelIoEx(handle, &pending->operation);
                cancelled = std::move(pending);
                error = waited == WAIT_TIMEOUT ? "I/O deadline exceeded"
                                              : windows_error("WaitForSingleObject", wait_error);
                return false;
            }
            if (!GetOverlappedResult(handle, &pending->operation, &transferred, FALSE)) {
                error = windows_error(reading ? "ReadFile" : "WriteFile");
                return false;
            }
        }
        if (!transferred) { error = "worker closed its pipe"; return false; }
        if (reading) std::memcpy(data, pending->bytes.data(), transferred);
        data += transferred;
        size -= transferred;
    }
    return true;
}

#else

std::string posix_error(const char* operation, int code = errno)
{
    return std::string(operation) + " failed: " + std::strerror(code);
}

void close_fd(int& fd) noexcept
{
    if (fd >= 0) close(fd);
    fd = -1;
}

bool make_pipe(int (&fds)[2], std::string& error)
{
#ifdef __linux__
    if (pipe2(fds, O_CLOEXEC) != 0) {
#else
    if (pipe(fds) != 0) {
#endif
        error = posix_error("pipe");
        return false;
    }
    // Never let file actions alias stdio if the embedding application closed a
    // standard descriptor before loading the SDK.
    for (int& fd : fds) {
        if (fd <= STDERR_FILENO) {
            const int duplicate = fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
            if (duplicate < 0) {
                error = posix_error("fcntl(F_DUPFD_CLOEXEC)");
                close_fd(fds[0]); close_fd(fds[1]);
                return false;
            }
            close(fd);
            fd = duplicate;
        }
#ifndef __linux__
        if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
            error = posix_error("fcntl(FD_CLOEXEC)");
            close_fd(fds[0]); close_fd(fds[1]);
            return false;
        }
#endif
    }
    return true;
}

bool nonblocking(int fd, std::string& error)
{
    const int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        error = posix_error("fcntl(O_NONBLOCK)");
        return false;
    }
    return true;
}

// A pipe with no reader normally kills the calling process with SIGPIPE.
// Block it only in this calling thread, and consume only a signal generated by
// our write; preserve the application's original mask and pending signals.
class BlockSigpipe {
public:
    bool okay = false;
    bool saw_epipe = false;
    BlockSigpipe()
    {
        sigemptyset(&blocked_);
        sigaddset(&blocked_, SIGPIPE);
        const int code = pthread_sigmask(SIG_BLOCK, &blocked_, &original_);
        if (code) { errno = code; return; }
        sigset_t pending{};
        if (sigpending(&pending) == 0) was_pending_ = sigismember(&pending, SIGPIPE) == 1;
        okay = true;
    }
    ~BlockSigpipe()
    {
        if (!okay) return;
        if (!was_pending_ && saw_epipe) {
            timespec zero{};
            while (sigtimedwait(&blocked_, nullptr, &zero) < 0 && errno == EINTR) {}
        }
        pthread_sigmask(SIG_SETMASK, &original_, nullptr);
    }
private:
    sigset_t blocked_{};
    sigset_t original_{};
    bool was_pending_ = false;
};

bool pipe_io(int fd, bool reading, uint8_t* data, size_t size,
             Process::Deadline deadline, std::string& error, BlockSigpipe* blocked = nullptr)
{
    if (fd < 0) { error = "worker is not running"; return false; }
    while (size) {
        const int timeout = remaining_ms(deadline);
        if (!timeout) { error = "I/O deadline exceeded"; return false; }
        pollfd ready{fd, static_cast<short>(reading ? POLLIN : POLLOUT), 0};
        const int result = poll(&ready, 1, timeout);
        if (result < 0) {
            if (errno == EINTR) continue;
            error = posix_error("poll"); return false;
        }
        if (!result) { error = "I/O deadline exceeded"; return false; }
        if (ready.revents & POLLNVAL) { error = "invalid worker pipe"; return false; }
        const size_t chunk = std::min<size_t>(size, 65536);
        const ssize_t count = reading ? read(fd, data, chunk) : write(fd, data, chunk);
        if (count < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            if (blocked && errno == EPIPE) blocked->saw_epipe = true;
            error = posix_error(reading ? "read" : "write"); return false;
        }
        if (!count) { error = "worker closed its pipe"; return false; }
        data += count;
        size -= static_cast<size_t>(count);
    }
    return true;
}

#endif
} // namespace

struct Process::Impl {
#ifdef _WIN32
    HANDLE input = nullptr;
    HANDLE output = nullptr;
    HANDLE process = nullptr;
    HANDLE job = nullptr;
    std::unique_ptr<PendingPipeIO> cancelled;
#else
    int input = -1;
    int output = -1;
    pid_t pid = -1;
#endif
};

Process::Process() : impl_(std::make_unique<Impl>()) {}
Process::~Process() { stop(); }

bool Process::start(const std::filesystem::path& worker,
                    const std::vector<std::string>& args, std::string& error)
{
    stop();
    error.clear();
    if (worker.empty() || worker.native().find(std::filesystem::path::value_type{})
                              != std::filesystem::path::string_type::npos) {
        error = "invalid worker path";
        return false;
    }
#ifdef _WIN32
    std::wstring command = quote_argument(worker.native());
    for (const auto& arg : args) {
        std::wstring wide;
        if (!utf8_to_wide(arg, wide)) { error = "worker argument is not valid UTF-8"; return false; }
        command += L' ';
        command += quote_argument(wide);
    }
    if (command.size() >= 32767) { error = "worker command line is too long"; return false; }
    HANDLE child_input = nullptr, child_output = nullptr, child_error = nullptr;
    HANDLE thread = nullptr;
    auto cleanup = [&] {
        close_handle(child_input); close_handle(child_output); close_handle(child_error);
        close_handle(thread);
    };
    if (!make_pipe(false, impl_->input, child_input, error)
        || !make_pipe(true, impl_->output, child_output, error)) {
        cleanup(); stop(); return false;
    }
    const HANDLE stderr_handle = GetStdHandle(STD_ERROR_HANDLE);
    if (!stderr_handle || stderr_handle == INVALID_HANDLE_VALUE
        || !DuplicateHandle(GetCurrentProcess(), stderr_handle, GetCurrentProcess(),
                            &child_error, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        child_error = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (child_error == INVALID_HANDLE_VALUE) {
            child_error = nullptr;
            error = windows_error("open worker stderr"); cleanup(); stop(); return false;
        }
    }
    impl_->job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!impl_->job || !SetInformationJobObject(impl_->job, JobObjectExtendedLimitInformation,
                                               &limits, sizeof(limits))) {
        error = windows_error("configure worker job"); cleanup(); stop(); return false;
    }
    SIZE_T attribute_bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 2, 0, &attribute_bytes);
    std::vector<uint8_t> attribute_buffer(attribute_bytes);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_buffer.data());
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 2, 0, &attribute_bytes)) {
        error = windows_error("InitializeProcThreadAttributeList"); cleanup(); stop(); return false;
    }
    HANDLE inherited[]{child_input, child_output, child_error};
    if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   inherited, sizeof(inherited), nullptr, nullptr)) {
        error = windows_error("UpdateProcThreadAttribute");
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        cleanup(); stop(); return false;
    }
    // Windows 10+: membership is installed atomically with process creation.
    // Assigning a suspended child afterward leaves an orphan if the host exits
    // in the interval between CreateProcess and AssignProcessToJobObject.
    HANDLE jobs[]{impl_->job};
    if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
                                   jobs, sizeof(jobs), nullptr, nullptr)) {
        error = windows_error("UpdateProcThreadAttribute(job)");
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        cleanup(); stop(); return false;
    }
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = child_input;
    startup.StartupInfo.hStdOutput = child_output;
    startup.StartupInfo.hStdError = child_error;
    PROCESS_INFORMATION created{};
    const BOOL launched = CreateProcessW(worker.c_str(), command.data(), nullptr, nullptr, TRUE,
                                         EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_NO_WINDOW,
                                         nullptr, nullptr, &startup.StartupInfo, &created);
    const DWORD launch_error = GetLastError();
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    if (!launched) {
        error = windows_error("CreateProcess", launch_error); cleanup(); stop(); return false;
    }
    impl_->process = created.hProcess;
    thread = created.hThread;
    if (ResumeThread(thread) == static_cast<DWORD>(-1)) {
        error = windows_error("ResumeThread"); cleanup(); stop(); return false;
    }
    cleanup();
    return true;
#else
    std::vector<std::string> storage;
    storage.reserve(args.size() + 1);
    storage.push_back(worker.native());
    for (const auto& arg : args) {
        if (arg.find('\0') != std::string::npos) { error = "worker argument contains NUL"; return false; }
        storage.push_back(arg);
    }
    std::vector<char*> argv;
    for (auto& arg : storage) argv.push_back(arg.data());
    argv.push_back(nullptr);
    std::vector<std::string> environment;
    for (char** entry = environ; entry && *entry; ++entry) {
        if (std::strncmp(*entry, "WCO_PARENT_PID=", 15) != 0) environment.emplace_back(*entry);
    }
    environment.push_back("WCO_PARENT_PID=" + std::to_string(getpid()));
    std::vector<char*> envp;
    for (auto& entry : environment) envp.push_back(entry.data());
    envp.push_back(nullptr);
    int input[2]{-1, -1}, output[2]{-1, -1};
    auto cleanup = [&] { close_fd(input[0]); close_fd(input[1]); close_fd(output[0]); close_fd(output[1]); };
    if (!make_pipe(input, error) || !make_pipe(output, error)
        || !nonblocking(input[1], error) || !nonblocking(output[0], error)) {
        cleanup(); return false;
    }
    posix_spawn_file_actions_t actions;
    int code = posix_spawn_file_actions_init(&actions);
    if (code) { error = posix_error("posix_spawn_file_actions_init", code); cleanup(); return false; }
    auto action = [&](int result) { if (!code) code = result; };
    action(posix_spawn_file_actions_adddup2(&actions, input[0], STDIN_FILENO));
    action(posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO));
    for (int fd : {input[0], input[1], output[0], output[1]})
        action(posix_spawn_file_actions_addclose(&actions, fd));
    posix_spawnattr_t attributes;
    const int attr_code = posix_spawnattr_init(&attributes);
    if (!code) code = attr_code;
    if (!attr_code) {
        action(posix_spawnattr_setpgroup(&attributes, 0));
        action(posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP));
    }
    pid_t child = -1;
    if (!code) code = posix_spawn(&child, worker.c_str(), &actions, &attributes, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    if (!attr_code) posix_spawnattr_destroy(&attributes);
    if (code) { error = posix_error("posix_spawn", code); cleanup(); return false; }
    impl_->pid = child;
    impl_->input = input[1]; input[1] = -1;
    impl_->output = output[0]; output[0] = -1;
    cleanup();
    return true;
#endif
}

bool Process::write_all(const uint8_t* data, size_t size, Deadline deadline, std::string& error)
{
    error.clear();
    if (!size) return true;
    if (!data) { error = "null input buffer"; return false; }
#ifndef _WIN32
    BlockSigpipe blocked;
    if (!blocked.okay) { error = posix_error("pthread_sigmask"); return false; }
#endif
#ifdef _WIN32
    return pipe_io(impl_->input, false, const_cast<uint8_t*>(data), size, deadline, error, impl_->cancelled);
#else
    return pipe_io(impl_->input, false, const_cast<uint8_t*>(data), size, deadline, error, &blocked);
#endif
}

bool Process::read_all(uint8_t* data, size_t size, Deadline deadline, std::string& error)
{
    error.clear();
    if (!size) return true;
    if (!data) { error = "null output buffer"; return false; }
#ifdef _WIN32
    return pipe_io(impl_->output, true, data, size, deadline, error, impl_->cancelled);
#else
    return pipe_io(impl_->output, true, data, size, deadline, error);
#endif
}

void Process::stop() noexcept
{
#ifdef _WIN32
    const auto deadline = Clock::now() + std::chrono::seconds(1);
    if (impl_->job) TerminateJobObject(impl_->job, 1);
    close_handle(impl_->input);
    close_handle(impl_->output);
    if (impl_->process) WaitForSingleObject(impl_->process, static_cast<DWORD>(remaining_ms(deadline)));
    if (impl_->cancelled) {
        if (WaitForSingleObject(impl_->cancelled->operation.hEvent,
                               static_cast<DWORD>(remaining_ms(deadline))) == WAIT_OBJECT_0) {
            impl_->cancelled.reset();
        } else {
            // A wedged kernel/driver must not turn stop into an infinite wait or
            // cause a later kernel completion to write to freed storage. Local
            // named-pipe cancellation normally completes immediately; only this
            // exceptional OS failure retains one 64 KiB operation until exit.
            (void)impl_->cancelled.release();
        }
    }
    close_handle(impl_->process);
    close_handle(impl_->job);
#else
    close_fd(impl_->input);
    close_fd(impl_->output);
    if (impl_->pid <= 0) return;
    const pid_t child = impl_->pid;
    impl_->pid = -1;
    // The direct child remains unreaped until here, so its PID/process-group ID
    // cannot be reused by an unrelated process between these operations.
    kill(-child, SIGKILL);
    kill(child, SIGKILL);
    const auto deadline = Clock::now() + std::chrono::seconds(1);
    int status = 0;
    for (;;) {
        const pid_t waited = waitpid(child, &status, WNOHANG);
        if (waited == child || (waited < 0 && errno != EINTR)) break;
        if (Clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
#endif
}

bool configure_worker_parent_death(std::string& error)
{
    error.clear();
#ifdef __linux__
    const char* expected_text = std::getenv("WCO_PARENT_PID");
    if (!expected_text || !*expected_text) { error = "missing worker parent identity"; return false; }
    char* end = nullptr;
    errno = 0;
    const long expected = std::strtol(expected_text, &end, 10);
    if (errno || !end || *end || expected <= 1 || expected > std::numeric_limits<pid_t>::max()) {
        error = "invalid worker parent identity"; return false;
    }
    if (getpgrp() != getpid()) { error = "worker does not own its process group"; return false; }
    if (getppid() != static_cast<pid_t>(expected)) {
        // Covers parent exit before the watchdog is installed, including
        // reparenting to a subreaper.
        kill(0, SIGKILL);
        error = "worker parent exited during startup";
        return false;
    }
    struct stat info{};
    if (fstat(STDIN_FILENO, &info) != 0 || !S_ISFIFO(info.st_mode)) {
        error = "worker stdin is not its private parent pipe";
        return false;
    }
    const int monitor = fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    if (monitor < 0) { error = posix_error("duplicate worker parent pipe"); return false; }
    try {
        std::thread([monitor] {
            // Request bytes do not wake this poll. HUP is reported independently
            // of requested events, even when unread request bytes remain.
            // Unlike PR_SET_PDEATHSIG this observes the owning process, rather
            // than the lifetime of the host thread which called posix_spawn.
            pollfd parent{monitor, 0, 0};
            int result;
            do { result = poll(&parent, 1, -1); } while (result < 0 && errno == EINTR);
            close(monitor);
            kill(0, SIGKILL);
        }).detach();
    } catch (const std::exception& exception) {
        close(monitor);
        error = std::string("start worker parent monitor: ") + exception.what();
        return false;
    }
#endif
    return true;
}

} // namespace wco
