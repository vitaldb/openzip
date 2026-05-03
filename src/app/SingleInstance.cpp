#include "stdafx.h"
#include "SingleInstance.h"

#include <AclAPI.h>

#include <chrono>

namespace openzip {

namespace {

constexpr DWORD kPipeBufferBytes = 32 * 1024;
constexpr DWORD kSendTimeoutMs = 5000;

std::wstring SessionScopedName(const wchar_t* prefix) {
    DWORD sid = 0;
    ::ProcessIdToSessionId(::GetCurrentProcessId(), &sid);
    wchar_t buf[256];
    ::swprintf_s(buf, L"%s%lu", prefix, sid);
    return buf;
}

}  // namespace

// ---------------------------------------------------------------------------
// UserOnlySecurityAttributes — restricts both kernel objects to the current
// user. Without this, both `CreateMutexW(nullptr, ...)` and
// `CreateNamedPipeW(..., nullptr)` use a default DACL that allows other users
// in the same session to attach (Terminal Services / fast user switching).
// An attacker could otherwise WriteFile() a forged "--password X --extract Y
// --target attacker_dir" command line to the leader's pipe.

UserOnlySecurityAttributes::UserOnlySecurityAttributes() {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return;

    DWORD needed = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    if (needed == 0) { ::CloseHandle(token); return; }

    std::vector<uint8_t> token_buf(needed);
    if (!::GetTokenInformation(token, TokenUser, token_buf.data(), needed, &needed)) {
        ::CloseHandle(token);
        return;
    }
    ::CloseHandle(token);

    TOKEN_USER* tu = reinterpret_cast<TOKEN_USER*>(token_buf.data());
    DWORD sid_len = ::GetLengthSid(tu->User.Sid);
    user_sid_ = ::LocalAlloc(LPTR, sid_len);
    if (!user_sid_) return;
    if (!::CopySid(sid_len, user_sid_, tu->User.Sid)) return;

    DWORD acl_size = sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD) + sid_len;
    acl_ = static_cast<PACL>(::LocalAlloc(LPTR, acl_size));
    if (!acl_) return;
    if (!::InitializeAcl(acl_, acl_size, ACL_REVISION)) return;
    if (!::AddAccessAllowedAce(acl_, ACL_REVISION, GENERIC_ALL, user_sid_)) return;

    if (!::InitializeSecurityDescriptor(&sd_, SECURITY_DESCRIPTOR_REVISION)) return;
    if (!::SetSecurityDescriptorDacl(&sd_, TRUE, acl_, FALSE)) return;

    sa_.nLength = sizeof(sa_);
    sa_.lpSecurityDescriptor = &sd_;
    sa_.bInheritHandle = FALSE;
    valid_ = true;
}

UserOnlySecurityAttributes::~UserOnlySecurityAttributes() {
    if (acl_)      ::LocalFree(acl_);
    if (user_sid_) ::LocalFree(user_sid_);
}

bool SingleInstance::TryAcquireOrSend(const std::wstring& cmdline) {
    mutex_name_ = SessionScopedName(L"Local\\OpenZipApp_SingleInstance_");
    pipe_name_ = SessionScopedName(L"\\\\.\\pipe\\OpenZipApp_");

    mutex_handle_ = ::CreateMutexW(sec_attrs_.get(), FALSE, mutex_name_.c_str());
    if (!mutex_handle_) {
        // Mutex API failed. Run as a standalone leader (no de-duping).
        Enqueue(cmdline);
        return true;
    }

    bool already_exists = (::GetLastError() == ERROR_ALREADY_EXISTS);
    if (already_exists) {
        bool sent = SendToExistingPipe(cmdline);
        ::CloseHandle(mutex_handle_);
        mutex_handle_ = nullptr;
        if (sent) return false;  // forwarded — caller should exit
        // Leader is gone or pipe unreachable. Fall through and process locally.
        Enqueue(cmdline);
        return true;
    }

    // We are the leader. Queue our own command line first.
    Enqueue(cmdline);
    shutdown_event_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    server_thread_ = std::thread([this] { ServerLoop(); });
    return true;
}

bool SingleInstance::PopNext(std::wstring& out, DWORD timeout_ms) {
    std::unique_lock<std::mutex> lk(queue_mtx_);
    if (!queue_cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                            [this] { return !queue_.empty() || shutting_down_.load(); })) {
        return false;
    }
    if (queue_.empty()) return false;
    out = std::move(queue_.front());
    queue_.pop_front();
    return true;
}

void SingleInstance::Shutdown() {
    if (shutting_down_.exchange(true)) return;
    if (shutdown_event_) ::SetEvent(shutdown_event_);
    queue_cv_.notify_all();
    if (server_thread_.joinable()) server_thread_.join();
    if (shutdown_event_) {
        ::CloseHandle(shutdown_event_);
        shutdown_event_ = nullptr;
    }
    if (mutex_handle_) {
        ::CloseHandle(mutex_handle_);
        mutex_handle_ = nullptr;
    }
}

void SingleInstance::Enqueue(std::wstring cmdline) {
    {
        std::lock_guard<std::mutex> lk(queue_mtx_);
        queue_.push_back(std::move(cmdline));
    }
    queue_cv_.notify_one();
}

bool SingleInstance::SendToExistingPipe(const std::wstring& cmdline) {
    if (!::WaitNamedPipeW(pipe_name_.c_str(), kSendTimeoutMs)) {
        return false;
    }
    HANDLE h = ::CreateFileW(pipe_name_.c_str(), GENERIC_WRITE, 0, nullptr,
                             OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    DWORD bytes = static_cast<DWORD>(cmdline.size() * sizeof(wchar_t));
    DWORD written = 0;
    BOOL ok = ::WriteFile(h, cmdline.c_str(), bytes, &written, nullptr);
    ::CloseHandle(h);
    return ok && written == bytes;
}

void SingleInstance::ServerLoop() {
    while (!shutting_down_.load()) {
        HANDLE pipe = ::CreateNamedPipeW(
            pipe_name_.c_str(),
            PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT |
                PIPE_REJECT_REMOTE_CLIENTS,
            PIPE_UNLIMITED_INSTANCES,
            0, kPipeBufferBytes, 0, sec_attrs_.get());
        if (pipe == INVALID_HANDLE_VALUE) break;

        OVERLAPPED ov{};
        ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent) {
            ::CloseHandle(pipe);
            break;
        }

        BOOL ok = ::ConnectNamedPipe(pipe, &ov);
        if (!ok) {
            DWORD err = ::GetLastError();
            if (err == ERROR_IO_PENDING) {
                HANDLE waits[] = {ov.hEvent, shutdown_event_};
                DWORD r = ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                if (r == WAIT_OBJECT_0 + 1) {
                    ::CancelIoEx(pipe, &ov);
                    ::CloseHandle(ov.hEvent);
                    ::CloseHandle(pipe);
                    break;
                }
            } else if (err != ERROR_PIPE_CONNECTED) {
                ::CloseHandle(ov.hEvent);
                ::CloseHandle(pipe);
                continue;
            }
        }
        ::CloseHandle(ov.hEvent);

        wchar_t buf[kPipeBufferBytes / sizeof(wchar_t)];
        DWORD bytes_read = 0;
        if (::ReadFile(pipe, buf, sizeof(buf) - sizeof(wchar_t), &bytes_read, nullptr) &&
            bytes_read > 0) {
            size_t wchars = bytes_read / sizeof(wchar_t);
            buf[wchars] = 0;
            std::wstring msg(buf, wchars);
            while (!msg.empty() && msg.back() == 0) msg.pop_back();
            if (!msg.empty()) Enqueue(std::move(msg));
        }

        ::DisconnectNamedPipe(pipe);
        ::CloseHandle(pipe);
    }
}

}  // namespace openzip
