// SPDX-License-Identifier: AGPL-3.0-only
#include "runtime_backend.h"
#include "runtime_protocol.h"

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__linux__)
#include <dlfcn.h>
#endif

namespace wco {
namespace {

// Foreign ABI declarations only. All control flow, protocol parsing and request
// management in this implementation is independently authored.
#ifdef _WIN32
using NativeChar = wchar_t;
using Module = HMODULE;
#else
using NativeChar = char;
using Module = void*;
#endif

struct MojoApi {
    void (*initialize)(int, const char* const*) = nullptr;
    void* (*create_environment)() = nullptr;
    void (*callbacks)(void*, int, ...) = nullptr;
    void (*parameters)(void*, int, ...) = nullptr;
    void (*append_switch)(void*, const char*, const NativeChar*) = nullptr;
    void (*start)(void*) = nullptr;
    void (*stop)(void*) = nullptr;
    void (*remove_environment)(void*) = nullptr;
    const void* (*read_payload)(const void*, uint32_t*) = nullptr;
    void (*remove_read)(const void*) = nullptr;
    void* (*create_write)(int, bool, uint32_t) = nullptr;
    void* (*write_payload)(void*, size_t) = nullptr;
    bool (*send)(void*, void*) = nullptr;
    void (*remove_write)(void*) = nullptr;
};

template<class Function>
bool symbol(Module module, const char* name, Function& output)
{
#ifdef _WIN32
    output = reinterpret_cast<Function>(GetProcAddress(module, name));
#elif defined(__linux__)
    output = reinterpret_cast<Function>(dlsym(module, name));
#else
    (void)module;
    (void)name;
    output = nullptr;
#endif
    return output != nullptr;
}

bool load_api(Module module, MojoApi& api, std::string& error)
{
#define WCO_LOAD(member, exported_name) \
    if (!symbol(module, exported_name, api.member)) { \
        error = std::string("Missing runtime export: ") + exported_name; \
        return false; \
    }
    WCO_LOAD(initialize, "InitializeMMMojo")
    WCO_LOAD(create_environment, "CreateMMMojoEnvironment")
    WCO_LOAD(callbacks, "SetMMMojoEnvironmentCallbacks")
    WCO_LOAD(parameters, "SetMMMojoEnvironmentInitParams")
    WCO_LOAD(append_switch, "AppendMMSubProcessSwitchNative")
    WCO_LOAD(start, "StartMMMojoEnvironment")
    WCO_LOAD(stop, "StopMMMojoEnvironment")
    WCO_LOAD(remove_environment, "RemoveMMMojoEnvironment")
    WCO_LOAD(read_payload, "GetMMMojoReadInfoRequest")
    WCO_LOAD(remove_read, "RemoveMMMojoReadInfo")
    WCO_LOAD(create_write, "CreateMMMojoWriteInfo")
    WCO_LOAD(write_payload, "GetMMMojoWriteInfoRequest")
    WCO_LOAD(send, "SendMMMojoWriteInfo")
    WCO_LOAD(remove_write, "RemoveMMMojoWriteInfo")
#undef WCO_LOAD
    return true;
}

struct CallbackState {
    MojoApi api;
    uint32_t version = 0;
    std::mutex mutex;
    std::condition_variable changed;
    bool ready = false;
    bool failed = false;
    bool stopping = false;
    bool in_flight = false;
    bool completed = false;
    uint64_t task_id = 0;
    RuntimeResult result;
    std::string error;

    void fail(const std::string& reason)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!failed)
            error = reason;
        failed = true;
        changed.notify_all();
    }
};

void on_connect(bool connected, void* user) noexcept
{
    if (!connected) {
        try { static_cast<CallbackState*>(user)->fail("Runtime IPC connection failed"); }
        catch (...) {}
    }
}

void on_disconnect(void* user) noexcept
{
    try { static_cast<CallbackState*>(user)->fail("Runtime OCR process disconnected"); }
    catch (...) {}
}

void on_launch_failed(int code, void* user) noexcept
{
    try {
        static_cast<CallbackState*>(user)->fail(
            "Runtime OCR process could not start (code " + std::to_string(code) + ")");
    } catch (...) {}
}

void on_error(const void*, int, void* user) noexcept
{
    try { static_cast<CallbackState*>(user)->fail("Runtime reported an IPC error"); }
    catch (...) {}
}

void on_push(uint32_t request_id, const void* info, void* user) noexcept
{
    auto* state = static_cast<CallbackState*>(user);
    if (!info)
        return;
    // The runtime transfers each received read-info to the callback.
    struct ReadGuard {
        CallbackState* state;
        const void* info;
        ~ReadGuard() { state->api.remove_read(info); }
    } guard { state, info };
    try {
        uint32_t size = 0;
        const void* payload = state->api.read_payload(info, &size);
        protocol::Message message;
        std::string error;
        if (!protocol::decode(state->version, request_id, payload, size, message, error)) {
            state->fail(error);
            return;
        }
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->stopping || state->failed)
            return;
        if (message.kind == protocol::Message::Kind::ready) {
            state->ready = message.supported;
            if (!message.supported) {
                state->failed = true;
                state->error = "Runtime does not support OCR or failed initialization";
            }
            state->changed.notify_all();
        } else if (message.kind == protocol::Message::Kind::result
                   && state->in_flight && message.task_id == state->task_id) {
            state->result = std::move(message.result);
            state->completed = true;
            state->changed.notify_all();
        }
    } catch (...) {
        try { state->fail("Exception while reading a runtime OCR response"); }
        catch (...) {}
    }
}

// OCR does not use these channels; acknowledge and release unexpected messages.
void on_unused_read(uint32_t, const void* info, void* user) noexcept
{
    if (info)
        static_cast<CallbackState*>(user)->api.remove_read(info);
}

std::filesystem::path absolute_path(const std::string& utf8)
{
    return std::filesystem::weakly_canonical(
        std::filesystem::absolute(std::filesystem::u8path(utf8)));
}

} // namespace

struct RuntimeBackend::Impl {
    std::mutex operations;
    Module module = nullptr;
    void* environment = nullptr;
    CallbackState* state = nullptr;
    uint64_t next_task = 2;

    ~Impl()
    {
        if (state) {
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->stopping = true;
                state->changed.notify_all();
            }
            if (environment) {
                state->api.stop(environment);
                state->api.remove_environment(environment);
            }
        }
        // Vendor-created threads can outlive StopMMMojoEnvironment. Keep both
        // their callback state and code resident until this private worker exits.
        // Never FreeLibrary/dlclose or destroy state while those threads exist.
    }
};

RuntimeBackend::RuntimeBackend() : impl_(new Impl) {}
RuntimeBackend::~RuntimeBackend() = default;

bool RuntimeBackend::start(const RuntimeConfig& config, std::string& error)
{
    error.clear();
    std::lock_guard<std::mutex> operation(impl_->operations);
    if (impl_->state) {
        error = "RuntimeBackend can only be started once per worker";
        return false;
    }
#if (defined(_WIN32) && !defined(_WIN64)) || (!defined(_WIN32) && !defined(__linux__))
    (void)config;
    error = "This backend supports Windows x64 and Linux only";
    return false;
#else
    try {
        namespace fs = std::filesystem;
        if (config.runtime_dir.empty() || config.ocr_path.empty()) {
            error = "Runtime directory and OCR entry path must be supplied";
            return false;
        }
        const fs::path runtime = absolute_path(config.runtime_dir);
        const fs::path entry = absolute_path(config.ocr_path);
        if (!fs::is_directory(runtime) || !fs::is_regular_file(entry)) {
            error = "Runtime directory or OCR entry path does not exist";
            return false;
        }
        uint32_t version = config.protocol;
        fs::path executable = entry;
#ifdef _WIN32
        std::wstring extension = entry.extension().native();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](wchar_t c) { return c >= L'A' && c <= L'Z' ? c + (L'a' - L'A') : c; });
        if (version == 0)
            version = extension == L".dll" ? 4 : 3;
        if (version == 4) {
            if (extension != L".dll") {
                error = "WeChat 4 on Windows requires the wxocr DLL entry path";
                return false;
            }
            executable = runtime.parent_path() / L"weixin.exe";
            if (!fs::is_regular_file(executable)) {
                error = "WeChat 4 host weixin.exe is missing from the runtime parent directory";
                return false;
            }
        }
        const fs::path mojo_path = runtime / L"mmmojo_64.dll";
#else
        if (version == 0)
            version = 4;
        if (version != 4) {
            error = "Linux runtime supports the WeChat 4 protocol only";
            return false;
        }
        const fs::path mojo_path = runtime / "libmmmojo.so";
#endif
        if (version != 3 && version != 4) {
            error = "Protocol must be 0 (auto), 3, or 4";
            return false;
        }
        if (!fs::is_regular_file(mojo_path)) {
            error = "The matching MMMojo library is missing from the runtime directory";
            return false;
        }
#ifdef _WIN32
        impl_->module = LoadLibraryExW(mojo_path.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!impl_->module) {
            error = "Could not load mmmojo_64.dll (Windows code "
                + std::to_string(GetLastError()) + ")";
            return false;
        }
#else
        impl_->module = dlopen(mojo_path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!impl_->module) {
            const char* detail = dlerror();
            error = std::string("Could not load libmmmojo.so: ") + (detail ? detail : "unknown loader error");
            return false;
        }
#endif
        MojoApi api;
        if (!load_api(impl_->module, api, error))
            return false;
        // The worker owns this state for its process lifetime (see ~Impl()).
        auto* state = new CallbackState;
        state->api = api;
        state->version = version;
        impl_->state = state;
        api.initialize(0, nullptr);
        impl_->environment = api.create_environment();
        if (!impl_->environment) {
            error = "Could not create the runtime IPC environment";
            return false;
        }
        void* env = impl_->environment;
        api.callbacks(env, 0, static_cast<void*>(state));
        api.callbacks(env, 1, &on_push);
        api.callbacks(env, 2, &on_unused_read);
        api.callbacks(env, 3, &on_unused_read);
        api.callbacks(env, 4, &on_connect);
        api.callbacks(env, 5, &on_disconnect);
        api.callbacks(env, 7, &on_launch_failed);
        api.callbacks(env, 8, &on_error);
        api.parameters(env, 0, true); // Host process; MMMojo launches its child.
        api.parameters(env, 2, executable.c_str());
#ifdef _WIN32
        api.append_switch(env, "no-sandbox", L"");
        api.append_switch(env, "user-lib-dir", runtime.c_str());
        if (version == 4) {
            const auto app_name = entry.stem().native();
            const auto app_path = entry.parent_path().native();
            api.append_switch(env, "type", app_name.c_str());
            api.append_switch(env, "app-path", app_path.c_str());
        }
#else
        api.append_switch(env, "no-sandbox", "");
#endif
        api.start(env);
        std::unique_lock<std::mutex> lock(state->mutex);
        state->changed.wait(lock, [state] { return state->ready || state->failed || state->stopping; });
        if (state->failed || state->stopping) {
            error = state->error.empty() ? "Runtime stopped during startup" : state->error;
            return false;
        }
        return true;
    } catch (const std::exception& exception) {
        error = std::string("Runtime initialization failed: ") + exception.what();
        return false;
    }
#endif
}

bool RuntimeBackend::recognize(const std::string& absolute_image_path,
                               RuntimeResult& result, std::string& error)
{
    result = {};
    error.clear();
    std::lock_guard<std::mutex> operation(impl_->operations);
    auto* state = impl_->state;
    if (!state || !impl_->environment) {
        error = "Runtime has not been started";
        return false;
    }
    try {
        const auto path = std::filesystem::u8path(absolute_image_path);
        if (!path.is_absolute() || !std::filesystem::is_regular_file(path)) {
            error = "OCR input must be an existing absolute image path";
            return false;
        }
        const uint64_t id = impl_->next_task;
        impl_->next_task = id == 0x7fffffff ? 2 : id + 1;
        const auto request = protocol::make_request(state->version, id, absolute_image_path);
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (!state->ready || state->failed || state->stopping) {
                error = state->error.empty() ? "Runtime is not ready" : state->error;
                return false;
            }
            state->task_id = id;
            state->in_flight = true;
            state->completed = false;
            state->result = {};
        }
        const uint32_t request_id = state->version == 3 ? 1 : 10010;
        void* info = state->api.create_write(1, false, request_id); // Push message.
        if (!info) {
            state->fail("Could not allocate an OCR IPC message");
            error = "Could not allocate an OCR IPC message";
            return false;
        }
        void* payload = state->api.write_payload(info, request.size());
        if (!payload) {
            state->api.remove_write(info);
            state->fail("Could not allocate the OCR request payload");
            error = "Could not allocate the OCR request payload";
            return false;
        }
        std::memcpy(payload, request.data(), request.size());
        // Send transfers write-info ownership to MMMojo. Do not release it twice.
        if (!state->api.send(impl_->environment, info))
            state->fail("Runtime rejected the OCR IPC message");
        std::unique_lock<std::mutex> lock(state->mutex);
        state->changed.wait(lock, [state] {
            return state->completed || state->failed || state->stopping;
        });
        state->in_flight = false;
        if (state->failed || state->stopping) {
            error = state->error.empty() ? "Runtime stopped during recognition" : state->error;
            return false;
        }
        result = std::move(state->result);
        return true;
    } catch (const std::exception& exception) {
        error = std::string("Runtime recognition failed: ") + exception.what();
        return false;
    }
}

} // namespace wco
