#pragma once

#include "stdafx.h"
#include "resource.h"
#include "CommandLine.h"
#include "core/extractor.h"

#include <atomic>
#include <mutex>
#include <string>

// Custom messages — used by the worker thread to bounce calls onto the UI thread.
#define WM_OPENZIP_PROMPT_PASSWORD (WM_USER + 100)
#define WM_OPENZIP_PROMPT_CONFLICT (WM_USER + 101)
#define WM_OPENZIP_ENTRY_START     (WM_USER + 102)
#define WM_OPENZIP_BYTES           (WM_USER + 103)
#define WM_OPENZIP_COMPLETE        (WM_USER + 104)

class CExtractDialog : public CDialogEx, public openzip::Extractor::ProgressCallback {
    DECLARE_DYNAMIC(CExtractDialog)
public:
    explicit CExtractDialog(const openzip::CommandLine& cmd, CWnd* parent = nullptr);

    enum { IDD = IDD_EXTRACT };

    openzip::Extractor::Result result() const { return result_; }

protected:
    void DoDataExchange(CDataExchange* pDX) override;
    BOOL OnInitDialog() override;
    void OnCancel() override;

    afx_msg LRESULT OnPromptPassword(WPARAM wp, LPARAM lp);
    afx_msg LRESULT OnPromptConflict(WPARAM wp, LPARAM lp);
    afx_msg LRESULT OnEntryStartMsg(WPARAM wp, LPARAM lp);
    afx_msg LRESULT OnBytesMsg(WPARAM wp, LPARAM lp);
    afx_msg LRESULT OnCompleteMsg(WPARAM wp, LPARAM lp);
    DECLARE_MESSAGE_MAP()

    // ProgressCallback (worker-thread side).
    void OnEntryStart(const openzip::Extractor::Entry& entry,
                      size_t index, size_t total) override;
    void OnBytes(uint64_t done, uint64_t total) override;
    std::wstring OnPasswordRequired(const std::wstring& archive_name,
                                    const std::wstring& entry_name,
                                    bool was_wrong) override;
    openzip::Extractor::ConflictAction OnFileConflict(const std::wstring& dest_path) override;
    bool ShouldCancel() override;
    void OnComplete(openzip::Extractor::Result r) override;

private:
    static UINT WorkerProc(LPVOID p);

    openzip::CommandLine cmd_;
    CWinThread* worker_ = nullptr;

    std::atomic<bool> cancel_requested_{false};
    std::atomic<openzip::Extractor::Result> result_{openzip::Extractor::Result::Cancelled};

    // Conflict "remember choice" state. Owned by UI thread; worker reads via SendMessage.
    openzip::Extractor::ConflictAction remembered_conflict_ = openzip::Extractor::ConflictAction::Cancel;
    bool has_remembered_ = false;

    // Throttled progress refresh.
    DWORD last_paint_tick_ = 0;
    uint64_t last_bytes_ = 0;
    DWORD speed_window_start_tick_ = 0;
    uint64_t speed_window_start_bytes_ = 0;

    // Argument bundles passed via WM_OPENZIP_PROMPT_* (allocated on worker stack).
    struct PasswordPromptArgs {
        std::wstring archive;
        std::wstring entry;
        bool was_wrong;
        std::wstring result_password;  // out
        bool cancelled;                // out
    };
    struct ConflictPromptArgs {
        std::wstring dest_path;
        openzip::Extractor::ConflictAction result_action;  // out
        bool remember;                                     // out
    };
    struct EntryStartArgs {
        std::wstring name;
        size_t index;
        size_t total;
    };
};
