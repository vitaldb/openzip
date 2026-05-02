#pragma once

#include <Windows.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace openzip {

// Session-scoped single-instance coordinator.
//
// Usage:
//   SingleInstance si;
//   if (!si.TryAcquireOrSend(::GetCommandLineW())) return FALSE;  // we're a follower
//   // we're the leader — drain the queue
//   std::wstring cmdline;
//   while (si.PopNext(cmdline, /*grace=*/200)) {
//       auto cmd = ParseCommandLine(cmdline.c_str());
//       CExtractDialog dlg(cmd);
//       dlg.DoModal();
//   }
//   // PopNext returned false → queue empty AND grace timed out → exit cleanly
//
// Followers connect to the leader's named pipe, send their command line, and exit.
// The leader runs the pipe server on a background thread and processes jobs
// sequentially on the main thread (one CExtractDialog at a time → no disk thrash).
class SingleInstance {
public:
    SingleInstance() = default;
    ~SingleInstance() { Shutdown(); }

    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;

    // Returns true if we are the leader (caller should drain via PopNext).
    // Returns false if a leader already exists; in that case the caller's command
    // line has already been forwarded and the caller should exit immediately.
    bool TryAcquireOrSend(const std::wstring& cmdline);

    // Pop the next queued command line, waiting up to timeout_ms. Returns false
    // if the queue is still empty after the grace window — leader should exit.
    bool PopNext(std::wstring& out, DWORD timeout_ms);

    // Stop the pipe server and release the mutex.
    void Shutdown();

private:
    void ServerLoop();
    bool SendToExistingPipe(const std::wstring& cmdline);
    void Enqueue(std::wstring cmdline);

    std::wstring mutex_name_;
    std::wstring pipe_name_;
    HANDLE mutex_handle_ = nullptr;
    HANDLE shutdown_event_ = nullptr;
    std::thread server_thread_;
    std::atomic<bool> shutting_down_{false};

    std::mutex queue_mtx_;
    std::condition_variable queue_cv_;
    std::deque<std::wstring> queue_;
};

}  // namespace openzip
